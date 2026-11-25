#include "agent_service.hpp"

#include <algorithm>
#include <cstring>

#include <agent.pb.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <sdkconfig.h>
#include <swd_host.h>

#include "fw_asset_manager.hpp"
#include "job_controller.hpp"
#include "qjs_binder.hpp"
#include "sidp_swd_backend.hpp"
#include "swd_prog.hpp"

#if CONFIG_SI_JS_ENABLE

namespace
{
    constexpr uint32_t DFSR_ADDR = 0xE000ED30;
    constexpr uint32_t DHCSR_ADDR = 0xE000EDF0;
    constexpr uint32_t DEMCR_ADDR = 0xE000EDFC;
    constexpr uint32_t DEMCR_VC_MASK = 0x7F1; // CORERESET and the fault catches; TRCENA and MON_* stay
    constexpr uint32_t DFSR_CLEAR_ALL = 0x1F;
    constexpr uint32_t HALT_TIMEOUT_MS = 1000;
    constexpr uint32_t SWD_LOCK_TIMEOUT_MS = 1000;
    constexpr size_t MAX_UPLOAD = CONFIG_SI_JS_MAX_SCRIPT_KB * 1024;

    sidp_swd_backend *backend()
    {
        return sidp_swd_backend::instance();
    }

    esp_err_t check_halted()
    {
        uint32_t dhcsr = 0;
        esp_err_t ret = swd_read_word(DHCSR_ADDR, &dhcsr);
        return ret != ESP_OK ? ret : (dhcsr & S_HALT) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    class agent_target final : public qjs_binder::js_target
    {
    public:
        esp_err_t read32(uint32_t addr, uint32_t *val) override { return swd_read_word(addr, val); }
        esp_err_t write32(uint32_t addr, uint32_t val) override { return swd_write_word(addr, val); }

        esp_err_t halt() override
        {
            sidp::stop_detect_t stop{};
            return backend()->halt(HALT_TIMEOUT_MS, stop);
        }

        esp_err_t wait_halt() override
        {
            const int64_t end = esp_timer_get_time() + HALT_TIMEOUT_MS * 1000LL;
            esp_err_t ret;
            while ((ret = check_halted()) == ESP_ERR_INVALID_STATE && esp_timer_get_time() < end) {
                vTaskDelay(1);
            }
            return ret == ESP_ERR_INVALID_STATE ? ESP_ERR_TIMEOUT : ret;
        }

        esp_err_t reset() override // nRST, then it runs: what a job run's reset() does
        {
            sidp::stop_detect_t stop{};
            return backend()->reset(sidp::RESET_NRST, false, stop);
        }

        esp_err_t reinit() override
        {
            swd_off();
            vTaskDelay(1);
            esp_err_t ret = swd_init_debug(pdMS_TO_TICKS(SWD_LOCK_TIMEOUT_MS));
            uint32_t dhcsr = 0;
            ret = ret ?: swd_read_word(DHCSR_ADDR, &dhcsr);
            return ret ?: swd_write_word(DHCSR_ADDR, DBGKEY | C_DEBUGEN | ((dhcsr & S_HALT) ? C_HALT : 0));
        }

        esp_err_t read_mem(uint32_t addr, uint8_t *buf, size_t len) override { return swd_read_memory(addr, buf, len); }

        esp_err_t write_mem(uint32_t addr, const uint8_t *buf, size_t len) override
        {
            return swd_write_memory(addr, const_cast<uint8_t *>(buf), len);
        }

        esp_err_t flash_read(uint32_t addr, uint8_t *buf, size_t len) override { return swd_read_memory(addr, buf, len); }

        esp_err_t flash_write(uint32_t addr, const uint8_t *buf, size_t len) override
        {
            auto *prog = swd_prog::instance();
            esp_err_t ret = prog->init(); // the algorithm loader restarts SWD and resets the target
            return ret ?: prog->write_range(addr, buf, len);
        }

        esp_err_t flash_erase(uint32_t addr, size_t len) override
        {
            auto *prog = swd_prog::instance();
            esp_err_t ret = prog->init();
            return ret ?: prog->erase_range(addr, len);
        }

        esp_err_t call(uint32_t addr, const uint32_t args[4], uint32_t stack_top, uint32_t timeout_ms, qjs_binder::cancel_fn cancelled,
                       uint32_t *r0) override
        {
            return swd_prog::instance()->call(addr, args, stack_top, timeout_ms, cancelled, r0);
        }

        esp_err_t resume() override { return backend()->resume(sidp::RUN_CONTINUE, 0); }

        esp_err_t read_reg(uint32_t id, uint32_t *val) override
        {
            esp_err_t ret = check_halted();
            return ret ?: swd_read_core_register(id, val);
        }

        esp_err_t write_reg(uint32_t id, uint32_t val) override
        {
            esp_err_t ret = check_halted();
            return ret ?: swd_write_core_register(id, val);
        }

        esp_err_t poll_core(qjs_binder::core_state *st) override
        {
            uint32_t dhcsr = 0;
            esp_err_t ret = swd_read_word(DHCSR_ADDR, &dhcsr); // clears S_RESET_ST
            if (ret != ESP_OK) {
                return ret;
            }
            *st = {(dhcsr & S_HALT) != 0, (dhcsr & S_LOCKUP) != 0, (dhcsr & S_RESET_ST) != 0, 0};
            return st->halted ? swd_read_word(DFSR_ADDR, &st->dfsr) : ESP_OK;
        }

        esp_err_t set_vector_catch(uint32_t vc) override
        {
            uint32_t demcr = 0;
            esp_err_t ret = swd_read_word(DEMCR_ADDR, &demcr);
            return ret ?: swd_write_word(DEMCR_ADDR, (demcr & ~DEMCR_VC_MASK) | (vc & DEMCR_VC_MASK));
        }

        esp_err_t clear_stop_cause() override { return swd_write_word(DFSR_ADDR, DFSR_CLEAR_ALL); }
    };

    agent_target target;

    union scratch_msg {
        si_agent_OpenRequest open;
        si_agent_RunRequest run;
        si_agent_EvalRequest eval;
        si_agent_UploadRequest upload;
        si_agent_EventReadRequest event_read;
        si_agent_CaptureReadRequest capture_read;
        si_agent_SessionInfo info;
        si_agent_ScriptResult result;
        si_agent_EventReadResponse events;
        si_agent_CaptureReadResponse capture;
    };

    void *ps_alloc(size_t n)
    {
        return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    bool decode(std::span<const uint8_t> payload, const pb_msgdesc_t *fields, void *msg)
    {
        pb_istream_t stream = pb_istream_from_buffer(payload.data(), payload.size());
        if (!pb_decode(&stream, fields, msg)) {
            ESP_LOGE("agent", "decode: %s", PB_GET_ERROR(&stream));
            return false;
        }
        return true;
    }

    sidp::status_t encode(pb_ostream_t &out, const pb_msgdesc_t *fields, const void *msg)
    {
        if (!pb_encode(&out, fields, msg)) {
            ESP_LOGE("agent", "encode: %s", PB_GET_ERROR(&out));
            return sidp::STATUS_ERROR;
        }
        return sidp::STATUS_OK;
    }
} // namespace

esp_err_t agent_service::init(notify_fn _notify)
{
    notify = _notify;
    frame = static_cast<uint8_t *>(ps_alloc(sidp::MAX_FRAME_SIZE));
    scratch = ps_alloc(sizeof(scratch_msg));
    events = ps_alloc(EVENTS_PER_READ * sizeof(qjs_binder::event_out));
    if (frame == nullptr || scratch == nullptr || events == nullptr) {
        heap_caps_free(frame);
        heap_caps_free(scratch);
        heap_caps_free(events);
        frame = nullptr;
        scratch = nullptr;
        events = nullptr;
        ESP_LOGW(TAG, "init: no PSRAM for agent sessions");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

sidp::status_t agent_service::handle(uint16_t opcode, std::span<const uint8_t> payload, pb_ostream_t &out)
{
    if (scratch == nullptr) {
        return sidp::STATUS_NO_SCRIPTS;
    }
    if (opcode == sidp::OP_AGENT_OPEN) {
        return op_open(payload, out);
    }
    if (!open) {
        return is_agent_opcode(opcode) ? sidp::STATUS_NO_SESSION : sidp::STATUS_UNSUPPORTED;
    }
    memset(scratch, 0, sizeof(scratch_msg));
    switch (opcode) {
    case sidp::OP_AGENT_CLOSE:
        close();
        return sidp::STATUS_OK;
    case sidp::OP_AGENT_INFO:
        return op_info(out);
    case sidp::OP_SCRIPT_RUN:
        return op_run(payload, out);
    case sidp::OP_SCRIPT_EVAL:
        return op_eval(payload, out);
    case sidp::OP_SCRIPT_STOP:
        qjs_binder::js_host::instance()->session_stop(target);
        return sidp::STATUS_OK;
    case sidp::OP_SCRIPT_UPLOAD:
        return op_upload(payload);
    case sidp::OP_EVENT_READ:
        return op_event_read(payload, out);
    case sidp::OP_CAPTURE_READ:
        return op_capture_read(payload, out);
    default:
        return sidp::STATUS_UNSUPPORTED;
    }
}

sidp::status_t agent_service::op_open(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    auto *js = qjs_binder::js_host::instance();
    if (js->engine().version[0] == '\0') {
        return sidp::STATUS_NO_SCRIPTS;
    }
    if (open) {
        return sidp::STATUS_BUSY;
    }
    auto &req = *static_cast<si_agent_OpenRequest *>(scratch);
    req = si_agent_OpenRequest_init_zero;
    if (!decode(payload, si_agent_OpenRequest_fields, &req)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    if (req.session_end < si_agent_SessionEnd_SESSION_END_AS_IS || req.session_end > si_agent_SessionEnd_SESSION_END_RESET) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }

    auto *asset = fw_asset_manager::instance();
    if (!asset->has_job() || asset->config().family != si::config::target_family::swd_cortex_m || !asset->config().has_algo) {
        return sidp::STATUS_NEEDS_CORTEX_M;
    }

    sidp::attach_params_t params{req.connect_under_reset, req.connect_under_reset};
    sidp::attach_info_t info{};
    esp_err_t ret = backend()->attach(params, info);
    if (ret == ESP_ERR_INVALID_STATE || ret == job_controller::ERR_RUNNING) {
        return sidp::STATUS_BUSY; // GDB or a job run has it; refused before any SWD traffic
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "open: attach failed: %s", esp_err_to_name(ret));
        return sidp::STATUS_SWD_ERROR;
    }

    const bool perms[3] = {req.has_permissions && req.permissions.write, req.has_permissions && req.permissions.flash,
                           req.has_permissions && req.permissions.reset};
    qjs_binder::session_config cfg = {{perms[0], perms[1], perms[2]}, info.profile != sidp::PROFILE_ARMV6M, notify};
    ret = js->begin_session(cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "open: no script session: %s", esp_err_to_name(ret));
        if (backend()->detach(sidp::DETACH_KEEP_HALTED) != ESP_OK) {
            backend()->abandon();
        }
        return ret == ESP_ERR_INVALID_STATE ? sidp::STATUS_BUSY : sidp::STATUS_NO_SCRIPTS;
    }

    open = true;
    session_end = req.session_end;
    perm_write = perms[0];
    perm_flash = perms[1];
    perm_reset = perms[2];
    cpu_id = info.cpu_id;
    profile = info.profile;
    fpu = (info.capabilities & sidp::CAP_FPU) != 0;
    ESP_LOGI(TAG, "session open: write %d, flash %d, reset %d, end %u", perm_write, perm_flash, perm_reset, session_end);
    return op_info(out);
}

sidp::status_t agent_service::op_info(pb_ostream_t &out)
{
    auto *js = qjs_binder::js_host::instance();
    auto &info = *static_cast<si_agent_SessionInfo *>(scratch);
    info = si_agent_SessionInfo_init_zero;
    info.has_permissions = true;
    info.permissions = {perm_write, perm_flash, perm_reset};
    info.session_end = static_cast<si_agent_SessionEnd>(session_end);
    const auto engine = js->engine();
    strlcpy(info.js_version, engine.version, sizeof(info.js_version));
    info.arena_kb = engine.arena_kb;
    info.cpu_id = cpu_id;
    info.profile = profile;
    info.fpu = fpu;
    js->read_events(0, nullptr, 0, &info.newest_event_seq, nullptr);
    return encode(out, si_agent_SessionInfo_fields, &info);
}

sidp::status_t agent_service::script_result(esp_err_t ret, pb_ostream_t &out)
{
    switch (ret) {
    case ESP_OK:
        break;
    case ESP_ERR_INVALID_SIZE:
        return sidp::STATUS_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE:
        return sidp::STATUS_NO_SESSION;
    default:
        return sidp::STATUS_NO_SCRIPTS;
    }
    const auto r = qjs_binder::js_host::instance()->last_script_result();
    auto &res = *static_cast<si_agent_ScriptResult *>(scratch);
    res = si_agent_ScriptResult_init_zero;
    res.outcome = static_cast<si_agent_ScriptResult_Outcome>(r.outcome);
    res.truncated = r.truncated;
    strlcpy(res.value, r.value, sizeof(res.value));
    strlcpy(res.error, r.error, sizeof(res.error));
    strlcpy(res.stack, r.stack, sizeof(res.stack));
    return encode(out, si_agent_ScriptResult_fields, &res);
}

sidp::status_t agent_service::op_run(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    auto &req = *static_cast<si_agent_RunRequest *>(scratch);
    req = si_agent_RunRequest_init_zero;
    if (!decode(payload, si_agent_RunRequest_fields, &req) || (req.staged && req.bytecode.size != 0)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    auto *js = qjs_binder::js_host::instance();
    esp_err_t ret;
    if (req.staged) {
        if (upload == nullptr || upload_len != upload_total) {
            return sidp::STATUS_INVALID_ARGUMENT;
        }
        ret = js->session_run(upload, upload_len, req.timeout_ms, target); // copies it
        drop_upload();
    } else {
        ret = js->session_run(req.bytecode.bytes, req.bytecode.size, req.timeout_ms, target);
    }
    return script_result(ret, out);
}

sidp::status_t agent_service::op_eval(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    auto &req = *static_cast<si_agent_EvalRequest *>(scratch);
    req = si_agent_EvalRequest_init_zero;
    if (!decode(payload, si_agent_EvalRequest_fields, &req)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    esp_err_t ret = qjs_binder::js_host::instance()->session_eval(req.bytecode.bytes, req.bytecode.size, req.timeout_ms, target);
    return script_result(ret, out);
}

sidp::status_t agent_service::op_upload(std::span<const uint8_t> payload)
{
    auto &req = *static_cast<si_agent_UploadRequest *>(scratch);
    req = si_agent_UploadRequest_init_zero;
    if (!decode(payload, si_agent_UploadRequest_fields, &req)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    if (req.offset == 0) {
        drop_upload();
        if (req.total == 0 || req.total > MAX_UPLOAD) {
            return sidp::STATUS_INVALID_ARGUMENT;
        }
        upload = static_cast<uint8_t *>(heap_caps_malloc(req.total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (upload == nullptr) {
            return sidp::STATUS_ERROR;
        }
        upload_total = req.total;
    }
    if (upload == nullptr || req.offset != upload_len || req.total != upload_total || req.data.size > upload_total - upload_len) {
        drop_upload();
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    memcpy(upload + upload_len, req.data.bytes, req.data.size);
    upload_len += req.data.size;
    return sidp::STATUS_OK;
}

void agent_service::drop_upload()
{
    heap_caps_free(upload);
    upload = nullptr;
    upload_len = 0;
    upload_total = 0;
}

sidp::status_t agent_service::op_event_read(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    auto &req = *static_cast<si_agent_EventReadRequest *>(scratch);
    req = si_agent_EventReadRequest_init_zero;
    if (!decode(payload, si_agent_EventReadRequest_fields, &req)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    const uint32_t after = req.after_seq;
    auto *evs = static_cast<qjs_binder::event_out *>(events);
    auto &res = *static_cast<si_agent_EventReadResponse *>(scratch);
    res = si_agent_EventReadResponse_init_zero;
    const size_t n = qjs_binder::js_host::instance()->read_events(after, evs, EVENTS_PER_READ, &res.newest_seq, &res.dropped);
    static_assert(EVENTS_PER_READ <= sizeof(res.events) / sizeof(res.events[0]));
    static_assert(sizeof(res.events[0].kind) == sizeof(evs[0].kind) && sizeof(res.events[0].json) == sizeof(evs[0].json));
    for (size_t i = 0; i < n; i++) {
        auto &e = res.events[i];
        e.seq = evs[i].seq;
        e.uptime_us = evs[i].uptime_us;
        e.capture_id = evs[i].capture_id;
        memcpy(e.kind, evs[i].kind, sizeof(e.kind));
        memcpy(e.json, evs[i].json, sizeof(e.json));
    }
    res.events_count = n;
    return encode(out, si_agent_EventReadResponse_fields, &res);
}

sidp::status_t agent_service::op_capture_read(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    auto &req = *static_cast<si_agent_CaptureReadRequest *>(scratch);
    req = si_agent_CaptureReadRequest_init_zero;
    if (!decode(payload, si_agent_CaptureReadRequest_fields, &req)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    const uint32_t id = req.capture_id, offset = req.offset;
    auto &res = *static_cast<si_agent_CaptureReadResponse *>(scratch);
    res = si_agent_CaptureReadResponse_init_zero;
    size_t len = 0;
    esp_err_t ret = qjs_binder::js_host::instance()->read_capture(id, offset, res.data.bytes, sizeof(res.data.bytes), &len, &res.total);
    if (ret != ESP_OK) {
        return sidp::STATUS_INVALID_ARGUMENT; // gone: a newer capture took its slot
    }
    res.data.size = len;
    return encode(out, si_agent_CaptureReadResponse_fields, &res);
}

void agent_service::service()
{
    if (open) {
        qjs_binder::js_host::instance()->session_service(target, 0);
    }
}

void agent_service::close()
{
    if (!open) {
        return;
    }
    open = false;
    drop_upload();
    qjs_binder::js_host::instance()->end_session(target); // watches and vector catch first
    esp_err_t ret = ESP_OK;
    sidp::detach_action_t action = sidp::DETACH_KEEP_HALTED; // leaves a running target running
    if (session_end == si_agent_SessionEnd_SESSION_END_RESUME) {
        action = sidp::DETACH_RESUME;
    } else if (session_end == si_agent_SessionEnd_SESSION_END_RESET) {
        sidp::stop_detect_t stop{};
        ret = backend()->reset(sidp::RESET_NRST, false, stop);
    }
    ret = ret ?: backend()->detach(action);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "close: target not answering (%s), abandoning it", esp_err_to_name(ret));
        backend()->abandon();
    }
    ESP_LOGI(TAG, "session closed");
}

#else // no scripts in this firmware

esp_err_t agent_service::init(notify_fn) { return ESP_OK; }
sidp::status_t agent_service::handle(uint16_t, std::span<const uint8_t>, pb_ostream_t &) { return sidp::STATUS_UNSUPPORTED; }
void agent_service::service() {}
void agent_service::close() {}

#endif

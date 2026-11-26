#include "agent_service.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <agent.pb.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <psa/crypto.h>
#include <sdkconfig.h>
#include <swd_host.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_asset_manager.hpp"
#include "incident_store.hpp"
#include "job_controller.hpp"
#include "prog_log.hpp"
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
    constexpr char TRAP_DIR[] = "/data/trap";
    constexpr char TRAP_CONFIG[] = "/data/trap/config.bin";
    constexpr char TRAP_MODULE[] = "/data/trap/module.bin";
    constexpr char NVS_NAMESPACE[] = "si_agent";
    constexpr char NVS_BOOTS_KEY[] = "boots";
    constexpr uint32_t TRAP_TOP_LEVEL_MS = 5000;
    constexpr uint32_t BOOT_DELAY_MS = 2000;
    constexpr uint32_t HOLDOFF_MS = 3000;
    constexpr uint32_t BUSY_RETRY_MS = 1000;
    constexpr uint32_t NO_JOB_RETRY_MS = 10000;
    constexpr uint32_t BACKOFF_MIN_MS = 1000;
    constexpr uint32_t BACKOFF_MAX_MS = 60000;
    constexpr uint32_t LOST_RESTART_MS = 5000;
    constexpr uint32_t STABLE_MS = 60000;
    constexpr uint32_t MAX_FAILURES = 5;

    int64_t now_us()
    {
        return esp_timer_get_time();
    }

    bool write_whole(const char *path, const void *data, size_t len)
    {
        char tmp[40];
        snprintf(tmp, sizeof(tmp), "%s.new", path);
        FILE *f = fopen(tmp, "wb");
        bool ok = f != nullptr && fwrite(data, 1, len, f) == len;
        ok = f != nullptr && fclose(f) == 0 && ok;
        if (ok) {
            unlink(path);
            ok = rename(tmp, path) == 0;
        }
        if (!ok) {
            unlink(tmp);
        }
        return ok;
    }

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

        esp_err_t flash_check(flash_op op, uint32_t addr, size_t len, char *why, size_t why_len) override
        {
            auto access = op == flash_op::read    ? swd_prog::flash_access::read
                          : op == flash_op::write ? swd_prog::flash_access::write
                                                  : swd_prog::flash_access::erase;
            return swd_prog::check_flash_args(access, addr, len, why, why_len);
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
            if (ret == ESP_OK && swd_prog::is_fp_reg(id) && !has_fpu) {
                ret = ESP_ERR_NOT_SUPPORTED; // DCRSR would answer with junk
            }
            return ret ?: swd_read_core_register(id, val);
        }

        esp_err_t write_reg(uint32_t id, uint32_t val) override
        {
            esp_err_t ret = check_halted();
            if (ret == ESP_OK && swd_prog::is_fp_reg(id) && !has_fpu) {
                ret = ESP_ERR_NOT_SUPPORTED; // DCRSR would answer with junk
            }
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

        bool has_fpu = false; // from attach (CAP_FPU), so FP register access costs no extra SWD read
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
        si_agent_ArmRequest arm;
        si_agent_TrapStatus status;
        si_agent_IncidentListRequest inc_list_req;
        si_agent_IncidentListResponse inc_list;
        si_agent_IncidentReadRequest inc_read_req;
        si_agent_IncidentReadResponse inc_read;
        si_agent_IncidentDeleteRequest inc_delete;
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
    inc_events = static_cast<char *>(ps_alloc(INCIDENT_EVENTS_MAX));
    if (frame == nullptr || scratch == nullptr || events == nullptr || inc_events == nullptr) {
        heap_caps_free(frame);
        heap_caps_free(scratch);
        heap_caps_free(events);
        heap_caps_free(inc_events);
        frame = nullptr;
        scratch = nullptr;
        events = nullptr;
        inc_events = nullptr;
        ESP_LOGW(TAG, "init: no PSRAM for agent sessions");
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, NVS_BOOTS_KEY, &boot_count);
        nvs_set_u32(nvs, NVS_BOOTS_KEY, ++boot_count);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    incident_store::instance()->init();
    sidp_swd_backend::instance()->set_yield_hook(on_attach_wanted);
    load_trap();
    return ESP_OK;
}

sidp::status_t agent_service::handle(uint16_t opcode, std::span<const uint8_t> payload, pb_ostream_t &out)
{
    if (scratch == nullptr) {
        return sidp::STATUS_NO_SCRIPTS;
    }
    memset(scratch, 0, sizeof(scratch_msg));
    switch (opcode) {
    case sidp::OP_AGENT_OPEN:
        return op_open(payload, out);
    case sidp::OP_SCRIPT_UPLOAD:
        return op_upload(payload);
    case sidp::OP_TRAP_ARM:
        return op_trap_arm(payload, out);
    case sidp::OP_TRAP_DISARM:
        return op_trap_disarm();
    case sidp::OP_TRAP_STATUS:
        return op_trap_status(out);
    case sidp::OP_INCIDENT_LIST:
        return op_incident_list(payload, out);
    case sidp::OP_INCIDENT_READ:
        return op_incident_read(payload, out);
    case sidp::OP_INCIDENT_DELETE:
        return op_incident_delete(payload);
    case sidp::OP_EVENT_READ:
        return open || trap.armed ? op_event_read(payload, out) : sidp::STATUS_NO_SESSION;
    case sidp::OP_CAPTURE_READ:
        return open || trap.armed ? op_capture_read(payload, out) : sidp::STATUS_NO_SESSION;
    default:
        break;
    }
    if (!open) {
        return is_agent_opcode(opcode) ? sidp::STATUS_NO_SESSION : sidp::STATUS_UNSUPPORTED;
    }
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
    default:
        return sidp::STATUS_UNSUPPORTED;
    }
}

sidp::status_t agent_service::claim(const si_agent_Permissions &perms, bool connect_under_reset, bool for_trap)
{
    auto *js = qjs_binder::js_host::instance();
    if (js->engine().version[0] == '\0') {
        return sidp::STATUS_NO_SCRIPTS;
    }
    auto *asset = fw_asset_manager::instance();
    if (!asset->has_job() || asset->config().family != si::config::target_family::swd_cortex_m || !asset->config().has_algo) {
        return sidp::STATUS_NEEDS_CORTEX_M;
    }

    sidp::attach_params_t params{connect_under_reset, connect_under_reset};
    sidp::attach_info_t info{};
    esp_err_t ret = backend()->attach(params, info);
    if (ret == ESP_ERR_INVALID_STATE || ret == job_controller::ERR_RUNNING) {
        return sidp::STATUS_BUSY; // GDB or a job run has it; refused before any SWD traffic
    }
    if (ret == ESP_ERR_NOT_FOUND) {
        return sidp::STATUS_NEEDS_CORTEX_M; // the job went away since the check above
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "claim: attach failed: %s", esp_err_to_name(ret));
        return sidp::STATUS_SWD_ERROR;
    }

    static const qjs_binder::incident_hooks hooks = {on_incident_open, on_incident_add};
    // A session's agent is there to resume a throttled watch; a trap lets it run after a while.
    uint32_t throttle_hold_ms = 0;
    if (for_trap) {
        throttle_hold_ms = trap.cfg.throttle_hold_ms == 0            ? DEFAULT_THROTTLE_HOLD_MS
                           : trap.cfg.throttle_hold_ms == UINT32_MAX ? 0
                                                                     : trap.cfg.throttle_hold_ms;
    }
    qjs_binder::session_config cfg = {{perms.write, perms.flash, perms.reset}, info.profile != sidp::PROFILE_ARMV6M, notify,
                                      for_trap ? &hooks : nullptr, throttle_hold_ms};
    ret = js->begin_session(cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "claim: no script session: %s", esp_err_to_name(ret));
        if (backend()->detach(sidp::DETACH_KEEP_HALTED) != ESP_OK) {
            backend()->abandon();
        }
        return ret == ESP_ERR_INVALID_STATE ? sidp::STATUS_BUSY : sidp::STATUS_NO_SCRIPTS;
    }
    cpu_id = info.cpu_id;
    profile = info.profile;
    fpu = (info.capabilities & sidp::CAP_FPU) != 0;
    target.has_fpu = fpu; // probed once by attach
    return sidp::STATUS_OK;
}

void agent_service::release(uint8_t end)
{
    qjs_binder::js_host::instance()->end_session(target); // watches and vector catch first
    esp_err_t ret = ESP_OK;
    sidp::detach_action_t action = sidp::DETACH_KEEP_HALTED; // leaves a running target running
    if (end == si_agent_SessionEnd_SESSION_END_RESUME) {
        action = sidp::DETACH_RESUME;
    } else if (end == si_agent_SessionEnd_SESSION_END_RESET) {
        // Caught at the reset vector, then let go with debug off (DETACH_RESUME clears
        // C_DEBUGEN): no firmware runs with debug on, where a BKPT would halt it for good.
        sidp::stop_detect_t stop{};
        ret = backend()->reset(sidp::RESET_NRST, true, stop);
        if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "release: the reset wasn't caught at its vector; letting the target go anyway");
            ret = ESP_OK;
        }
        action = sidp::DETACH_RESUME;
    }
    ret = ret ?: backend()->detach(action);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "release: target not answering (%s), abandoning it", esp_err_to_name(ret));
        backend()->abandon();
    }
}

sidp::status_t agent_service::op_open(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    if (open) {
        return sidp::STATUS_BUSY;
    }
    if (trap.armed) {
        return sidp::STATUS_TRAP_ARMED;
    }
    auto &req = *static_cast<si_agent_OpenRequest *>(scratch);
    req = si_agent_OpenRequest_init_zero;
    if (!decode(payload, si_agent_OpenRequest_fields, &req)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    if (req.session_end < si_agent_SessionEnd_SESSION_END_AS_IS || req.session_end > si_agent_SessionEnd_SESSION_END_RESET) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    const si_agent_Permissions perms = req.has_permissions ? req.permissions : si_agent_Permissions{};
    const auto status = claim(perms, req.connect_under_reset, false);
    if (status != sidp::STATUS_OK) {
        return status;
    }

    open = true;
    session_end = req.session_end;
    perm_write = perms.write;
    perm_flash = perms.flash;
    perm_reset = perms.reset;
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
    if (open || trap_holds) {
        qjs_binder::js_host::instance()->session_service(target, 0);
    }
    trap_service();
}

void agent_service::idle(uint32_t ms)
{
    const int64_t end = now_us() + ms * 1000LL;
    for (;;) {
        service();
        const int64_t left_ms = (end - now_us()) / 1000;
        if (left_ms <= 0) {
            return;
        }
        if (trap_holds) {
            qjs_binder::js_host::instance()->session_service(target, std::min<int64_t>(POLL_MS, left_ms));
        } else {
            vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(left_ms)));
        }
    }
}

void agent_service::close()
{
    if (!open) {
        return;
    }
    open = false;
    drop_upload();
    release(session_end);
    ESP_LOGI(TAG, "session closed");
}

void agent_service::connection_closed()
{
    close();
    drop_upload();
}

void agent_service::load_trap()
{
    FILE *f = fopen(TRAP_CONFIG, "rb");
    if (f == nullptr) {
        return;
    }
    uint8_t buf[8 + si_agent_TrapConfig_size + 32];
    const size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    const uint32_t pb_len = len >= 8 ? buf[4] | buf[5] << 8 : 0;
    si_agent_TrapConfig cfg = si_agent_TrapConfig_init_zero;
    if (len < 8 || memcmp(buf, "SITR", 4) != 0 || pb_len > si_agent_TrapConfig_size || len != 8 + pb_len + 32 ||
        !decode({buf + 8, pb_len}, si_agent_TrapConfig_fields, &cfg)) {
        ESP_LOGE(TAG, "load_trap: %s is damaged, trap dropped", TRAP_CONFIG);
        erase_trap();
        return;
    }
    trap = {};
    trap.armed = true;
    trap.cfg = cfg;
    memcpy(trap.sha256, buf + 8 + pb_len, 32);
    trap_retry("starting", BOOT_DELAY_MS);
    ESP_LOGI(TAG, "trap '%s' armed, starting", trap.cfg.name);
}

esp_err_t agent_service::save_trap(const uint8_t *module, size_t len)
{
    uint8_t buf[8 + si_agent_TrapConfig_size + 32] = {'S', 'I', 'T', 'R'};
    pb_ostream_t out = pb_ostream_from_buffer(buf + 8, si_agent_TrapConfig_size);
    if (!pb_encode(&out, si_agent_TrapConfig_fields, &trap.cfg)) {
        return ESP_FAIL;
    }
    buf[4] = out.bytes_written;
    buf[5] = out.bytes_written >> 8;
    memcpy(buf + 8 + out.bytes_written, trap.sha256, 32);
    if (mkdir(TRAP_DIR, 0755) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    bool ok = write_whole(TRAP_MODULE, module, len) && write_whole(TRAP_CONFIG, buf, 8 + out.bytes_written + 32);
    if (!ok) {
        erase_trap();
    }
    return ok ? ESP_OK : ESP_FAIL;
}

void agent_service::erase_trap()
{
    unlink(TRAP_CONFIG);
    unlink(TRAP_MODULE);
}

uint8_t *agent_service::load_module(size_t *len)
{
    struct stat st = {};
    if (stat(TRAP_MODULE, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > MAX_UPLOAD) {
        return nullptr;
    }
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    FILE *f = buf != nullptr ? fopen(TRAP_MODULE, "rb") : nullptr;
    bool ok = f != nullptr && fread(buf, 1, st.st_size, f) == (size_t)st.st_size;
    if (f != nullptr) {
        fclose(f);
    }
    uint8_t sha[32];
    size_t sha_len = 0;
    ok = ok && psa_hash_compute(PSA_ALG_SHA_256, buf, st.st_size, sha, sizeof(sha), &sha_len) == PSA_SUCCESS &&
         memcmp(sha, trap.sha256, sizeof(sha)) == 0;
    if (!ok) {
        heap_caps_free(buf);
        return nullptr;
    }
    *len = st.st_size;
    return buf;
}

void agent_service::drop_trap_hold()
{
    trap.starting = false;
    if (trap_holds) {
        release(si_agent_SessionEnd_SESSION_END_AS_IS);
        trap_holds = false;
        trap_yields = false;
    }
}

void agent_service::trap_retry(const char *why, uint32_t ms)
{
    drop_trap_hold();
    trap.state = si_agent_TrapState_TRAP_STATE_WAITING;
    strlcpy(trap.note, why, sizeof(trap.note));
    trap.retry_us = now_us() + ms * 1000LL;
}

void agent_service::trap_fail(const char *why)
{
    drop_trap_hold();
    trap.state = si_agent_TrapState_TRAP_STATE_FAILED;
    strlcpy(trap.note, why, sizeof(trap.note));
    ESP_LOGE(TAG, "trap '%s' failed: %s", trap.cfg.name, why);
}

void agent_service::trap_pause(const char *why)
{
    drop_trap_hold();
    trap.state = si_agent_TrapState_TRAP_STATE_PAUSED;
    strlcpy(trap.note, why, sizeof(trap.note));
    trap.retry_us = now_us() + HOLDOFF_MS * 1000LL;
    ESP_LOGI(TAG, "trap '%s' paused: %s", trap.cfg.name, why);
}

void agent_service::trap_disarm()
{
    drop_trap_hold();
    erase_trap();
    if (trap.armed) {
        ESP_LOGI(TAG, "trap '%s' disarmed", trap.cfg.name);
    }
    trap = {};
}

void agent_service::trap_start()
{
    run_wanted = false;
    auto *js = qjs_binder::js_host::instance();
    if (js->refuses(trap.sha256)) {
        trap_fail("the probe crashed while this trap ran; restart the probe to try it again");
        return;
    }
    size_t len = 0;
    uint8_t *module = load_module(&len);
    if (module == nullptr) {
        trap_fail("the trap's module file is missing or damaged");
        return;
    }
    const si_agent_Permissions perms = trap.cfg.has_permissions ? trap.cfg.permissions : si_agent_Permissions{};
    const auto status = claim(perms, trap.cfg.connect_under_reset, true);
    if (status != sidp::STATUS_OK) {
        heap_caps_free(module);
        switch (status) {
        case sidp::STATUS_BUSY:
            trap_retry("the target is busy: a debugger or a programming run has it", BUSY_RETRY_MS);
            return;
        case sidp::STATUS_NEEDS_CORTEX_M:
            trap_retry("no Cortex-M job on the device", NO_JOB_RETRY_MS);
            return;
        case sidp::STATUS_SWD_ERROR:
            trap.backoff_ms = std::clamp(trap.backoff_ms * 2, BACKOFF_MIN_MS, BACKOFF_MAX_MS);
            trap_retry("the target doesn't answer", trap.backoff_ms);
            return;
        default:
            trap_fail("scripts can't run, see the device log");
            return;
        }
    }
    trap_holds = true;
    trap_yields = trap.cfg.mode == si_agent_TrapMode_TRAP_MODE_YIELD;

    // Don't wait here for the top level: this is the SIDP service task, and a
    // trap's top level usually never ends, so every start would leave GDB and
    // every other request unanswered for TRAP_TOP_LEVEL_MS. trap_service()
    // checks the outcome between requests (trap_check_start).
    uint32_t token = 0;
    esp_err_t ret = js->session_start(module, len, &token);
    heap_caps_free(module);
    if (ret != ESP_OK) {
        char why[sizeof(trap.note)];
        snprintf(why, sizeof(why), "module: %s", esp_err_to_name(ret));
        if (++trap.failures >= MAX_FAILURES) {
            trap_fail(why);
        } else {
            trap.backoff_ms = std::clamp(trap.backoff_ms * 2, BACKOFF_MIN_MS, BACKOFF_MAX_MS);
            trap_retry(why, trap.backoff_ms);
        }
        return;
    }
    trap.starting = true;
    trap.start_token = token;
    trap.start_until_us = now_us() + TRAP_TOP_LEVEL_MS * 1000LL;
    strlcpy(trap.note, "starting the module", sizeof(trap.note));
}

void agent_service::trap_check_start()
{
    auto *js = qjs_binder::js_host::instance();
    if (!js->session_poll_result(trap.start_token, now_us() >= trap.start_until_us, target)) {
        return;
    }
    trap.starting = false;
    const auto r = js->last_script_result();
    if (r.outcome == qjs_binder::script_outcome::error || r.outcome == qjs_binder::script_outcome::stopped) {
        char why[sizeof(trap.note)];
        snprintf(why, sizeof(why), "module: %s", r.error);
        if (++trap.failures >= MAX_FAILURES) {
            trap_fail(why);
        } else {
            trap.backoff_ms = std::clamp(trap.backoff_ms * 2, BACKOFF_MIN_MS, BACKOFF_MAX_MS);
            trap_retry(why, trap.backoff_ms);
        }
        return;
    }
    trap.state = si_agent_TrapState_TRAP_STATE_RUNNING;
    trap.note[0] = '\0';
    trap.starts++;
    trap.backoff_ms = 0;
    trap.lost_since_us = 0;
    trap.running_since_us = now_us();
    ESP_LOGI(TAG, "trap '%s' running", trap.cfg.name);
}

void agent_service::trap_service()
{
    if (!trap.armed || open) {
        return;
    }
    auto *js = qjs_binder::js_host::instance();
    const int64_t now = now_us();
    if (run_wanted.exchange(false) && trap_yields) {
        trap_pause("a programming run");
    }
    if (trap_holds && trap.starting) {
        trap_check_start();
        return;
    }
    if (trap_holds) {
        if (trap.failures && now - trap.running_since_us > STABLE_MS * 1000LL) {
            trap.failures = 0;
        }
        if (!js->target_lost()) {
            trap.lost_since_us = 0;
        } else if (trap.lost_since_us == 0) {
            trap.lost_since_us = now;
        } else if (now - trap.lost_since_us > LOST_RESTART_MS * 1000LL) {
            trap_retry("the target stopped answering", BACKOFF_MIN_MS);
            return;
        }
        if (!js->session_running()) {
            if (++trap.failures >= MAX_FAILURES) {
                trap_fail("the module's runtime kept ending, see the runtime events");
            } else {
                trap.backoff_ms = std::clamp(trap.backoff_ms * 2, BACKOFF_MIN_MS, BACKOFF_MAX_MS);
                trap_retry("the module's runtime ended, restarting it", trap.backoff_ms);
            }
        } else if (js->module_failed()) {
            // Its top level threw after it started: whatever loop it ran has stopped,
            // though the runtime (and its watches) would go on looking armed.
            if (++trap.failures >= MAX_FAILURES) {
                trap_fail("the module kept failing, see the result events");
            } else {
                trap.backoff_ms = std::clamp(trap.backoff_ms * 2, BACKOFF_MIN_MS, BACKOFF_MAX_MS);
                trap_retry("the module's top level failed, restarting it", trap.backoff_ms);
            }
        }
        return;
    }
    if (trap.state == si_agent_TrapState_TRAP_STATE_FAILED) {
        return;
    }
    if (trap.state == si_agent_TrapState_TRAP_STATE_PAUSED && (backend()->is_connected() || job_controller::instance()->is_running())) {
        trap.retry_us = now + HOLDOFF_MS * 1000LL;
        return;
    }
    if (now >= trap.retry_us) {
        trap_start();
    }
}

void agent_service::on_attach_wanted()
{
    instance()->yield_to_debugger();
}

void agent_service::yield_to_debugger()
{
    if (trap_yields) {
        trap_pause("a debugger attached");
    }
}

bool agent_service::make_way_for_run()
{
    if (!trap_holds) {
        return true;
    }
    if (!trap_yields) {
        return false;
    }
    trap_pause("a programming run");
    return true;
}

bool agent_service::yield_for_run(uint32_t wait_ms)
{
    if (!trap_yields) {
        return false;
    }
    run_wanted = true;
    const int64_t end = now_us() + wait_ms * 1000LL;
    while (trap_holds && now_us() < end) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (trap_holds) {
        run_wanted = false;
        return false;
    }
    return true;
}

sidp::status_t agent_service::op_trap_arm(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    if (open) {
        return sidp::STATUS_BUSY;
    }
    auto &req = *static_cast<si_agent_ArmRequest *>(scratch);
    req = si_agent_ArmRequest_init_zero;
    if (!decode(payload, si_agent_ArmRequest_fields, &req) || (req.staged && req.bytecode.size != 0) || !req.has_config ||
        req.config.name[0] == '\0' || req.config.mode < si_agent_TrapMode_TRAP_MODE_YIELD ||
        req.config.mode > si_agent_TrapMode_TRAP_MODE_EXCLUSIVE) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    const si_agent_TrapConfig cfg = req.config;
    const uint32_t timeout_ms = req.timeout_ms;
    uint8_t *module = nullptr;
    size_t len = 0;
    if (req.staged) {
        if (upload == nullptr || upload_len != upload_total) {
            return sidp::STATUS_INVALID_ARGUMENT;
        }
        module = upload;
        len = upload_len;
        upload = nullptr;
        drop_upload();
    } else {
        len = req.bytecode.size;
        module = static_cast<uint8_t *>(heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (module == nullptr) {
            return sidp::STATUS_ERROR;
        }
        memcpy(module, req.bytecode.bytes, len);
    }

    trap_disarm();
    trap.cfg = cfg;
    qjs_binder::js_host::instance()->read_events(0, nullptr, 0, &trap.first_event_seq, nullptr);
    size_t sha_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, module, len, trap.sha256, sizeof(trap.sha256), &sha_len) != PSA_SUCCESS) {
        heap_caps_free(module);
        trap = {};
        return sidp::STATUS_ERROR;
    }
    const si_agent_Permissions perms = cfg.has_permissions ? cfg.permissions : si_agent_Permissions{};
    const auto status = claim(perms, cfg.connect_under_reset, true);
    if (status != sidp::STATUS_OK) {
        heap_caps_free(module);
        trap = {};
        return status;
    }
    trap_holds = true;
    trap_yields = cfg.mode == si_agent_TrapMode_TRAP_MODE_YIELD;
    trap.armed = true;

    auto *js = qjs_binder::js_host::instance();
    const esp_err_t ret = js->session_run(module, len, timeout_ms, target);
    const auto r = js->last_script_result();
    const bool started = ret == ESP_OK && (r.outcome == qjs_binder::script_outcome::ok || r.outcome == qjs_binder::script_outcome::pending);
    if (started && save_trap(module, len) != ESP_OK) {
        heap_caps_free(module);
        trap_disarm();
        ESP_LOGE(TAG, "arm: can't save the trap");
        return sidp::STATUS_ERROR;
    }
    heap_caps_free(module);

    auto &st = *static_cast<si_agent_TrapStatus *>(scratch);
    st = si_agent_TrapStatus_init_zero;
    st.has_first_result = true;
    st.first_result.outcome =
        ret != ESP_OK ? si_agent_ScriptResult_Outcome_OUTCOME_ERROR : static_cast<si_agent_ScriptResult_Outcome>(r.outcome);
    st.first_result.truncated = r.truncated;
    strlcpy(st.first_result.value, r.value, sizeof(st.first_result.value));
    strlcpy(st.first_result.error, ret != ESP_OK ? esp_err_to_name(ret) : r.error, sizeof(st.first_result.error));
    strlcpy(st.first_result.stack, r.stack, sizeof(st.first_result.stack));
    if (started) {
        trap.state = si_agent_TrapState_TRAP_STATE_RUNNING;
        trap.starts = 1;
        trap.running_since_us = now_us();
        ESP_LOGI(TAG, "trap '%s' armed", trap.cfg.name);
    } else {
        trap_disarm();
    }
    fill_status(st);
    return encode(out, si_agent_TrapStatus_fields, &st);
}

sidp::status_t agent_service::op_trap_disarm()
{
    trap_disarm();
    return sidp::STATUS_OK;
}

void agent_service::fill_status(si_agent_TrapStatus &st)
{
    if (trap.armed) {
        st.has_config = true;
        st.config = trap.cfg;
        st.state = trap.state;
        strlcpy(st.note, trap.note, sizeof(st.note));
        st.module_sha256.size = sizeof(trap.sha256);
        memcpy(st.module_sha256.bytes, trap.sha256, sizeof(trap.sha256));
        st.starts = trap.starts;
        st.suppressed = trap.suppressed;
    }
    auto *store = incident_store::instance();
    st.incidents = store->count();
    st.newest_incident = store->newest();
    qjs_binder::js_host::instance()->read_events(0, nullptr, 0, &st.newest_event_seq, nullptr);
}

sidp::status_t agent_service::op_trap_status(pb_ostream_t &out)
{
    auto &st = *static_cast<si_agent_TrapStatus *>(scratch);
    st = si_agent_TrapStatus_init_zero;
    fill_status(st);
    return encode(out, si_agent_TrapStatus_fields, &st);
}

sidp::status_t agent_service::op_incident_list(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    auto &req = *static_cast<si_agent_IncidentListRequest *>(scratch);
    req = si_agent_IncidentListRequest_init_zero;
    if (!decode(payload, si_agent_IncidentListRequest_fields, &req)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    const uint32_t after = req.after_id;
    auto &res = *static_cast<si_agent_IncidentListResponse *>(scratch);
    res = si_agent_IncidentListResponse_init_zero;
    res.incidents_count = incident_store::instance()->list(after, res.incidents, sizeof(res.incidents) / sizeof(res.incidents[0]), &res.more);
    return encode(out, si_agent_IncidentListResponse_fields, &res);
}

sidp::status_t agent_service::op_incident_read(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    auto &req = *static_cast<si_agent_IncidentReadRequest *>(scratch);
    req = si_agent_IncidentReadRequest_init_zero;
    if (!decode(payload, si_agent_IncidentReadRequest_fields, &req)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    const uint32_t id = req.id, offset = req.offset;
    auto &res = *static_cast<si_agent_IncidentReadResponse *>(scratch);
    res = si_agent_IncidentReadResponse_init_zero;
    size_t len = 0;
    esp_err_t ret = incident_store::instance()->read(id, offset, res.data.bytes, sizeof(res.data.bytes), &len, &res.total);
    if (ret != ESP_OK) {
        return ret == ESP_FAIL ? sidp::STATUS_ERROR : sidp::STATUS_INVALID_ARGUMENT;
    }
    res.data.size = len;
    return encode(out, si_agent_IncidentReadResponse_fields, &res);
}

sidp::status_t agent_service::op_incident_delete(std::span<const uint8_t> payload)
{
    auto &req = *static_cast<si_agent_IncidentDeleteRequest *>(scratch);
    req = si_agent_IncidentDeleteRequest_init_zero;
    if (!decode(payload, si_agent_IncidentDeleteRequest_fields, &req)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    return incident_store::instance()->remove(req.id) == ESP_OK ? sidp::STATUS_OK : sidp::STATUS_INVALID_ARGUMENT;
}

uint32_t agent_service::on_incident_open(const qjs_binder::incident_open &inc)
{
    return instance()->incident_open(inc);
}

void agent_service::on_incident_add(uint32_t id, qjs_binder::incident_part part, const char *json, size_t len)
{
    const uint8_t type = part == qjs_binder::incident_part::data ? incident_store::SEC_DATA : incident_store::SEC_OUTCOME;
    incident_store::instance()->append(id, type, json, len);
}

uint32_t agent_service::incident_open(const qjs_binder::incident_open &inc)
{
    const int64_t now = now_us();
    if (now - trap.hour_start_us >= 3600LL * 1000000 || trap.hour_start_us == 0) {
        trap.hour_start_us = now;
        trap.hour_count = 0;
    }
    const uint32_t limit = trap.cfg.incidents_per_hour ? trap.cfg.incidents_per_hour : CONFIG_SI_AGENT_INCIDENTS_PER_HOUR;
    if (trap.hour_count >= limit) {
        trap.suppressed++;
        return 0;
    }
    trap.hour_count++;

    auto *store = incident_store::instance();
    const incident_store::header h = {
        static_cast<uint8_t>(inc.watch ? 0 : 1), inc.watch_id, inc.name, trap.cfg.name, boot_count, (uint64_t)now, prog_log::instance()->utc_ms(),
    };
    const uint32_t id = store->create(h);
    if (id == 0) {
        return 0;
    }
    store->append(id, incident_store::SEC_CAUSE, inc.cause_json, strlen(inc.cause_json));
    const size_t n = events_json(inc_events, INCIDENT_EVENTS_MAX);
    store->append(id, incident_store::SEC_EVENTS, inc_events, n);
    if (inc.capture != nullptr) {
        store->append(id, incident_store::SEC_CAPTURE, inc.capture, inc.capture_len);
    }
    ESP_LOGI(TAG, "incident %lu written in %lld ms", (unsigned long)id, (now_us() - now) / 1000); // slow storage shows here
    return id;
}

size_t agent_service::events_json(char *out, size_t cap)
{
    auto *js = qjs_binder::js_host::instance();
    auto *evs = static_cast<qjs_binder::event_out *>(events);
    uint32_t newest = 0;
    js->read_events(0, nullptr, 0, &newest, nullptr);
    uint32_t after = newest > INCIDENT_EVENTS ? newest - INCIDENT_EVENTS : 0;
    after = std::max(after, trap.first_event_seq); // sessions are refused while armed: the rest are the trap's
    size_t n = strlcpy(out, "[", cap);
    bool first = true;
    for (size_t got; (got = js->read_events(after, evs, EVENTS_PER_READ, nullptr, nullptr)) != 0;) {
        for (size_t i = 0; i < got; i++) {
            after = evs[i].seq;
            char head[128];
            const int h = snprintf(head, sizeof(head), "%s{\"seq\":%lu,\"us\":%llu,\"kind\":\"%s\",\"capture\":%lu,\"data\":", first ? "" : ",",
                                   (unsigned long)evs[i].seq, (unsigned long long)evs[i].uptime_us, evs[i].kind,
                                   (unsigned long)evs[i].capture_id);
            const size_t body = strlen(evs[i].json);
            if (n + h + body + 3 > cap) {
                goto done;
            }
            memcpy(out + n, head, h);
            n += h;
            memcpy(out + n, evs[i].json, body);
            n += body;
            out[n++] = '}';
            first = false;
        }
    }
done:
    out[n++] = ']';
    out[n] = '\0';
    return n;
}

#else // no scripts in this firmware

esp_err_t agent_service::init(notify_fn) { return ESP_OK; }
sidp::status_t agent_service::handle(uint16_t, std::span<const uint8_t>, pb_ostream_t &) { return sidp::STATUS_UNSUPPORTED; }
void agent_service::service() {}
void agent_service::idle(uint32_t ms) { vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(ms))); }
void agent_service::close() {}
void agent_service::connection_closed() {}
bool agent_service::make_way_for_run() { return true; }
bool agent_service::yield_for_run(uint32_t) { return false; }

#endif

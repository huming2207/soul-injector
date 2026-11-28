#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "offline_flasher.hpp"

#include "esp32_serial_backend.hpp"
#include "fw_asset_manager.hpp"
#include "job_controller.hpp"
#include "swd_cortexm_backend.hpp"
#include "qjs_binder.hpp"

namespace
{
    class backend_target final : public qjs_binder::js_target
    {
    public:
        explicit backend_target(target_backend &backend) : backend(backend) {}
        esp_err_t read32(uint32_t addr, uint32_t *val) override { return backend.read_mem32(addr, val); }
        esp_err_t write32(uint32_t addr, uint32_t val) override { return backend.write_mem32(addr, val); }
        esp_err_t halt() override { return backend.halt_target(); }
        esp_err_t wait_halt() override { return backend.wait_halt(); }
        esp_err_t reset() override { return backend.reset_target(); }
        esp_err_t reinit() override { return backend.reinit_debug(); }
        esp_err_t read_mem(uint32_t addr, uint8_t *buf, size_t len) override { return backend.read_mem(addr, buf, len); }
        esp_err_t write_mem(uint32_t addr, const uint8_t *buf, size_t len) override { return backend.write_mem(addr, buf, len); }
        esp_err_t flash_read(uint32_t addr, uint8_t *buf, size_t len) override { return backend.flash_read(addr, buf, len); }
        esp_err_t flash_write(uint32_t addr, const uint8_t *buf, size_t len) override { return backend.flash_write(addr, buf, len); }
        esp_err_t flash_erase(uint32_t addr, uint32_t count) override { return backend.flash_erase(addr, count); }
        esp_err_t flash_check(flash_op op, uint32_t addr, size_t len, char *why, size_t why_len) override
        {
            auto o = op == flash_op::read    ? target_backend::flash_op::read
                     : op == flash_op::write ? target_backend::flash_op::write
                                             : target_backend::flash_op::erase;
            return backend.flash_check(o, addr, len, why, why_len);
        }
        esp_err_t call(uint32_t addr, const uint32_t args[4], uint32_t stack_top, uint32_t timeout_ms, qjs_binder::cancel_fn cancelled,
                       uint32_t *r0) override
        {
            return backend.call(addr, args, stack_top, timeout_ms, cancelled, r0);
        }
        esp_err_t resume() override { return backend.resume_target(); }
        esp_err_t read_reg(uint32_t id, uint32_t *val) override { return backend.read_core_reg(id, val); }
        esp_err_t write_reg(uint32_t id, uint32_t val) override { return backend.write_core_reg(id, val); }

    private:
        target_backend &backend;
    };

    bool cancel_requested()
    {
        return job_controller::instance()->cancel_requested();
    }

    void progress(const char *stage, int pct)
    {
        qjs_binder::js_host::instance()->progress(stage, pct);
    }

    void program_progress(uint32_t done, uint32_t total)
    {
        progress("program", total ? (int)(done * 100ULL / total) : 100);
        display_manager::instance()->get_composer()->set_progress(done, total); // never blocks
    }

    bool has_script()
    {
        return fw_asset_manager::instance()->script_path() != nullptr;
    }

    esp_err_t run_script_hook(qjs_binder::hook h, target_backend &backend)
    {
        auto *js = qjs_binder::js_host::instance();
        if (!js->exports_hook(h)) {
            return ESP_OK;
        }
        // Exported but its runtime is gone (stopped in an earlier callback, out of
        // memory): run_hook() fails "script isn't running", and so does the run.
        backend_target target(backend);
        return js->run_hook(h, target, fw_asset_manager::instance()->hook_timeout_ms(), cancel_requested);
    }

    const char *stage_name(flasher::pg_state s)
    {
        switch (s) {
        case flasher::LOAD_ASSET:
            return "load";
        case flasher::PRE_PROGRAM:
            return "preProgram";
        case flasher::DETECT:
            return "detect";
        case flasher::ERASE:
            return "erase";
        case flasher::PROGRAM:
            return "program";
        case flasher::VERIFY:
            return "verify";
        case flasher::SELF_TEST:
            return "test";
        case flasher::POST_PROGRAM:
            return "postProgram";
#ifdef CONFIG_SI_SG_PROG_RIG
        case flasher::SG_CURRENT_TEST:
            return "currentTest";
#endif
        default:
            return "unknown";
        }
    }
} // namespace

void offline_flasher::init()
{
    display = display_manager::instance();
    composer = display->get_composer();
    // Release any transport a previous (aborted) run left open, otherwise the
    // backend short-circuits its next connect on stale state.
    if (backend != nullptr) {
        backend->release_transport();
    }
    backend = nullptr; // this run's comes from its job (select_backend); until then, none

    qjs_binder::js_host::instance()->end_run();
    qjs_binder::js_host::instance()->forget_last_script(); // record only this run's

    failed_state = flasher::DONE;
    state = flasher::LOAD_ASSET;
}

void offline_flasher::select_backend()
{
    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    switch (cfg.family) {
    case si::config::target_family::esp32_serial:
        backend = esp32_serial_backend::instance();
        break;
    case si::config::target_family::swd_cortex_m:
    default:
        backend = swd_cortexm_backend::instance();
        break;
    }
    ESP_LOGI(TAG, "select_backend: using %s", backend->name());
}

void offline_flasher::end_run(bool passed)
{
    if (backend == nullptr) {
        return;
    }
    // The job says how the target is left (default: reset after a pass, as it is after a failure).
    target_backend::run_end how = target_backend::run_end::as_is;
    switch (fw_asset_manager::instance()->run_end(passed)) {
    case si_job_RunEnd_RUN_END_RESET:
        how = target_backend::run_end::reset;
        break;
    case si_job_RunEnd_RUN_END_RESUME:
        how = target_backend::run_end::resume;
        break;
    default:
        break;
    }
    const esp_err_t ret = backend->end_run(how);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "end_run: 0x%x", ret); // the run's result stands
    }
}

void offline_flasher::on_error()
{
    auto *js = qjs_binder::js_host::instance();
    if (backend != nullptr && has_script() && js->has_hook(qjs_binder::hook::on_fail)) {
        backend_target target(*backend);
        auto ret = js->run_on_fail(stage_name(failed_state), nullptr, target, fw_asset_manager::instance()->hook_timeout_ms(),
                                   cancel_requested);
        ESP_LOGI(TAG, "onFail(%s): 0x%x", stage_name(failed_state), ret);
    }

    // Terminal error path: release the transport so the backend cannot carry
    // stale connection state into the next session (the FSM waits for target
    // disconnect/reconnect and then reloads assets; a still-connected backend
    // would short-circuit its connect sequence on the new target).
    end_run(false);
    led.set_color(0x80, 0x00, 0x00);
}

void offline_flasher::on_erase()
{
    ESP_LOGI(TAG, "Erasing");
    composer->display_erase(UINT8_MAX);
    progress("erase", 0);

    auto ret = backend->erase();
    progress("erase", 100);
    if (ret == ESP_ERR_INVALID_STATE) {
        composer->display_error("Job error", "The job has no flash address.\nSend the job again.");
        state = flasher::ERROR;
        ESP_LOGE(TAG, "Failed to read flash addresses");
    } else if (ret != ESP_OK) {
        composer->display_error("Erase failed", "Could not erase the chip.\nTry again. If it fails again, set the board aside.");
        state = flasher::ERROR;
    } else {
        state = flasher::PROGRAM;
    }
}

void offline_flasher::on_program()
{
    composer->display_program(0);
    progress("program", 0);
    backend->on_progress = program_progress;
    auto ret = backend->program(&written_len);
    backend->on_progress = nullptr;
    if (ret != ESP_OK) {
        composer->display_error("Write failed", "Could not write the firmware.\nTry again. If it fails again, set the board aside.");
        state = flasher::ERROR;
    } else {
        state = flasher::VERIFY;
    }
}

void offline_flasher::on_detect()
{
    ESP_LOGI(TAG, "Detecting");

    composer->display_connecting();
    auto ret = backend->detect();
    uint32_t max_retry = 10;
    while (ret != ESP_OK && max_retry > 0 && !cancel_requested()) { // each try takes seconds: a cancel stops them
        ESP_LOGE(TAG, "Detect failed, retrying");
        max_retry -= 1;
        vTaskDelay(pdMS_TO_TICKS(30));
        ret = backend->detect();
    }

    if (ret != ESP_OK) {
        composer->display_error("No board found", "Check the cable and that the board has power.");
        state = flasher::ERROR;
    } else {
        state = flasher::ERASE; // To erase
    }
}

void offline_flasher::on_done()
{
    // Terminal success path: release the transport (idempotent for backends
    // that already released it in POST_PROGRAM / SG_CURRENT_TEST).
    if (backend != nullptr) {
        backend->release_transport();
    }
    led.set_color(0, 80, 0); // Green
    composer->display_done();
}

void offline_flasher::on_verify()
{
    led.set_color(0x68, 0x26, 0x99); // Purple?
    composer->display_verify();
    progress("verify", 0);
    auto ret = backend->verify(written_len);
    progress("verify", 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to verify!");
        composer->display_error("Check failed", "The firmware read back wrong.\nTry again. If it fails again, set the board aside.");
        state = flasher::ERROR;
    } else {
        ESP_LOGI(TAG, "Firmware verified");
        state = flasher::SELF_TEST;
    }
}

void offline_flasher::on_self_test()
{
    led.set_color(0, 0xb7, 0xeb); // Cyan??
    ESP_LOGI(TAG, "Run self test");

    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    if (has_script() && qjs_binder::js_host::instance()->exports_hook(qjs_binder::hook::test)) {
        if (cfg.test_count > 0) {
            ESP_LOGW(TAG, "Self test: the script's test() replaces the job's %u self tests", (unsigned)cfg.test_count);
        }
        composer->display_test(0, 0, "Running the job's test script");
        auto ret = run_script_hook(qjs_binder::hook::test, *backend);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Self test: script: %s (0x%x)", qjs_binder::js_host::instance()->last_error(), ret);
            composer->display_error("Test failed", qjs_binder::js_host::instance()->last_error());
            state = flasher::ERROR;
            return;
        }
        if (cfg.family != si::config::target_family::esp32_serial) {
            backend->reset_target();
        }
        state = flasher::POST_PROGRAM;
        return;
    }

    for (size_t idx = 0; idx < cfg.test_count; idx += 1) {
        const si::config::test_item &item = cfg.tests[idx];
        composer->display_test(idx, (cfg.test_count - 1), nullptr);

        if (item.type == si::config::test_item::INTERNAL_SIMPLE_TEST) {
            uint32_t func_ret = UINT32_MAX;
            ESP_LOGW(TAG, "Self test: 0x%08lx, %s type %u", item.addr, item.name, item.type);
            auto ret = backend->self_test(item, &func_ret);
            if (ret == ESP_ERR_NOT_SUPPORTED) {
                /*
                 * This backend/family has no self test support (always the
                 * case for esp32 targets when self_tests entries exist).
                 * Skip the remaining tests but still run POST_PROGRAM - the
                 * post-program procedure and the SG rig current test must
                 * not be silently skipped.
                 */
                ESP_LOGW(TAG, "Self test not supported on this target, skipping remaining tests");
                if (cfg.family != si::config::target_family::esp32_serial) {
                    backend->reset_target();
                }
                state = flasher::POST_PROGRAM;
                return;
            } else if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Self test @ 0x%08lx failed, host error 0x%x, function returned 0x%lx", item.addr, ret, func_ret);
                char msg[128] = {0};
                snprintf(msg, sizeof(msg), "Test %u of %u (%s) could not run.\nAddress 0x%08lx", idx + 1, cfg.test_count, item.name, item.addr);
                composer->display_error("Test failed", msg);
                state = flasher::ERROR;
                return;
            }

            if (func_ret != 0) {
                ESP_LOGW(TAG, "Self test failed, host returned 0x%x, target returned error 0x%lx", ret, func_ret);
                char msg[128] = {0};
                snprintf(msg, sizeof(msg), "Test %u of %u (%s) failed with code %lu.\nAddress 0x%08lx", idx + 1, cfg.test_count, item.name,
                         func_ret, item.addr);
                composer->display_error("Test failed", msg);
                state = flasher::ERROR;
                return;
            }

            ESP_LOGW(TAG, "Self test OK, host returned 0x%x, function returned 0x%lx", ret, func_ret);

        } else if (item.type == si::config::test_item::INTERNAL_EXTEND_TEST) {
            ESP_LOGW(TAG, "Unsupported InternalExtendTest type!");
        }
    }

    // Keep ESP32 targets in download mode until release_transport() after POST_PROGRAM,
    // so post_prog register operations can still talk to the flasher stub.
    if (cfg.family != si::config::target_family::esp32_serial) {
        backend->reset_target();
    }
    state = flasher::POST_PROGRAM;
}

void offline_flasher::on_post_program()
{
    // Only when there's something to do: otherwise Pass follows at once
    const bool has_steps = has_script() ? qjs_binder::js_host::instance()->exports_hook(qjs_binder::hook::post_program)
                                        : fw_asset_manager::instance()->post_program_steps() != nullptr;
    if (has_steps) {
        composer->display_finishing();
    }

    auto ret = backend->begin_session();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "post_prog: cannot begin backend session: 0x%x", ret);
        composer->display_error("Finishing failed", "The job's last steps failed.\nTry again.");
        state = flasher::ERROR;
        return;
    }

    if (has_script()) {
        ret = run_script_hook(qjs_binder::hook::post_program, *backend);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "post_prog: script: %s (0x%x)", qjs_binder::js_host::instance()->last_error(), ret);
            composer->display_error("Script error", qjs_binder::js_host::instance()->last_error());
            state = flasher::ERROR;
            return;
        }
        end_run(true);
#ifndef CONFIG_SI_SG_PROG_RIG
        state = flasher::DONE;
#else
        state = flasher::SG_CURRENT_TEST;
#endif
        return;
    }

    const si_job_Procedure *steps = fw_asset_manager::instance()->post_program_steps();
    if (steps == nullptr) {
        ESP_LOGI(TAG, "post_prog: no steps, skipping");
        end_run(true); // also releases the transport, which would poison the next session
#ifndef CONFIG_SI_SG_PROG_RIG
        state = flasher::DONE;
#else
        state = flasher::SG_CURRENT_TEST;
#endif
        return;
    }

    ret = procedure_executor::execute(*steps, *backend);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "post_prog: execution error: 0x%x", ret);
        composer->display_error("Finishing failed", "The job's last steps failed.\nTry again.");
        state = flasher::ERROR;
        return;
    }

    ESP_LOGI(TAG, "post_prog: OK");

    end_run(true);

#ifndef CONFIG_SI_SG_PROG_RIG
    state = flasher::DONE;
#else
    state = flasher::SG_CURRENT_TEST;
#endif
}

#ifdef CONFIG_SI_SG_PROG_RIG
void offline_flasher::on_current_test()
{
    // Shut up the target transport to run firmware
    backend->release_transport();

    double min = 0, max = 0, avg = 0;
    auto ret = pwr_test.init((gpio_num_t)CONFIG_SI_SG_PWR_TESTER_ALERT);
    ret = ret ?: pwr_test.start_testing(3000, &max, &min, &avg);
    if (ret != ESP_OK) {
        composer->display_error("Current test failed", "The current sensor did not answer.");
        state = flasher::ERROR;
        ESP_LOGE(TAG, "curr_test: failed to start testing 0x%x", ret);
        return;
    }

    ESP_LOGI(TAG, "Min=%.6f Max=%.6f, Avg=%.6f, ret=0x%x", min * 1000, max * 1000, avg * 1000, ret);
    composer->display_current(min * 1000000, max * 1000000, avg * 1000000, "OK", lv_color_make(0, 0xff, 0));
    state = flasher::DONE;
    vTaskDelay(pdMS_TO_TICKS(10000));
}
#endif

void offline_flasher::on_cancelled()
{
    end_run(false);
    led.set_color(0x80, 0x80, 0x00); // Yellow
    composer->display_notice("Stopped", "The job was cancelled.");
}

void offline_flasher::check_cancel()
{
    bool finished = state == flasher::ERROR || state == flasher::DONE || state == flasher::CANCELLED;
    if (!finished && job_controller::instance()->cancel_requested()) {
        ESP_LOGW(TAG, "Cancel requested");
        failed_state = state;
        state = flasher::CANCELLED;
    }
}

esp_err_t offline_flasher::handle_states()
{
    check_cancel();

    // Remember which state failed so the run result can name the stage.
    const flasher::pg_state current = state;
    esp_err_t ret = run_state();
    if (state == flasher::ERROR && current != flasher::ERROR) {
        failed_state = current;
        // A step that failed because it was cancelled (a script hook stops,
        // a retry loop gives up) ends as cancelled: no onFail, no red screen,
        // as the run's record already says
        if (cancel_requested()) {
            state = flasher::CANCELLED;
        }
    }
    return ret;
}

esp_err_t offline_flasher::run_state()
{
    switch (state) {
    case flasher::PRE_PROGRAM: {
        on_pre_program();
        break;
    }

    case flasher::LOAD_ASSET: {
        on_load_asset();
        break;
    }

    case flasher::DETECT: {
        on_detect();
        break;
    }

    case flasher::ERASE: {
        on_erase();
        break;
    }

    case flasher::PROGRAM: {
        on_program();
        break;
    }

    case flasher::ERROR: {
        on_error();
        return ESP_FAIL;
    }

    case flasher::DONE: {
        on_done();
        return ESP_OK;
    }

    case flasher::CANCELLED: {
        on_cancelled();
        return ESP_FAIL;
    }

    case flasher::VERIFY: {
        on_verify();
        break;
    }

    case flasher::POST_PROGRAM: {
        on_post_program();
        break;
    }

    case flasher::SELF_TEST: {
        on_self_test();
        break;
    }

#ifdef CONFIG_SI_SG_PROG_RIG
    case flasher::SG_CURRENT_TEST: {
        on_current_test();
        break;
    }
#endif
    }

    return ESP_ERR_NOT_FINISHED;
}

void offline_flasher::on_pre_program()
{
    led.set_color(0, 0xb7, 0xeb); // Cyan??
    auto ret = backend->begin_session();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pre_prog: cannot begin backend session: 0x%x", ret);
        composer->display_error("Preparing failed", "The job's first steps failed.\nTry again.");
        state = flasher::ERROR;
        return;
    }

    if (has_script()) {
        ret = run_script_hook(qjs_binder::hook::pre_program, *backend);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "pre_prog: script: %s (0x%x)", qjs_binder::js_host::instance()->last_error(), ret);
            composer->display_error("Script error", qjs_binder::js_host::instance()->last_error());
            state = flasher::ERROR;
            return;
        }
        state = flasher::DETECT;
        return;
    }

    const si_job_Procedure *steps = fw_asset_manager::instance()->pre_program_steps();
    if (steps == nullptr) {
        ESP_LOGI(TAG, "pre_prog: no steps, skipping");
        state = flasher::DETECT; // To detect
        return;
    }

    ret = procedure_executor::execute(*steps, *backend);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "pre_prog: execution error: 0x%x", ret);
        composer->display_error("Preparing failed", "The job's first steps failed.\nTry again.");
        state = flasher::ERROR;
        return;
    }

    ESP_LOGI(TAG, "pre_prog: OK");
    state = flasher::DETECT; // To detect
}

void offline_flasher::on_load_asset()
{
    composer->display_starting();
    auto *asset = fw_asset_manager::instance();
    if (!asset->has_job()) {
        ESP_LOGE(TAG, "load_asset: no job");
        composer->display_notice("No job", "Send a job from the computer.");
        state = flasher::ERROR;
        return;
    }

    // Images may have been replaced since the job was activated.
    auto ret = asset->verify_images();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "load_asset: images do not match the job: 0x%x %s", ret, esp_err_to_name(ret));
        composer->display_notice("Job out of date", "The firmware files changed.\nSend the job again.");
        state = flasher::ERROR;
        return;
    }

    select_backend();

    const char *script = asset->script_path();
    if (script != nullptr) {
        auto *js = qjs_binder::js_host::instance();
        const auto &cfg = asset->config();
        qjs_binder::run_info info = {asset->job_name(), cfg.variant_name, si::config::family_to_str(cfg.family), asset->script_network()};
        ret = js->begin_run(script, asset->script_sha256(), info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "load_asset: script: %s (0x%x)", js->last_error(), ret);
            composer->display_error("Script error", js->last_error());
            state = flasher::ERROR;
            return;
        }
    }
    state = flasher::PRE_PROGRAM;
}

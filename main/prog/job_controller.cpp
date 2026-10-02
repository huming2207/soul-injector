#include "job_controller.hpp"

#include <cstring>

#include <esp_log.h>
#include <nvs.h>

#include "asset_store.hpp"
#include "fw_asset_manager.hpp"
#include "prog_log.hpp"

esp_err_t job_controller::init(EventGroupHandle_t _evt_group, EventBits_t run_request_bit)
{
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    evt_group = _evt_group;
    run_bit = run_request_bit;

    nvs_handle_t handle = 0;
    uint8_t stored = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, NVS_TRIGGER_KEY, &stored) == ESP_OK && stored == si_manage_Trigger_TRIGGER_AUTO_ON_DETECT) {
            trigger = si_manage_Trigger_TRIGGER_AUTO_ON_DETECT;
        }
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "init: trigger %s", trigger == si_manage_Trigger_TRIGGER_AUTO_ON_DETECT ? "auto" : "manual");
    return ESP_OK;
}

void job_controller::lock()
{
    xSemaphoreTake(mutex, portMAX_DELAY);
}

void job_controller::unlock()
{
    xSemaphoreGive(mutex);
}

esp_err_t job_controller::save_trigger(si_manage_Trigger new_trigger)
{
    nvs_handle_t handle = 0;
    auto ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(handle, NVS_TRIGGER_KEY, (uint8_t)new_trigger);
    ret = ret ?: nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

esp_err_t job_controller::set_job(const uint8_t *sha256, si_manage_Trigger new_trigger)
{
    if (new_trigger != si_manage_Trigger_TRIGGER_MANUAL && new_trigger != si_manage_Trigger_TRIGGER_AUTO_ON_DETECT) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    esp_err_t ret = ERR_RUNNING;
    if (!running) {
        ret = fw_asset_manager::instance()->activate_staged(sha256);
        ret = ret ?: save_trigger(new_trigger);
    }
    if (ret == ESP_OK) {
        trigger = new_trigger;
    }
    unlock();
    return ret;
}

esp_err_t job_controller::commit_asset()
{
    lock();
    esp_err_t ret = running ? ERR_RUNNING : asset_store::instance()->commit();
    unlock();
    return ret;
}

void job_controller::start_run_locked(si_manage_Trigger run_trigger)
{
    running = true;
    current_trigger = run_trigger;
    last_run_id += 1;
    cancel_flag.store(false);
    ESP_LOGI(TAG, "run %lu started", last_run_id);
}

esp_err_t job_controller::request_run(uint32_t *run_id_out)
{
    lock();
    esp_err_t ret = ESP_OK;
    if (running) {
        ret = ERR_RUNNING;
    } else if (!fw_asset_manager::instance()->has_job()) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (prog_log::instance()->is_full()) {
        ret = ERR_LOG_FULL;
    } else {
        start_run_locked(si_manage_Trigger_TRIGGER_MANUAL);
        *run_id_out = last_run_id;
    }
    unlock();

    if (ret == ESP_OK) {
        xEventGroupSetBits(evt_group, run_bit);
    }
    return ret;
}

bool job_controller::begin_auto_run()
{
    lock();
    bool start = !running && trigger == si_manage_Trigger_TRIGGER_AUTO_ON_DETECT && fw_asset_manager::instance()->has_job() &&
                 !prog_log::instance()->is_full();
    if (start) {
        start_run_locked(si_manage_Trigger_TRIGGER_AUTO_ON_DETECT);
    }
    unlock();
    return start;
}

void job_controller::cancel(uint32_t run_id)
{
    lock();
    if (running && (run_id == 0 || run_id == last_run_id)) {
        cancel_flag.store(true);
    }
    unlock();
}

si_manage_Stage job_controller::to_stage(flasher::pg_state failed_state)
{
    switch (failed_state) {
    case flasher::LOAD_ASSET:
        return si_manage_Stage_STAGE_LOAD;
    case flasher::PRE_PROGRAM:
        return si_manage_Stage_STAGE_PRE_PROGRAM;
    case flasher::DETECT:
        return si_manage_Stage_STAGE_DETECT;
    case flasher::ERASE:
        return si_manage_Stage_STAGE_ERASE;
    case flasher::PROGRAM:
        return si_manage_Stage_STAGE_PROGRAM;
    case flasher::VERIFY:
        return si_manage_Stage_STAGE_VERIFY;
    case flasher::SELF_TEST:
        return si_manage_Stage_STAGE_SELF_TEST;
    case flasher::POST_PROGRAM:
        return si_manage_Stage_STAGE_POST_PROGRAM;
#ifdef CONFIG_SI_SG_PROG_RIG
    case flasher::SG_CURRENT_TEST:
        return si_manage_Stage_STAGE_CURRENT_TEST;
#endif
    default:
        return si_manage_Stage_STAGE_NONE;
    }
}

void job_controller::finish_run(esp_err_t ret, flasher::pg_state failed_state, uint32_t duration_ms)
{
    lock();
    last_result.run_id = last_run_id;
    last_result.duration_ms = duration_ms;
    last_result.failed_stage = si_manage_Stage_STAGE_NONE;
    if (ret == ESP_OK) {
        last_result.outcome = si_manage_Outcome_OUTCOME_PASS;
    } else {
        last_result.outcome = cancel_flag.load() ? si_manage_Outcome_OUTCOME_CANCELLED : si_manage_Outcome_OUTCOME_FAIL;
        last_result.failed_stage = to_stage(failed_state);
    }
    unlock();

    ESP_LOGI(TAG, "run %lu finished: outcome %d, stage %d, %lu ms", last_result.run_id, last_result.outcome, last_result.failed_stage, duration_ms);

    // Still marked running so the job cannot change while it is recorded.
    record_run(ret);
    lock();
    running = false;
    cancel_flag.store(false);
    unlock();
}

void job_controller::record_run(esp_err_t ret)
{
    auto *asset = fw_asset_manager::instance();
    si_manage_RunRecord record = si_manage_RunRecord_init_zero;
    record.has_result = true;
    record.result = last_result;
    record.trigger = current_trigger;
    record.error_code = ret;
    strlcpy(record.job_name, asset->job_name(), sizeof(record.job_name));
    memcpy(record.job_sha256, asset->job_sha256(), sizeof(record.job_sha256));
    prog_log::instance()->add_run(record);
}

void job_controller::get_status(si_manage_JobStatus &status)
{
    auto *asset = fw_asset_manager::instance();
    status = si_manage_JobStatus_init_zero;

    lock();
    status.has_job = asset->has_job();
    if (status.has_job) {
        strlcpy(status.name, asset->job_name(), sizeof(status.name));
        memcpy(status.sha256, asset->job_sha256(), sizeof(status.sha256));
    }
    status.trigger = trigger;
    status.state = running ? si_manage_RunState_RUN_STATE_RUNNING : si_manage_RunState_RUN_STATE_IDLE;
    status.current_run_id = running ? last_run_id : 0;
    status.has_last_result = true;
    status.last_result = last_result;
    unlock();
}

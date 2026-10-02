#pragma once

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

#include <esp_err.h>
#include <manage.pb.h>

#include "offline_flasher.hpp"

/**
 * Decides when the active job runs and keeps the result of the last run.
 *
 * Two tasks use it: the SIDP service task (job and asset changes, run and
 * cancel requests, status) and the programming task (automatic runs and run
 * results). One mutex serialises them, and anything that replaces the job or
 * an asset is refused while a run is in progress, so the programming task can
 * read the job without holding the lock.
 *
 * The trigger survives reboots (NVS); run results and run IDs do not, but
 * every finished run is recorded in prog_log.
 */
class job_controller
{
public:
    static job_controller *instance()
    {
        static job_controller _instance;
        return &_instance;
    }

    job_controller(job_controller const &) = delete;
    void operator=(job_controller const &) = delete;

    /** Returned when a request conflicts with a run in progress. */
    static const constexpr esp_err_t ERR_RUNNING = ESP_ERR_NOT_FINISHED;

    /** Returned by request_run() while the production log needs collecting (as prog_log::check_space()). */
    static const constexpr esp_err_t ERR_LOG_FULL = ESP_ERR_NO_MEM;

    /** @p run_request_bit in @p evt_group wakes the programming task for a run. */
    esp_err_t init(EventGroupHandle_t evt_group, EventBits_t run_request_bit);

    // ---- SIDP service task ----
    /** Activate the staged job and set the trigger. ERR_RUNNING while a run is in progress. */
    esp_err_t set_job(const uint8_t *sha256, si_manage_Trigger new_trigger);

    /** Commit the asset upload. ERR_RUNNING while a run is in progress. */
    esp_err_t commit_asset();

    /**
     * Queue one run of the active job. ERR_RUNNING while a run is in progress,
     * ERR_LOG_FULL while the log is full, ESP_FAIL after a log write error.
     */
    esp_err_t request_run(uint32_t *run_id_out);

    /** Ask the current run to stop; @p run_id 0 matches any run. */
    void cancel(uint32_t run_id);

    void get_status(si_manage_JobStatus &status);

    // ---- Programming task ----
    /** Claim an automatic run after a target was plugged in; refused while the log cannot take its record. */
    bool begin_auto_run();

    bool cancel_requested() const
    {
        return cancel_flag.load();
    }

    void finish_run(esp_err_t ret, flasher::pg_state failed_state, uint32_t duration_ms);

private:
    job_controller() = default;

    void lock();
    void unlock();
    void start_run_locked(si_manage_Trigger run_trigger);
    void record_run(esp_err_t ret);
    esp_err_t save_trigger(si_manage_Trigger new_trigger);
    static si_manage_Stage to_stage(flasher::pg_state failed_state);

    SemaphoreHandle_t mutex = nullptr;
    EventGroupHandle_t evt_group = nullptr;
    EventBits_t run_bit = 0;
    si_manage_Trigger trigger = si_manage_Trigger_TRIGGER_MANUAL;
    bool running = false;
    si_manage_Trigger current_trigger = si_manage_Trigger_TRIGGER_MANUAL; // What started the current run
    uint32_t last_run_id = 0;
    std::atomic<bool> cancel_flag = false;
    si_manage_RunResult last_result = si_manage_RunResult_init_zero;

    static const constexpr char NVS_NAMESPACE[] = "si_job";
    static const constexpr char NVS_TRIGGER_KEY[] = "trigger";
    static const constexpr char *TAG = "job_ctrl";
};

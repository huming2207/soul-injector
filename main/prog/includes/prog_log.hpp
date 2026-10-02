#pragma once

#include <atomic>
#include <cstdint>

#include <esp_err.h>
#include <manage.pb.h>
#include <pb.h>
#include <wear_levelling.h>

class on9rstore;

/**
 * Production log: one record per programming run plus on9rstore's boot
 * records, kept on the "log" partition until the host collects them.
 *
 * Entries are never overwritten before the host acknowledges them. A run is
 * only admitted when check_space() says its record will fit, so a finished
 * run can always be recorded. A failed write stops further runs until the
 * device restarts and the store recovers.
 *
 * Thread safety: the programming task adds records, the SIDP service task
 * reads, acknowledges and sets the time; on9rstore locks its own state.
 */
class prog_log
{
public:
    static prog_log *instance()
    {
        static prog_log _instance;
        return &_instance;
    }

    prog_log(prog_log const &) = delete;
    void operator=(prog_log const &) = delete;

    /** Mount the log partition and open the store. */
    esp_err_t init();

    /**
     * ESP_OK when the record of one more run fits. ESP_ERR_NO_MEM while the
     * host must collect the log first, ESP_FAIL after a failed write.
     */
    esp_err_t check_space();

    /** Record a finished run. */
    void add_run(const si_manage_RunRecord &record);

    /** Set the clock for this boot; later calls in the same boot are ignored. */
    esp_err_t set_time(uint64_t utc_ms);

    /** Encode the entries after @p after_id that fit in @p out as LogReadResponse. */
    esp_err_t read(uint64_t after_id, pb_ostream_t &out);

    /** Mark entries up to @p up_to_id collected, then write any held record. */
    esp_err_t ack(uint64_t up_to_id);

    void get_info(si_manage_DeviceInfo &info);

private:
    prog_log() = default;

    esp_err_t read_entry(uint64_t after_id, si_manage_LogEntry &entry);
    static void convert_boot_record(si_manage_LogEntry &entry);

    on9rstore *store = nullptr;
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    bool time_set = false; // SIDP service task only
    std::atomic<bool> write_failed = false;

    /** on9rstore entry type of an encoded RunRecord. */
    static const constexpr uint16_t ENTRY_RUN = 1;
    static const constexpr char BASE_PATH[] = "/log";
    static const constexpr char PARTITION_LABEL[] = "log";
    static const constexpr char *TAG = "prog_log";
};

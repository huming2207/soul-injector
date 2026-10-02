#pragma once

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_err.h>
#include <manage.pb.h>
#include <pb.h>
#include <wear_levelling.h>

class on9rstore;

/**
 * Production log: one record per programming run plus on9rstore's boot
 * records, kept on the "log" partition until the host collects them.
 *
 * Entries are never overwritten before the host acknowledges them. When the
 * log is full, the run record that did not fit is held in RAM (lost on reset)
 * and further runs are refused until the host acknowledges.
 *
 * Thread safety: the programming task adds records, the SIDP service task
 * reads, acknowledges and sets the time; a mutex guards the held record.
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

    /** Record a finished run; held in RAM when the log is full. */
    void add_run(const si_manage_RunRecord &record);

    /** True while a run record waits for the host to collect the log. */
    bool is_full();

    /** Set the clock for this boot; later calls in the same boot are ignored. */
    esp_err_t set_time(uint64_t utc_ms);

    /** Encode the entries after @p after_id that fit in @p out as LogReadResponse. */
    esp_err_t read(uint64_t after_id, pb_ostream_t &out);

    /** Mark entries up to @p up_to_id collected, then write any held record. */
    esp_err_t ack(uint64_t up_to_id);

    void get_info(si_manage_DeviceInfo &info);

private:
    prog_log() = default;

    void lock();
    void unlock();
    esp_err_t write_pending_locked();
    esp_err_t read_entry(uint64_t after_id, si_manage_LogEntry &entry);
    static void convert_boot_record(si_manage_LogEntry &entry);

    on9rstore *store = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    bool time_set = false;
    size_t pending_len = 0; // Encoded RunRecord held while the log is full; 0: none
    uint8_t pending[si_manage_RunRecord_size] = {};

    /** on9rstore entry type of an encoded RunRecord. */
    static const constexpr uint16_t ENTRY_RUN = 1;
    static const constexpr char BASE_PATH[] = "/log";
    static const constexpr char PARTITION_LABEL[] = "log";
    static const constexpr char *TAG = "prog_log";
};

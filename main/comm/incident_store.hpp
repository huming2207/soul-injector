#pragma once

#include <cstddef>
#include <cstdint>

#include <atomic>

#include <agent.pb.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

#if CONFIG_SI_JS_ENABLE

/**
 * Trap incidents in /data/inc, one file each. The public calls come from the
 * SIDP service task; files are written on a task of their own, so a watch
 * that records an incident doesn't hold up USB requests or GDB packets while
 * the flash is written (about 0.35 s an incident). open() and add() copy what
 * they're given and return at once; list(), read() and remove() see what has
 * been written so far, waiting at most for one file write in progress.
 */
class incident_store
{
public:
    static incident_store *instance()
    {
        static incident_store _instance;
        return &_instance;
    }

    incident_store(incident_store const &) = delete;
    void operator=(incident_store const &) = delete;

    enum section : uint8_t { SEC_CAUSE = 1, SEC_DATA = 2, SEC_OUTCOME = 3, SEC_EVENTS = 4, SEC_CAPTURE = 5 };

    struct header {
        uint8_t kind;
        uint16_t watch_id;
        const char *name;
        const char *trap;
        uint32_t boot;
        uint64_t uptime_us;
        uint64_t utc_ms;
    };

    struct part {
        uint8_t type; // section
        const void *data;
        size_t len;
    };

    esp_err_t init();
    /**
     * Takes the next id and queues the incident, its header and first
     * `count` sections, for writing. 0 when it can't be recorded: the store
     * isn't ready, or the writes queued before it didn't drain in time.
     */
    uint32_t open(const header &h, const part *parts, size_t count);
    /** Queues one more section of incident `id`, written after everything queued before it. */
    esp_err_t add(uint32_t id, uint8_t type, const void *data, size_t len);
    size_t list(uint32_t after_id, si_agent_IncidentInfo *out, size_t max, bool *more);
    esp_err_t read(uint32_t id, uint32_t offset, uint8_t *buf, size_t cap, size_t *len, uint32_t *total);
    esp_err_t remove(uint32_t id); // 0: all

    uint32_t count() const;
    uint32_t newest() const;

    static const constexpr size_t HEADER_SIZE = 96;

private:
    incident_store() = default;

    struct entry {
        uint32_t id;
        uint32_t size;
    };

    static const constexpr size_t MAX_PARTS = 4;
    struct job { // one allocation: this, then the parts' bytes
        uint32_t id;
        bool create;
        uint8_t kind;
        uint16_t watch_id;
        char name[24];
        char trap[24];
        uint32_t boot;
        uint64_t uptime_us;
        uint64_t utc_ms;
        size_t count;
        struct {
            uint8_t type;
            size_t offset;
            size_t len;
        } parts[MAX_PARTS];
        size_t alloc;
        int64_t queued_us;
        uint8_t *bytes() { return reinterpret_cast<uint8_t *>(this + 1); }
    };

    job *make_job(const part *parts, size_t count);
    bool enqueue(job *j);
    static void writer_task(void *arg);
    void write(job &j);
    bool write_new(job &j, uint32_t *total);
    esp_err_t append(uint32_t id, uint8_t type, const void *data, size_t len);

    int find(uint32_t id) const;
    void drop(size_t index);
    void make_room(uint32_t keep_id, size_t need, bool new_entry);
    void save_next_id(uint32_t value);
    void lock() const;
    void unlock() const;

    entry entries[CONFIG_SI_AGENT_INCIDENTS_MAX] = {};
    size_t n = 0;
    uint32_t next_id = 1;
    size_t bytes = 0;
    bool ready = false;

    SemaphoreHandle_t mutex = nullptr; // entries, n, next_id, bytes and the files
    QueueHandle_t queue = nullptr;     // job *
    std::atomic<size_t> pending_bytes{0};

    static const constexpr size_t QUEUE_LEN = 32;
    static const constexpr size_t PENDING_MAX = 256 * 1024; // PSRAM held by queued writes
    static const constexpr uint32_t ENQUEUE_WAIT_MS = 2000; // then the write is dropped
    static const constexpr char DIR_PATH[] = "/data/inc";
    static const constexpr size_t MAX_BYTES = CONFIG_SI_AGENT_INCIDENTS_KB * 1024;
    static const constexpr char *TAG = "incidents";
};

#endif

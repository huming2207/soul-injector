#pragma once

#include <cstddef>
#include <cstdint>

#include <agent.pb.h>
#include <esp_err.h>
#include <sdkconfig.h>

#if CONFIG_SI_JS_ENABLE

class incident_store // SIDP service task only
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

    esp_err_t init();
    uint32_t create(const header &h); // 0 on failure
    esp_err_t append(uint32_t id, uint8_t type, const void *data, size_t len);
    size_t list(uint32_t after_id, si_agent_IncidentInfo *out, size_t max, bool *more);
    esp_err_t read(uint32_t id, uint32_t offset, uint8_t *buf, size_t cap, size_t *len, uint32_t *total);
    esp_err_t remove(uint32_t id); // 0: all

    uint32_t count() const { return n; }
    uint32_t newest() const { return n ? entries[n - 1].id : 0; }

    static const constexpr size_t HEADER_SIZE = 96;

private:
    incident_store() = default;

    struct entry {
        uint32_t id;
        uint32_t size;
    };

    int find(uint32_t id) const;
    void drop(size_t index);
    void make_room(uint32_t keep_id, size_t need, bool new_entry);
    void save_next_id();

    entry entries[CONFIG_SI_AGENT_INCIDENTS_MAX] = {};
    size_t n = 0;
    uint32_t next_id = 1;
    size_t bytes = 0;
    bool ready = false;

    static const constexpr char DIR_PATH[] = "/data/inc";
    static const constexpr size_t MAX_BYTES = CONFIG_SI_AGENT_INCIDENTS_KB * 1024;
    static const constexpr char *TAG = "incidents";
};

#endif

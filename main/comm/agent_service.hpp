#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <agent.pb.h>
#include <esp_err.h>
#include <pb.h>
#include <sidp_defs.hpp>

namespace qjs_binder
{
    struct incident_open;
    enum class incident_part : uint8_t;
}

// Agent sessions and traps, sidp_client's docs/sidp-agent.md. Runs on the SIDP service task, like GDB.
class agent_service
{
public:
    static agent_service *instance()
    {
        static agent_service _instance;
        return &_instance;
    }

    agent_service(agent_service const &) = delete;
    void operator=(agent_service const &) = delete;

    using notify_fn = void (*)(uint32_t newest_seq);

    esp_err_t init(notify_fn notify); // notify: any task, must not block

    static bool is_agent_opcode(uint16_t opcode) { return (opcode & 0xFF00) == 0x0300; }

    std::span<uint8_t> frame_buffer() const { return {frame, frame != nullptr ? sidp::MAX_FRAME_SIZE : 0}; } // empty without PSRAM

    sidp::status_t handle(uint16_t opcode, std::span<const uint8_t> payload, pb_ostream_t &out);

    void service(); // every POLL_MS or so while active()

    void idle(uint32_t ms);

    bool active() const { return open || trap_holds.load(); }

    void close(); // the way OpenRequest.session_end asked

    void connection_closed();

    bool make_way_for_run(); // false: an exclusive trap keeps the target

    bool yield_for_run(uint32_t wait_ms); // programming task

    static const constexpr uint32_t POLL_MS = 5;

private:
    agent_service() = default;

    struct trap_info {
        bool armed;
        si_agent_TrapConfig cfg;
        uint8_t sha256[32];
        si_agent_TrapState state;
        char note[96];
        uint32_t starts;
        uint32_t failures;
        uint32_t suppressed;
        uint32_t hour_count;
        int64_t hour_start_us;
        int64_t retry_us;
        int64_t lost_since_us;
        int64_t running_since_us;
        uint32_t backoff_ms;
        uint32_t first_event_seq; // events up to this one aren't the trap's (an earlier session's)
        bool starting;          // the module was started; waiting for its top level's outcome
        uint32_t start_token;
        int64_t start_until_us; // past it a top level that's still going counts as started
    };

    sidp::status_t op_open(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_info(pb_ostream_t &out);
    sidp::status_t op_run(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_eval(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_upload(std::span<const uint8_t> payload);
    sidp::status_t op_event_read(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_capture_read(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_trap_arm(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_trap_disarm();
    sidp::status_t op_trap_status(pb_ostream_t &out);
    sidp::status_t op_incident_list(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_incident_read(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_incident_delete(std::span<const uint8_t> payload);
    sidp::status_t script_result(esp_err_t ret, pb_ostream_t &out);
    void drop_upload();

    sidp::status_t claim(const si_agent_Permissions &perms, bool connect_under_reset, bool for_trap);
    void release(uint8_t session_end);

    void load_trap();
    esp_err_t save_trap(const uint8_t *module, size_t len);
    void erase_trap();
    void trap_service();
    void trap_start();
    void trap_check_start();
    void trap_pause(const char *why);
    void trap_retry(const char *why, uint32_t first_ms);
    void trap_fail(const char *why);
    void trap_disarm();
    void yield_to_debugger();

    static uint32_t on_incident_open(const qjs_binder::incident_open &inc);
    static void on_incident_add(uint32_t id, qjs_binder::incident_part part, const char *json, size_t len);
    static void on_attach_wanted();
    uint8_t *load_module(size_t *len);
    void drop_trap_hold();
    void fill_status(si_agent_TrapStatus &st);
    uint32_t incident_open(const qjs_binder::incident_open &inc);
    void incident_add(uint32_t id, qjs_binder::incident_part part, const char *json, size_t len);
    size_t events_json(char *out, size_t cap);

    notify_fn notify = nullptr;
    uint8_t *frame = nullptr;   // PSRAM, MAX_FRAME_SIZE
    void *scratch = nullptr;    // PSRAM, the largest request or response message
    void *events = nullptr;     // PSRAM, EVENTS_PER_READ qjs_binder::event_out
    char *inc_events = nullptr; // PSRAM, INCIDENT_EVENTS_MAX
    uint8_t *upload = nullptr;  // PSRAM, a staged module
    size_t upload_len = 0;
    size_t upload_total = 0;

    bool open = false;
    uint8_t session_end = 0;    // si_agent_SessionEnd
    bool perm_write = false;
    bool perm_flash = false;
    bool perm_reset = false;
    uint32_t cpu_id = 0;
    uint32_t profile = 0;
    bool fpu = false;

    trap_info trap = {};
    uint32_t boot_count = 0;
    std::atomic<bool> trap_holds = false;
    std::atomic<bool> trap_yields = false;
    std::atomic<bool> run_wanted = false;

    static const constexpr size_t EVENTS_PER_READ = 8;
    static const constexpr uint32_t INCIDENT_EVENTS = 16;
    static const constexpr size_t INCIDENT_EVENTS_MAX = 16384;
    static const constexpr char *TAG = "agent";
};

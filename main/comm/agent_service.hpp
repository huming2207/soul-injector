#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <esp_err.h>
#include <pb.h>
#include <sidp_defs.hpp>

// Agent sessions, sidp_client's docs/sidp-agent.md. Runs on the SIDP service task, like GDB.
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

    bool active() const { return open; }

    void close(); // the way OpenRequest.session_end asked

    static const constexpr uint32_t POLL_MS = 5;

private:
    agent_service() = default;

    sidp::status_t op_open(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_info(pb_ostream_t &out);
    sidp::status_t op_run(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_eval(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_upload(std::span<const uint8_t> payload);
    sidp::status_t op_event_read(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_capture_read(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t script_result(esp_err_t ret, pb_ostream_t &out);
    void drop_upload();

    notify_fn notify = nullptr;
    uint8_t *frame = nullptr;   // PSRAM, MAX_FRAME_SIZE
    void *scratch = nullptr;    // PSRAM, the largest request or response message
    void *events = nullptr;     // PSRAM, EVENTS_PER_READ qjs_binder::event_out
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

    static const constexpr size_t EVENTS_PER_READ = 8;
    static const constexpr char *TAG = "agent";
};

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_err.h>
#include <pb.h>
#include <sidp_defs.hpp>

/**
 * SIDP over the USB CDC port: accepts connections and serves the management
 * service (device info, asset upload, jobs). See sidp_client's
 * docs/sidp-management.md for the wire contract.
 *
 * Debug opcodes are answered with STATUS_UNSUPPORTED until a real SWD
 * backend is wired to sidp_session.
 *
 * One task owns the connection; it serves one request at a time, as SIDP v1
 * allows only one outstanding request. The task stack is in internal RAM
 * because asset requests write to the flash filesystem.
 */
class sidp_service
{
public:
    static sidp_service *instance()
    {
        static sidp_service _instance;
        return &_instance;
    }

    sidp_service(sidp_service const &) = delete;
    void operator=(sidp_service const &) = delete;

    /**
     * Bind the CDC port and start the service task. Call after the TinyUSB
     * driver is installed. @p serial must stay valid for the process lifetime.
     */
    esp_err_t init(const char *serial);

private:
    sidp_service() = default;

    static void task_handler(void *ctx);
    void serve_connection();
    void handle_frame(std::span<const uint8_t> frame);
    sidp::status_t dispatch(uint16_t opcode, std::span<const uint8_t> payload, pb_ostream_t &out);
    void send_response(const sidp::msg_header_t &request, sidp::status_t status, size_t payload_len);

    sidp::status_t op_device_info(pb_ostream_t &out);
    sidp::status_t op_asset_begin(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_asset_write(std::span<const uint8_t> payload);
    sidp::status_t op_asset_commit();
    sidp::status_t op_job_set(std::span<const uint8_t> payload);
    sidp::status_t op_job_get(pb_ostream_t &out);
    sidp::status_t op_job_run_once(pb_ostream_t &out);
    sidp::status_t op_job_cancel(std::span<const uint8_t> payload);

    static bool decode(std::span<const uint8_t> payload, const pb_msgdesc_t *fields, void *msg);
    static sidp::status_t encode(pb_ostream_t &out, const pb_msgdesc_t *fields, const void *msg);
    static sidp::status_t to_status(esp_err_t ret);

    const char *serial = "";
    uint8_t *tx_buf = nullptr; // Internal RAM; one response at a time
    TaskHandle_t task = nullptr;

    /** Largest management response frame; manage.pb.h sizes are all far below. */
    static const constexpr size_t TX_BUF_SIZE = 256;
    static const constexpr uint32_t READ_TIMEOUT_MS = 100;
    static const constexpr uint32_t ACCEPT_POLL_MS = 50;
    static const constexpr char *TAG = "sidp_svc";
};

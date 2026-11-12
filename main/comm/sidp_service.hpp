#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_err.h>
#include <esp_pm.h>
#include <manage.pb.h>
#include <pb.h>
#include <sidp_defs.hpp>
#include <sidp_session.hpp>
#include <sidp_transport.hpp>
#include <protocomm_security.h>

#include "wifi_manager.hpp"

/**
 * SIDP over USB CDC, and over WiFi when the network settings enable it:
 * accepts connections and serves the management service (device info, asset
 * upload, jobs, production log, network settings; sidp_client's
 * proto/manage.proto) and the power service (sidp_client's docs/sidp-power.md).
 * Debug opcodes go to a sidp_session driving sidp_swd_backend.
 *
 * Over WiFi the device connects out to the server in its settings and stays
 * connected; the server's "open" and "close" messages start and end each SIDP
 * connection. USB has priority: its first request ends a network connection,
 * and an "open" while USB is in use is refused with "busy". A network
 * connection must log in with the device password before anything else
 * (LOGIN, protocomm Security 2); USB is trusted.
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
     * Bind the CDC port, start networking if it is set up, and start the service
     * task. Call after the TinyUSB driver is installed and NVS is initialised.
     * @p serial must stay valid for the process lifetime.
     */
    esp_err_t init(const char *serial);

private:
    sidp_service() = default;

    static void task_handler(void *ctx);
    esp_err_t start_network();
    bool serve_network();
    void keep_network_connected();
    bool other_link_wins(sidp::transport_intf &serving);
    void set_network_busy(bool busy);
    void get_net_status(si_manage_NetStatus &status);
    void start_login(bool network);
    void end_login();
    void run_session(sidp::transport_intf &transport);
    void serve_connection(sidp::transport_intf &transport, sidp::sidp_session &session);
    void handle_frame(sidp::transport_intf &transport, sidp::sidp_session &session, std::span<const uint8_t> frame);
    sidp::status_t dispatch(uint16_t opcode, std::span<const uint8_t> payload, pb_ostream_t &out);
    void send_response(sidp::transport_intf &transport, const sidp::msg_header_t &request, sidp::status_t status, size_t payload_len);

    sidp::status_t op_device_info(pb_ostream_t &out);
    sidp::status_t op_set_time(std::span<const uint8_t> payload);
    sidp::status_t op_asset_begin(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_asset_write(std::span<const uint8_t> payload);
    sidp::status_t op_asset_commit();
    sidp::status_t op_job_set(std::span<const uint8_t> payload);
    sidp::status_t op_job_get(pb_ostream_t &out);
    sidp::status_t op_job_run_once(pb_ostream_t &out);
    sidp::status_t op_job_cancel(std::span<const uint8_t> payload);
    sidp::status_t op_log_read(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_log_ack(std::span<const uint8_t> payload);
    sidp::status_t op_power_info(pb_ostream_t &out);
    sidp::status_t op_power_start(std::span<const uint8_t> payload, pb_ostream_t &out);
    sidp::status_t op_power_stop(pb_ostream_t &out);
    sidp::status_t op_net_config_get(pb_ostream_t &out);
    sidp::status_t op_net_config_set(std::span<const uint8_t> payload);
    sidp::status_t op_net_status(pb_ostream_t &out);
    sidp::status_t op_login(std::span<const uint8_t> payload, pb_ostream_t &out);

    static bool decode(std::span<const uint8_t> payload, const pb_msgdesc_t *fields, void *msg);
    static sidp::status_t encode(pb_ostream_t &out, const pb_msgdesc_t *fields, const void *msg);
    static sidp::status_t to_status(esp_err_t ret);

    const char *serial = "";
    uint8_t *tx_buf = nullptr; // Internal RAM; one response at a time
    TaskHandle_t task = nullptr;

    // Networking, set up at boot from net_config. Only the service task changes these.
    wifi_manager wifi = {};
    si_manage_NetConfig net_cfg = si_manage_NetConfig_init_zero; // Settings in use until the next boot
    esp_pm_lock_handle_t net_pm_lock = nullptr; // Held during a session over WiFi
    bool net_started = false;
    bool usb_accepted = false;  // USB connected during a network connection; served next
    protocomm_security_handle_t login = nullptr; // Security 2 state of a network connection
    uint32_t login_steps = 0;   // Handshake messages accepted: 2 means logged in
    bool logged_in = false;
    int64_t next_connect_us = 0;
    uint32_t retry_ms = 0;

    /** Largest management response frame; LOG_READ fills it with as many entries as fit. */
    static const constexpr size_t TX_BUF_SIZE = 1024;
    static const constexpr uint32_t READ_TIMEOUT_MS = 100;
    static const constexpr uint32_t RUNNING_POLL_MS = 10;
    static const constexpr uint32_t ACCEPT_POLL_MS = 50;
    static const constexpr uint32_t CLEANUP_RETRY_MS = 500;
    static const constexpr uint32_t CLEANUP_ATTEMPTS = 6;
    // Connecting to the server: the first attempt after a drop is immediate, then the wait doubles up to the maximum.
    static const constexpr uint32_t RETRY_MIN_MS = 500;
    static const constexpr uint32_t RETRY_MAX_MS = 3000;
    static const constexpr int NET_TIMEOUT_MS = 5000;
    // A dead link is noticed within about 20 s, so a target is not held for long.
    static const constexpr int PONG_TIMEOUT_SEC = 20;
    static const constexpr int KEEP_ALIVE_IDLE_SEC = 5;
    static const constexpr int KEEP_ALIVE_INTERVAL_SEC = 5;
    static const constexpr int KEEP_ALIVE_COUNT = 3;
    static const constexpr uint32_t LOGIN_SESSION_ID = 1;
    // Each wrong password costs this long, which makes guessing slow.
    static const constexpr uint32_t LOGIN_FAIL_DELAY_MS = 1000;
    static const constexpr char *TAG = "sidp_svc";
};

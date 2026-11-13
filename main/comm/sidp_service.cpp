#include "sidp_service.hpp"

#include <cstring>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <manage.pb.h>
#include <pb_decode.h>
#include <power.pb.h>
#include <pb_encode.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include <sidp_transport_cdc.hpp>
#include <sidp_transport_websocket.hpp>
#include <esp_transport_tcp.h>
#include <esp_transport_ws.h>
#include <protocomm_security2.h>

#include <atomic>

#include "asset_store.hpp"
#include "job_controller.hpp"
#include "net_config.hpp"
#include "power_profiler.hpp"
#include "prog_log.hpp"
#include "sidp_swd_backend.hpp"
#include "swd_prog.hpp"

// The connection being served. Set by the service task around each connection;
// the power sampler's sinks read it from their own task.
static std::atomic<sidp::transport_intf *> active_transport{nullptr};

static bool send_debug_frame(std::span<const uint8_t> frame)
{
    auto *transport = active_transport.load();
    return transport != nullptr && transport->write_message(frame) == ESP_OK;
}

// Power samples share the log queue: they are dropped, never queued ahead of debug traffic.
static esp_err_t send_power_data(std::span<const uint8_t> frame)
{
    auto *transport = active_transport.load();
    return transport != nullptr ? transport->write_message_log(frame) : ESP_ERR_INVALID_STATE;
}

static esp_err_t send_power_event(std::span<const uint8_t> frame)
{
    auto *transport = active_transport.load();
    return transport != nullptr ? transport->write_message(frame) : ESP_ERR_INVALID_STATE;
}

esp_err_t sidp_service::init(const char *_serial)
{
    serial = _serial;
    tx_buf = static_cast<uint8_t *>(heap_caps_malloc(TX_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (tx_buf == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    auto ret = sidp::cdc_slip_transport::instance().init(TINYUSB_CDC_ACM_0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init: CDC transport failed: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    // USB works without the network, so a network failure is only logged.
    ret = start_network();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "init: networking unavailable: %s", esp_err_to_name(ret));
    }

    // Boards without a profiler answer the power opcodes with STATUS_UNSUPPORTED.
    ret = power_profiler::instance()->init(send_power_data, send_power_event);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "init: power profiler unavailable: %s", esp_err_to_name(ret));
    }

    // Debug requests use SWD, so this shares the flasher's core. It sits above the
    // flasher in priority so that requests are still answered during a run.
    if (xTaskCreatePinnedToCore(task_handler, "sidp_svc", 8192, this, tskIDLE_PRIORITY + 11, &task, swd_def::TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "init: cannot create task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---- Networking ----
// Without CONFIG_SI_NETWORK_ENABLE these do nothing, so WiFi and the WebSocket
// client stay out of the app.

#ifdef CONFIG_SI_NETWORK_ENABLE

esp_err_t sidp_service::start_network()
{
    // The WiFi station MAC: always 6 bytes (the default MAC is 8 on chips with 802.15.4).
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, sizeof(device_id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    auto ret = net_config::read(net_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!net_cfg.enabled) {
        ESP_LOGI(TAG, "start_network: networking is off");
        return ESP_OK;
    }

    ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "net", &net_pm_lock);
    ret = ret ?: wifi.init(net_cfg.wifi_ssid, net_cfg.wifi_password);
    if (ret != ESP_OK) {
        return ret;
    }

    // Our own TCP and WebSocket transport, so the SIDP transport can reach the
    // socket and turn Nagle's algorithm off on it.
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    esp_transport_handle_t ws_layer = tcp != nullptr ? esp_transport_ws_init(tcp) : nullptr;
    if (ws_layer == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_transport_keep_alive_t keep_alive = {
        .keep_alive_enable = true,
        .keep_alive_idle = KEEP_ALIVE_IDLE_SEC,
        .keep_alive_interval = KEEP_ALIVE_INTERVAL_SEC,
        .keep_alive_count = KEEP_ALIVE_COUNT,
    };
    esp_transport_tcp_set_keep_alive(tcp, &keep_alive);
    // With its own transport the client ignores the URL's path and the headers in
    // its config, so they go to the transport. The agent knows the device by its ID.
    const char *host = strstr(net_cfg.server_url, "://");
    const char *path = host != nullptr ? strchr(host + 3, '/') : nullptr;
    snprintf(ws_headers, sizeof(ws_headers), "X-SIDP-Device: %s\r\n", device_id);
    esp_transport_ws_config_t ws_layer_cfg = {};
    ws_layer_cfg.ws_path = path != nullptr ? path : "/";
    ws_layer_cfg.headers = ws_headers;
    ws_layer_cfg.propagate_control_frames = true; // As the client sets for its own transport
    ret = esp_transport_ws_set_config(ws_layer, &ws_layer_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    // The first attempt fails without an IP address; keep_network_connected() retries.
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = net_cfg.server_url;
    ws_cfg.ext_transport = ws_layer;
    ws_cfg.network_timeout_ms = NET_TIMEOUT_MS;
    ws_cfg.pingpong_timeout_sec = PONG_TIMEOUT_SEC;
    ret = sidp::websocket_transport::instance().init(ws_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    net_started = true;
    ESP_LOGI(TAG, "start_network: server %s, device ID %s", net_cfg.server_url, device_id);
    return ESP_OK;
}

bool sidp_service::serve_network()
{
    if (!net_started) {
        return false;
    }

    // The WebSocket stays connected; the server's "open" starts each connection.
    auto &ws = sidp::websocket_transport::instance();
    if (ws.begin_session() == ESP_OK) {
        ESP_LOGI(TAG, "network connection accepted");
        ws.finish("ready");
        run_session(ws);
        // Also tells the agent that USB took over, when it did.
        ws.finish("closed");
        ESP_LOGI(TAG, "network connection closed");
        return true;
    }

    keep_network_connected();
    return false;
}

void sidp_service::keep_network_connected()
{
    auto &ws = sidp::websocket_transport::instance();
    if (ws.server_connected()) {
        retry_ms = 0;
        return;
    }
    if (!wifi.has_ip() || ws.is_connecting()) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (now < next_connect_us) {
        return;
    }
    if (ws.reconnect() == ESP_OK) {
        retry_ms = retry_ms == 0 ? RETRY_MIN_MS : retry_ms * 2;
        retry_ms = retry_ms > RETRY_MAX_MS ? RETRY_MAX_MS : retry_ms;
    }
    next_connect_us = now + (int64_t)retry_ms * 1000;
}

bool sidp_service::other_link_wins(sidp::transport_intf &serving)
{
    if (!net_started) {
        return false;
    }

    auto &usb = sidp::cdc_slip_transport::instance();
    auto &ws = sidp::websocket_transport::instance();
    if (&serving == &usb) {
        // USB has priority: an "open" from the network is refused at once.
        if (ws.link_is_connected()) {
            ESP_LOGI(TAG, "network connection refused: USB is in use");
            ws.finish("busy");
        }
        return false;
    }

    // USB takes over on its first request, not on DTR alone: software that only
    // opens the port, such as ModemManager probing it, must not end the session.
    if (!usb_accepted && usb.begin_session() == ESP_OK) {
        usb_accepted = true;
    }
    if (usb_accepted && usb.has_request()) {
        ESP_LOGW(TAG, "USB takes over from the network connection");
        return true;
    }
    return false;
}

void sidp_service::set_network_busy(bool busy)
{
    // Light sleep and modem power save would make every request slower.
    if (busy) {
        esp_pm_lock_acquire(net_pm_lock);
        wifi.set_power_save(false);
    } else {
        wifi.set_power_save(true);
        esp_pm_lock_release(net_pm_lock);
    }
}

void sidp_service::start_login(bool network)
{
    login_steps = 0;
    logged_in = !network; // USB is trusted
    if (network && protocomm_security2.init(&login) == ESP_OK) {
        protocomm_security2.new_transport_session(login, LOGIN_SESSION_ID);
    }
}

void sidp_service::end_login()
{
    if (login != nullptr) {
        protocomm_security2.close_transport_session(login, LOGIN_SESSION_ID);
        protocomm_security2.cleanup(login);
        login = nullptr;
    }
    logged_in = false;
}

sidp::status_t sidp_service::op_login(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    if (login == nullptr) {
        return sidp::STATUS_UNSUPPORTED; // USB needs no login
    }

    const protocomm_security2_params_t params = {
        .salt = reinterpret_cast<const char *>(net_cfg.password_salt.bytes),
        .salt_len = static_cast<uint16_t>(net_cfg.password_salt.size),
        .verifier = reinterpret_cast<const char *>(net_cfg.password_verifier.bytes),
        .verifier_len = static_cast<uint16_t>(net_cfg.password_verifier.size),
    };
    uint8_t *reply = nullptr;
    ssize_t reply_len = 0;
    auto ret = protocomm_security2.security_req_handler(login, &params, LOGIN_SESSION_ID, payload.data(), (ssize_t)payload.size(), &reply, &reply_len, nullptr);
    if (ret != ESP_OK) {
        // A wrong password, or messages out of order: start over.
        ESP_LOGW(TAG, "op_login: login failed");
        protocomm_security2.close_transport_session(login, LOGIN_SESSION_ID);
        protocomm_security2.new_transport_session(login, LOGIN_SESSION_ID);
        login_steps = 0;
        vTaskDelay(pdMS_TO_TICKS(LOGIN_FAIL_DELAY_MS));
        return sidp::STATUS_UNAUTHORIZED;
    }

    const bool written = pb_write(&out, reply, reply_len);
    free(reply); // Allocated by the handler
    if (!written) {
        return sidp::STATUS_ERROR;
    }
    // The handler takes command 0, then command 1, which checks the proof.
    logged_in = ++login_steps == 2;
    if (logged_in) {
        ESP_LOGI(TAG, "op_login: logged in");
    }
    return sidp::STATUS_OK;
}

void sidp_service::get_net_status(si_manage_NetStatus &status)
{
    strlcpy(status.device_id, device_id, sizeof(status.device_id));
    if (!net_started) {
        return;
    }

    auto &ws = sidp::websocket_transport::instance();
    if (!wifi.has_ip()) {
        status.state = si_manage_NetState_NET_STATE_WIFI_CONNECTING;
    } else if (!ws.server_connected()) {
        status.state = si_manage_NetState_NET_STATE_SERVER_CONNECTING;
    } else {
        status.state = si_manage_NetState_NET_STATE_CONNECTED;
    }
    strlcpy(status.ip, wifi.has_ip() ? wifi.get_ip() : "", sizeof(status.ip));
    status.rssi = wifi.get_rssi();
    status.wifi_reason = wifi.get_last_reason();
    status.network_session = active_transport.load() == &ws;
}

#else

esp_err_t sidp_service::start_network()
{
    return ESP_OK;
}

bool sidp_service::serve_network()
{
    return false;
}

void sidp_service::keep_network_connected()
{
}

bool sidp_service::other_link_wins(sidp::transport_intf &)
{
    return false;
}

void sidp_service::set_network_busy(bool)
{
}

void sidp_service::get_net_status(si_manage_NetStatus &)
{
}

void sidp_service::start_login(bool)
{
    logged_in = true;
}

void sidp_service::end_login()
{
}

sidp::status_t sidp_service::op_login(std::span<const uint8_t>, pb_ostream_t &)
{
    return sidp::STATUS_UNSUPPORTED;
}

#endif

void sidp_service::task_handler(void *ctx)
{
    auto *svc = static_cast<sidp_service *>(ctx);
    auto &usb = sidp::cdc_slip_transport::instance();
    while (true) {
        // Accepts only a new connection: DTR asserted again, or "open" from the server.
        // USB may already be accepted, when it took over from the network.
        if (svc->usb_accepted || usb.begin_session() == ESP_OK) {
            svc->usb_accepted = false;
            ESP_LOGI(TAG, "USB connection accepted");
            svc->run_session(usb);
            ESP_LOGI(TAG, "USB connection closed");
        } else if (!svc->serve_network()) {
            vTaskDelay(pdMS_TO_TICKS(ACCEPT_POLL_MS));
        }
    }
}

void sidp_service::run_session(sidp::transport_intf &transport)
{
    const bool network = &transport != &sidp::cdc_slip_transport::instance();
    active_transport.store(&transport);
    start_login(network);
    if (network) {
        set_network_busy(true);
    }

    // One debug session per connection. Its buffers are allocated here and freed on return.
    sidp::sidp_session session(*sidp_swd_backend::instance(), send_debug_frame);
    if (session.init() == ESP_OK) {
        serve_connection(transport, session);
    } else {
        ESP_LOGE(TAG, "run_session: no memory for the debug session");
    }

    // A measurement belongs to its connection.
    si_power_StopResponse stopped = si_power_StopResponse_init_zero;
    power_profiler::instance()->stop(stopped);
    transport.close_session();

    // The target stays owned until it is cleaned up, so nothing else may use it before then.
    // Firmware that sleeps or turns SWD off may never answer, so stop waiting after a while.
    uint32_t attempts = 0;
    while (!session.handle_disconnect()) {
        if (++attempts >= CLEANUP_ATTEMPTS) {
            ESP_LOGW(TAG, "run_session: target is not answering, abandoning it");
            sidp_swd_backend::instance()->abandon();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(CLEANUP_RETRY_MS));
    }

    end_login();
    active_transport.store(nullptr);
    if (network) {
        set_network_busy(false);
    }
}

void sidp_service::serve_connection(sidp::transport_intf &transport, sidp::sidp_session &session)
{
    while (!transport.needs_disconnect() && !session.needs_disconnect()) {
        // A running target has to be checked often so that a stop is reported promptly.
        const uint32_t timeout_ms = session.get_state() == sidp::TARGET_RUNNING ? RUNNING_POLL_MS : READ_TIMEOUT_MS;

        uint8_t *frame = nullptr;
        size_t frame_len = 0;
        auto ret = transport.start_read(&frame, &frame_len, timeout_ms);
        if (ret == ESP_OK) {
            handle_frame(transport, session, std::span<const uint8_t>(frame, frame_len));
            transport.end_read(frame);
        } else if (ret != ESP_ERR_TIMEOUT) {
            return;
        }

        session.handle_poll();
        if (other_link_wins(transport)) {
            return;
        }
    }
}

void sidp_service::handle_frame(sidp::transport_intf &transport, sidp::sidp_session &session, std::span<const uint8_t> frame)
{
    const auto &header = *reinterpret_cast<const sidp::msg_header_t *>(frame.data());
    if (header.version != sidp::PROTOCOL_VERSION) {
        ESP_LOGE(TAG, "handle_frame: unsupported protocol version %u", header.version);
        transport.close_session();
        return;
    }
    if (header.kind != sidp::KIND_REQUEST || header.request_id == 0) {
        return; // Only requests are valid from the peer.
    }
    if (!logged_in && header.opcode != sidp::OP_LOGIN) {
        send_response(transport, header, sidp::STATUS_UNAUTHORIZED, 0);
        return;
    }

    if (header.opcode < sidp::MANAGEMENT_OPCODE_BASE) {
        session.handle_request(frame);
        return;
    }

    constexpr size_t prefix_len = sizeof(sidp::msg_header_t) + sizeof(sidp::response_prefix_t);
    pb_ostream_t out = pb_ostream_from_buffer(tx_buf + prefix_len, TX_BUF_SIZE - prefix_len);
    auto status = dispatch(header.opcode, frame.subspan(sizeof(sidp::msg_header_t)), out);
    send_response(transport, header, status, status == sidp::STATUS_OK ? out.bytes_written : 0);
}

sidp::status_t sidp_service::dispatch(uint16_t opcode, std::span<const uint8_t> payload, pb_ostream_t &out)
{
    switch (opcode) {
    case sidp::OP_DEVICE_INFO:
        return op_device_info(out);
    case sidp::OP_SET_TIME:
        return op_set_time(payload);
    case sidp::OP_ASSET_BEGIN:
        return op_asset_begin(payload, out);
    case sidp::OP_ASSET_WRITE:
        return op_asset_write(payload);
    case sidp::OP_ASSET_COMMIT:
        return op_asset_commit();
    case sidp::OP_JOB_SET:
        return op_job_set(payload);
    case sidp::OP_JOB_GET:
        return op_job_get(out);
    case sidp::OP_JOB_RUN_ONCE:
        return op_job_run_once(out);
    case sidp::OP_JOB_CANCEL:
        return op_job_cancel(payload);
    case sidp::OP_LOG_READ:
        return op_log_read(payload, out);
    case sidp::OP_LOG_ACK:
        return op_log_ack(payload);
    case sidp::OP_POWER_INFO:
        return op_power_info(out);
    case sidp::OP_POWER_START:
        return op_power_start(payload, out);
    case sidp::OP_POWER_STOP:
        return op_power_stop(out);
    case sidp::OP_NET_CONFIG_GET:
        return op_net_config_get(out);
    case sidp::OP_NET_CONFIG_SET:
        return op_net_config_set(payload);
    case sidp::OP_NET_STATUS:
        return op_net_status(out);
    case sidp::OP_LOGIN:
        return op_login(payload, out);
    default:
        return sidp::STATUS_UNSUPPORTED;
    }
}

void sidp_service::send_response(sidp::transport_intf &transport, const sidp::msg_header_t &request, sidp::status_t status, size_t payload_len)
{
    auto &header = *reinterpret_cast<sidp::msg_header_t *>(tx_buf);
    header.version = sidp::PROTOCOL_VERSION;
    header.kind = sidp::KIND_RESPONSE;
    header.opcode = request.opcode;
    header.request_id = request.request_id;
    auto &prefix = *reinterpret_cast<sidp::response_prefix_t *>(tx_buf + sizeof(sidp::msg_header_t));
    prefix.status = status;

    if (status != sidp::STATUS_OK) {
        ESP_LOGW(TAG, "request %lu op 0x%04x: status %ld", request.request_id, request.opcode, (long)status);
    }

    std::span<uint8_t> response(tx_buf, sizeof(sidp::msg_header_t) + sizeof(sidp::response_prefix_t) + payload_len);
    if (!sidp::crc32_hasher::set_message_crc(response) || transport.write_message(response) != ESP_OK) {
        // The transport latches itself closed; serve_connection() then exits.
        ESP_LOGE(TAG, "send_response: cannot queue response");
    }
}

bool sidp_service::decode(std::span<const uint8_t> payload, const pb_msgdesc_t *fields, void *msg)
{
    pb_istream_t stream = pb_istream_from_buffer(payload.data(), payload.size());
    if (!pb_decode(&stream, fields, msg)) {
        ESP_LOGE(TAG, "decode: %s", PB_GET_ERROR(&stream));
        return false;
    }
    return true;
}

sidp::status_t sidp_service::encode(pb_ostream_t &out, const pb_msgdesc_t *fields, const void *msg)
{
    if (!pb_encode(&out, fields, msg)) {
        ESP_LOGE(TAG, "encode: %s", PB_GET_ERROR(&out));
        return sidp::STATUS_ERROR;
    }
    return sidp::STATUS_OK;
}

sidp::status_t sidp_service::to_status(esp_err_t ret)
{
    switch (ret) {
    case ESP_OK:
        return sidp::STATUS_OK;
    case job_controller::ERR_RUNNING:
        return sidp::STATUS_BUSY;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
    case ESP_ERR_INVALID_CRC:
    case ESP_ERR_NOT_ALLOWED:
    case ESP_ERR_NOT_FOUND:
        return sidp::STATUS_INVALID_ARGUMENT;
    case ESP_ERR_NOT_SUPPORTED:
        return sidp::STATUS_UNSUPPORTED;
    default:
        return sidp::STATUS_ERROR;
    }
}

sidp::status_t sidp_service::op_device_info(pb_ostream_t &out)
{
    si_manage_DeviceInfo info = si_manage_DeviceInfo_init_zero;
    strlcpy(info.serial, serial, sizeof(info.serial));
    strlcpy(info.firmware_version, esp_app_get_description()->version, sizeof(info.firmware_version));

    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info(asset_store::BASE_PATH, &total_bytes, &free_bytes) == ESP_OK) {
        info.storage_total_kb = total_bytes / 1024;
        info.storage_free_kb = free_bytes / 1024;
    }
    prog_log::instance()->get_info(info);
    return encode(out, si_manage_DeviceInfo_fields, &info);
}

sidp::status_t sidp_service::op_set_time(std::span<const uint8_t> payload)
{
    si_manage_SetTimeRequest request = si_manage_SetTimeRequest_init_zero;
    if (!decode(payload, si_manage_SetTimeRequest_fields, &request)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    return to_status(prog_log::instance()->set_time(request.utc_ms));
}

sidp::status_t sidp_service::op_asset_begin(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    si_manage_AssetBeginRequest request = si_manage_AssetBeginRequest_init_zero;
    if (!decode(payload, si_manage_AssetBeginRequest_fields, &request)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }

    si_manage_AssetBeginResponse response = si_manage_AssetBeginResponse_init_zero;
    auto ret = asset_store::instance()->begin(request.name, request.size, request.sha256, &response.present, &response.offset);
    if (ret != ESP_OK) {
        return to_status(ret);
    }
    return encode(out, si_manage_AssetBeginResponse_fields, &response);
}

sidp::status_t sidp_service::op_asset_write(std::span<const uint8_t> payload)
{
    if (payload.size() <= sizeof(sidp::asset_write_request_t)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }

    uint32_t offset = 0;
    memcpy(&offset, payload.data(), sizeof(offset));
    auto data = payload.subspan(sizeof(sidp::asset_write_request_t));
    return to_status(asset_store::instance()->write(offset, data.data(), data.size()));
}

sidp::status_t sidp_service::op_asset_commit()
{
    return to_status(job_controller::instance()->commit_asset());
}

sidp::status_t sidp_service::op_job_set(std::span<const uint8_t> payload)
{
    si_manage_JobSetRequest request = si_manage_JobSetRequest_init_zero;
    if (!decode(payload, si_manage_JobSetRequest_fields, &request)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    return to_status(job_controller::instance()->set_job(request.sha256, request.trigger));
}

sidp::status_t sidp_service::op_job_get(pb_ostream_t &out)
{
    si_manage_JobStatus status = si_manage_JobStatus_init_zero;
    job_controller::instance()->get_status(status);
    return encode(out, si_manage_JobStatus_fields, &status);
}

sidp::status_t sidp_service::op_job_run_once(pb_ostream_t &out)
{
    si_manage_JobRunOnceResponse response = si_manage_JobRunOnceResponse_init_zero;
    auto ret = job_controller::instance()->request_run(&response.run_id);
    if (ret == job_controller::ERR_LOG_FULL) {
        return sidp::STATUS_LOG_FULL;
    }
    if (ret != ESP_OK) {
        return to_status(ret);
    }
    return encode(out, si_manage_JobRunOnceResponse_fields, &response);
}

sidp::status_t sidp_service::op_job_cancel(std::span<const uint8_t> payload)
{
    si_manage_JobCancelRequest request = si_manage_JobCancelRequest_init_zero;
    if (!decode(payload, si_manage_JobCancelRequest_fields, &request)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    job_controller::instance()->cancel(request.run_id);
    return sidp::STATUS_OK;
}

sidp::status_t sidp_service::op_log_read(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    si_manage_LogReadRequest request = si_manage_LogReadRequest_init_zero;
    if (!decode(payload, si_manage_LogReadRequest_fields, &request)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    return to_status(prog_log::instance()->read(request.after_id, out));
}

sidp::status_t sidp_service::op_log_ack(std::span<const uint8_t> payload)
{
    si_manage_LogAckRequest request = si_manage_LogAckRequest_init_zero;
    if (!decode(payload, si_manage_LogAckRequest_fields, &request)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    return to_status(prog_log::instance()->ack(request.up_to_id));
}

sidp::status_t sidp_service::op_power_info(pb_ostream_t &out)
{
    auto *profiler = power_profiler::instance();
    if (!profiler->available()) {
        return sidp::STATUS_UNSUPPORTED;
    }

    si_power_PowerInfo info = si_power_PowerInfo_init_zero;
    profiler->get_info(info);
    return encode(out, si_power_PowerInfo_fields, &info);
}

sidp::status_t sidp_service::op_power_start(std::span<const uint8_t> payload, pb_ostream_t &out)
{
    si_power_StartRequest request = si_power_StartRequest_init_zero;
    if (!decode(payload, si_power_StartRequest_fields, &request)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }

    si_power_StartResponse response = si_power_StartResponse_init_zero;
    auto ret = power_profiler::instance()->start(request, response);
    if (ret == ESP_ERR_INVALID_STATE) {
        return sidp::STATUS_BUSY;
    }
    if (ret != ESP_OK) {
        return to_status(ret);
    }
    return encode(out, si_power_StartResponse_fields, &response);
}

sidp::status_t sidp_service::op_power_stop(pb_ostream_t &out)
{
    si_power_StopResponse response = si_power_StopResponse_init_zero;
    power_profiler::instance()->stop(response);
    return encode(out, si_power_StopResponse_fields, &response);
}

sidp::status_t sidp_service::op_net_config_get(pb_ostream_t &out)
{
    si_manage_NetConfig cfg = si_manage_NetConfig_init_zero;
    auto ret = net_config::read(cfg);
    if (ret != ESP_OK) {
        return to_status(ret);
    }
    // Write-only
    memset(cfg.wifi_password, 0, sizeof(cfg.wifi_password));
    cfg.password_salt.size = 0;
    cfg.password_verifier.size = 0;
    return encode(out, si_manage_NetConfig_fields, &cfg);
}

sidp::status_t sidp_service::op_net_config_set(std::span<const uint8_t> payload)
{
    // Whoever reaches the device over the network must not be able to point it elsewhere.
    if (active_transport.load() != &sidp::cdc_slip_transport::instance()) {
        ESP_LOGW(TAG, "op_net_config_set: refused over the network");
        return sidp::STATUS_UNSUPPORTED;
    }

    si_manage_NetConfig cfg = si_manage_NetConfig_init_zero;
    if (!decode(payload, si_manage_NetConfig_fields, &cfg)) {
        return sidp::STATUS_INVALID_ARGUMENT;
    }
    return to_status(net_config::write(cfg));
}

sidp::status_t sidp_service::op_net_status(pb_ostream_t &out)
{
    si_manage_NetStatus status = si_manage_NetStatus_init_zero;
    get_net_status(status);
    return encode(out, si_manage_NetStatus_fields, &status);
}

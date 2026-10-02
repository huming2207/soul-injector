#include "sidp_service.hpp"

#include <cstring>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <manage.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <sidp_transport_cdc.hpp>

#include "asset_store.hpp"
#include "job_controller.hpp"
#include "prog_log.hpp"

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

    if (xTaskCreate(task_handler, "sidp_svc", 6144, this, tskIDLE_PRIORITY + 5, &task) != pdPASS) {
        ESP_LOGE(TAG, "init: cannot create task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void sidp_service::task_handler(void *ctx)
{
    auto *svc = static_cast<sidp_service *>(ctx);
    auto &transport = sidp::cdc_slip_transport::instance();
    while (true) {
        // Accepts only a new physical connection (DTR asserted again).
        if (transport.begin_session() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(ACCEPT_POLL_MS));
            continue;
        }

        ESP_LOGI(TAG, "connection accepted");
        svc->serve_connection();
        transport.close_session();
        ESP_LOGI(TAG, "connection closed");
    }
}

void sidp_service::serve_connection()
{
    auto &transport = sidp::cdc_slip_transport::instance();
    while (!transport.needs_disconnect()) {
        uint8_t *frame = nullptr;
        size_t frame_len = 0;
        auto ret = transport.start_read(&frame, &frame_len, READ_TIMEOUT_MS);
        if (ret == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (ret != ESP_OK) {
            return;
        }

        handle_frame(std::span<const uint8_t>(frame, frame_len));
        transport.end_read(frame);
    }
}

void sidp_service::handle_frame(std::span<const uint8_t> frame)
{
    const auto &header = *reinterpret_cast<const sidp::msg_header_t *>(frame.data());
    if (header.version != sidp::PROTOCOL_VERSION) {
        ESP_LOGE(TAG, "handle_frame: unsupported protocol version %u", header.version);
        sidp::cdc_slip_transport::instance().close_session();
        return;
    }
    if (header.kind != sidp::KIND_REQUEST || header.request_id == 0) {
        return; // Only requests are valid from the peer.
    }

    constexpr size_t prefix_len = sizeof(sidp::msg_header_t) + sizeof(sidp::response_prefix_t);
    pb_ostream_t out = pb_ostream_from_buffer(tx_buf + prefix_len, TX_BUF_SIZE - prefix_len);
    auto status = dispatch(header.opcode, frame.subspan(sizeof(sidp::msg_header_t)), out);
    send_response(header, status, status == sidp::STATUS_OK ? out.bytes_written : 0);
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
    default:
        // Includes every debug opcode until sidp_session gets an SWD backend.
        return sidp::STATUS_UNSUPPORTED;
    }
}

void sidp_service::send_response(const sidp::msg_header_t &request, sidp::status_t status, size_t payload_len)
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
    auto &transport = sidp::cdc_slip_transport::instance();
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

#include <cstring>

#include <esp_flash.h>
#include <esp_crc.h>
#include <esp_mac.h>
#include <esp_log.h>

#include "comm_fsm.hpp"
#include "file_utils.hpp"

esp_err_t comm_fsm::init(comm_interface *_interface)
{
    if (_interface == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    comm_if = _interface;

    if (xTaskCreatePinnedToCore(rx_handler_task, "cdc_rx", 16384, this, tskIDLE_PRIORITY + 1, nullptr, 0) != pdPASS) {
        return ESP_ERR_NOT_FINISHED;
    }

    return ESP_OK;
}

esp_err_t comm_fsm::send_ack(uint16_t crc, uint32_t timeout_ms)
{
    return send_pkt(comm_def::PKT_ACK, nullptr, 0);
}

esp_err_t comm_fsm::send_nack(uint32_t timeout_ms)
{
    return send_pkt(comm_def::PKT_NACK, nullptr, 0);
}

esp_err_t comm_fsm::send_dev_info(uint32_t timeout_ms)
{
    const char *idf_ver = IDF_VER;
    const char *dev_model = SI_DEVICE_MODEL;
    const char *dev_build = SI_DEVICE_BUILD;

    comm_def::device_info dev_info = {};
    auto ret = esp_efuse_mac_get_default(dev_info.mac_addr);
    ret = ret ?: esp_flash_read_unique_chip_id(esp_flash_default_chip, (uint64_t *)dev_info.flash_id);
    strcpy(dev_info.esp_idf_ver, idf_ver);
    strcpy(dev_info.dev_build, dev_build);
    strcpy(dev_info.dev_model, dev_model);

    ret = ret ?: send_pkt(comm_def::PKT_DEVICE_INFO, (uint8_t *)&dev_info, sizeof(dev_info), timeout_ms);

    return ret;
}

esp_err_t comm_fsm::send_chunk_ack(comm_def::chunk_ack state, uint32_t aux, uint32_t timeout_ms)
{
    comm_def::chunk_ack_pkt pkt = {};
    pkt.aux_info = aux;
    pkt.state = state;

    return send_pkt(comm_def::PKT_CHUNK_ACK, (uint8_t *)&pkt, sizeof(pkt), timeout_ms);
}

esp_err_t comm_fsm::send_pkt(comm_def::pkt_type type, const uint8_t *buf, size_t len, uint32_t timeout_tick)
{
    if (buf == nullptr && len > 0) return ESP_ERR_INVALID_ARG;

    comm_def::header header = {};
    header.type = type;
    header.len = len;
    header.crc = 0; // Set later
    uint16_t crc = get_crc16((uint8_t *) &header, sizeof(header));

    // When packet has no data body, just send header (e.g. ACK)
    if (buf == nullptr || len < 1) {
        header.crc = crc;
        return send_buf_with_header((uint8_t *) &header, sizeof(header), nullptr, 0, timeout_tick);
    } else {
        crc = get_crc16(buf, len, crc);
        header.crc = crc;
        return send_buf_with_header((uint8_t *) &header, sizeof(header), buf, len, timeout_tick);
    }
}

esp_err_t comm_fsm::send_buf_with_header(const uint8_t *header_buf, size_t header_len,
                                        const uint8_t *buf, size_t len, uint32_t timeout_tick)
{
    if (header_buf == nullptr || header_len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    auto ret = comm_if->encode_and_send(header_buf, header_len, true, false, timeout_tick);
    ret = ret ?: comm_if->encode_and_send(buf, len, false, true, timeout_tick);

    return ret;
}

uint16_t comm_fsm::get_crc16(const uint8_t *buf, size_t len, uint16_t init)
{
//  * CRC-16/XMODEM, poly= 0x1021, init = 0x0000, refin = false, refout = false, xorout = 0x0000
// *     crc = ~crc16_be((uint16_t)~0x0000, buf, length);
    if (buf == nullptr || len < 1) {
        return 0;
    }

    return ~esp_crc16_be((uint16_t)~init, buf, len);
}

void comm_fsm::rx_handler_task(void *_ctx)
{
    ESP_LOGI(TAG, "Rx handler task started");
    auto *ctx = comm_fsm::instance();
    while(true) {
        if (ctx->comm_if == nullptr) {
            vTaskDelay(1);
            return;
        }

        auto ret = ctx->comm_if->wait_for_recv(portMAX_DELAY);
        ret = ret ?: ctx->comm_if->pause_recv();
        ret = ret ?: ctx->comm_if->acquire_read_buf(&ctx->rx_buf_ptr, &ctx->rx_buf_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Read comm if failed: 0x%x", ret);
            ctx->comm_if->release_read_buf(ctx->rx_buf_len);
            ctx->comm_if->resume_recv();
            return;
        } else {
            ctx->parse_pkt();
        }

        ctx->comm_if->release_read_buf(ctx->rx_buf_len);
        ctx->comm_if->resume_recv();
        ctx->rx_buf_len = 0;
        ctx->rx_buf_ptr = nullptr;
    }
}


void comm_fsm::parse_pkt()
{
    if (comm_if == nullptr) {
        return;
    }

    if (rx_buf_len < sizeof(comm_def::header)) {
        ESP_LOGW(TAG, "Packet too short, failed to decode header: %u", rx_buf_len);
        send_nack();
        return;
    }

    auto *header = (comm_def::header *)rx_buf_ptr;

    uint16_t expected_crc = header->crc;
    header->crc = 0;

    uint16_t actual_crc = get_crc16(rx_buf_ptr, rx_buf_len);
    if (actual_crc != expected_crc) {
        ESP_LOGW(TAG, "Incoming packet CRC corrupted, expect 0x%x, actual 0x%x", expected_crc, actual_crc);
        send_nack();
        return;
    }

    if (expect_file_chunk && header->type != comm_def::PKT_DATA_CHUNK) {
        ESP_LOGW(TAG, "Invalid state - data chunk expected while received type 0x%x", header->type);
        send_nack();
        return;
    }

    switch (header->type) {
        case comm_def::PKT_PING: {
            send_ack();
            break;
        }

        case comm_def::PKT_DEVICE_INFO: {
            send_dev_info();
            break;
        }

        case comm_def::PKT_STORE_FILE: {
            handle_store_file_req();
            break;
        }

        case comm_def::PKT_FETCH_FILE:{
            handle_fetch_file_req();
            break;
        }

        case comm_def::PKT_DATA_CHUNK: {
            if (expect_file_chunk) {
                handle_file_chunk();
            } else {
                ESP_LOGW(TAG, "Invalid state - no chunk expected to come or should have EOL'ed??");
                send_chunk_ack(comm_def::CHUNK_ERR_INTERNAL, 0);
            }
            break;
        }

        case comm_def::PKT_DELETE_FILE: {
            handle_delete_file();
            break;
        }

        case comm_def::PKT_GET_FILE_INFO: {
            handle_get_file_info();
            break;
        }

        case comm_def::PKT_NUKE_STORAGE: {
            handle_nuke_storage();
            break;
        }

        default: {
            ESP_LOGW(TAG, "Unknown packet type 0x%x received", header->type);
            send_nack();
            break;
        }
    }
}

void comm_fsm::handle_fetch_file_req()
{

}

void comm_fsm::handle_store_file_req()
{
    uint8_t *buf = (rx_buf_ptr + sizeof(comm_def::header));
    auto *fw_info = (comm_def::file_attr_info *)(buf);

    file_handle = fopen(fw_info->name, "wb");
    if (file_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to open firmware path");
        memset(file_name, 0, sizeof(file_name));
        return;
    }

    memset(file_name, 0, sizeof(file_name));
    strncpy(file_name, fw_info->name, fw_info->name_len);
    file_name[sizeof(file_name) - 1] = '\0';

    file_expect_len = fw_info->len;
    file_crc = fw_info->crc;
    file_curr_offset = 0;

    expect_file_chunk = true;
    send_chunk_ack(comm_def::CHUNK_XFER_NEXT, 0);
}

void comm_fsm::handle_file_chunk()
{
    uint8_t *buf = (rx_buf_ptr + sizeof(comm_def::header));

    auto *chunk = (comm_def::chunk_pkt *)(buf);

    // Scenario 0: if len == 0 then that's force abort, discard the buffer and set back the states
    if (chunk->len == 0) {
        ESP_LOGE(TAG, "Zero len chunk - force abort!");
        file_expect_len = 0;
        file_curr_offset = 0;
        file_crc = 0;

        expect_file_chunk = false;
        send_chunk_ack(comm_def::CHUNK_ERR_ABORT_REQUESTED, 0);
        return;
    }

    // Scenario 1: if len is too long, reject & abort.
    if (chunk->len + file_curr_offset > file_expect_len) {
        ESP_LOGE(TAG, "Chunk recv buffer is full, incoming %u while expect %u only", chunk->len + file_curr_offset, file_expect_len);
        file_expect_len = 0;
        file_curr_offset = 0;
        file_crc = 0;

        if (file_handle != nullptr) {
            fclose(file_handle);
            file_handle = nullptr;
        }

        expect_file_chunk = false;
        send_chunk_ack(comm_def::CHUNK_ERR_NAME_TOO_LONG, chunk->len + file_curr_offset);
        return;
    }

    // Scenario 2: Normal recv
    if (fwrite(chunk->buf, 1, chunk->len, file_handle) < chunk->len) {
        ESP_LOGE(TAG, "Error occur when processing recv buffer - write failed");
        send_chunk_ack(comm_def::CHUNK_ERR_INTERNAL, ESP_ERR_NO_MEM);
        return;
    }

    file_curr_offset += chunk->len; // Add offset

    // When file write finishes, check CRC, and clean up
    if (file_curr_offset == file_expect_len) {
        if (file_utils::validate_file_crc32(file_name, file_crc) == ESP_OK) {
            ESP_LOGI(TAG, "Chunk recv successful, got %u bytes", file_expect_len);

            file_expect_len = 0;
            file_curr_offset = 0;
            file_crc = 0;
            expect_file_chunk = false;
            memset(file_name, 0, sizeof(file_name));
            send_chunk_ack(comm_def::CHUNK_XFER_DONE, file_curr_offset);
        } else {
            ESP_LOGE(TAG, "Chunk recv CRC mismatched!");
            send_chunk_ack(comm_def::CHUNK_ERR_CRC32_FAIL, 0);
        }
    } else {
        ESP_LOGI(TAG, "Chunk recv - await next @ %u, total %u", file_curr_offset, file_expect_len);
        send_chunk_ack(comm_def::CHUNK_XFER_NEXT, file_curr_offset);
    }
}

void comm_fsm::handle_get_file_info()
{

}

void comm_fsm::handle_delete_file()
{

}

void comm_fsm::handle_nuke_storage()
{

}

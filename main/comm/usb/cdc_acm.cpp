#include <esp_log.h>
#include <esp_crc.h>
#include <esp_flash.h>
#include <esp_mac.h>

#include "cdc_acm.hpp"
#include "config_manager.hpp"
#include "file_utils.hpp"

uint8_t cdc_acm::rx_raw_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE] = {};

esp_err_t cdc_acm::init(tinyusb_cdcacm_itf_t channel)
{
    cdc_channel = channel;
    static char sn_str[32] = {};
    static char lang[2] = {0x09, 0x04};

    static char *desc_str[5] = {
            lang,                // 0: is supported language is English (0x0409)
            const_cast<char *>(USB_DESC_MANUFACTURER), // 1: Manufacturer
            const_cast<char *>(USB_DESC_PRODUCT),      // 2: Product
            sn_str,       // 3: Serials, should use chip ID
            const_cast<char *>(USB_DESC_CDC_NAME),          // 4: CDC Interface
    };

    tinyusb_config_t tusb_cfg = {}; // the configuration using default values
    tusb_cfg.string_descriptor = (const char **)desc_str;
    tusb_cfg.device_descriptor = nullptr;
    tusb_cfg.self_powered = false;
    tusb_cfg.external_phy = false;

    uint8_t sn_buf[16] = { 0 };
    esp_efuse_mac_get_default(sn_buf);
    esp_flash_read_unique_chip_id(esp_flash_default_chip, reinterpret_cast<uint64_t *>(sn_buf + 6));

    snprintf(sn_str, 32, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             sn_buf[0], sn_buf[1], sn_buf[2], sn_buf[3], sn_buf[4], sn_buf[5], sn_buf[6], sn_buf[7],
             sn_buf[8], sn_buf[9], sn_buf[10], sn_buf[11], sn_buf[12], sn_buf[13]);

    ESP_LOGI(TAG, "Initialised with SN: %s", sn_str);

    auto ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB driver install failed");
        return ret;
    }

    tinyusb_config_cdcacm_t acm_cfg = {};
    acm_cfg.usb_dev = TINYUSB_USBDEV_0;
    acm_cfg.cdc_port = cdc_channel;
    acm_cfg.rx_unread_buf_sz = 512;
    acm_cfg.callback_rx = &serial_rx_cb;
    acm_cfg.callback_rx_wanted_char = nullptr;
    acm_cfg.callback_line_state_changed = nullptr;
    acm_cfg.callback_line_coding_changed = nullptr;

    ret = ret ?: tusb_cdc_acm_init(&acm_cfg);

    rx_event = xEventGroupCreate();
    if (rx_event == nullptr) {
        ESP_LOGE(TAG, "Failed to create Rx event group");
        return ESP_ERR_NO_MEM;
    }

    xTaskCreatePinnedToCore(rx_handler_task, "cdc_rx", 16384, this, tskIDLE_PRIORITY + 1, nullptr, 0);

    return ret;
}

void cdc_acm::serial_rx_cb(int itf, cdcacm_event_t *event)
{
    auto *ctx = cdc_acm::instance();
    if (ctx->cdc_channel != itf) {
        return;
    }

    size_t rx_size = 0;
    auto ret = tinyusb_cdcacm_read(static_cast<tinyusb_cdcacm_itf_t>(itf), rx_raw_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB read fail: 0x%x", ret);
        memset(rx_raw_buf, 0, sizeof(rx_raw_buf));
        return;
    }

    if (rx_size < 1) {
        memset(rx_raw_buf, 0, sizeof(rx_raw_buf));
        return;
    }

    // Start de-SLIP
    size_t idx = 0;
    while (idx < std::min(rx_size, (size_t)CONFIG_TINYUSB_CDC_RX_BUFSIZE)) {
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed when de-SLIP: 0x%x", ret);
            return;
        }

        switch (rx_raw_buf[idx]) {
            case SLIP_START: {
                xEventGroupClearBits(ctx->rx_event, cdc_def::EVT_NEW_PACKET);
                xEventGroupSetBits(ctx->rx_event, cdc_def::EVT_READING_SLIP_FRAME);

                idx += 1;
                break;
            }

            case SLIP_ESC: {
                // During receive: if it's not started, just ignore it
                if ((xEventGroupGetBits(ctx->rx_event) & cdc_def::EVT_READING_SLIP_FRAME) == 0) {
                    break;
                }

                // Skip the ESC byte
                idx += 1;

                // Handle the second bytes
                switch (rx_raw_buf[idx]) {
                    case SLIP_ESC_START: {
                        ret = ctx->write_to_rx_buf(SLIP_START);
                        break;
                    }

                    case SLIP_ESC_END: {
                        ret = ctx->write_to_rx_buf(SLIP_END);
                        break;
                    }

                    case SLIP_ESC_ESC: {
                        ret = ctx->write_to_rx_buf(SLIP_ESC);
                        break;
                    }

                    default: {
                        xEventGroupClearBits(ctx->rx_event, cdc_def::EVT_READING_SLIP_FRAME);
                        xEventGroupClearBits(ctx->rx_event, cdc_def::EVT_NEW_PACKET);
                        ESP_LOGE(TAG, "Unexpected SLIP ESC: 0x%02x", rx_raw_buf[idx]);
                        return;
                    }
                }

                idx += 1;
                break;
            }

            case SLIP_END: {
                // During receive: if it's not started, just ignore it
                if ((xEventGroupGetBits(ctx->rx_event) & cdc_def::EVT_READING_SLIP_FRAME) == 0) {
                    break;
                }

                xEventGroupSetBits(ctx->rx_event, cdc_def::EVT_NEW_PACKET);
                xEventGroupClearBits(ctx->rx_event, cdc_def::EVT_READING_SLIP_FRAME);

                break;
            }

            default: {
                // During receive: if it's not started, just ignore it
                if ((xEventGroupGetBits(ctx->rx_event) & cdc_def::EVT_READING_SLIP_FRAME) == 0) {
                    break;
                }

                ret = ctx->write_to_rx_buf(rx_raw_buf[idx]);
                break;
            }
        }
    }

    memset(rx_raw_buf, 0, sizeof(rx_raw_buf));
}

[[noreturn]] void cdc_acm::rx_handler_task(void *_ctx)
{
    ESP_LOGI(TAG, "Rx handler task started");
    auto *ctx = cdc_acm::instance();
    while(true) {
        if (xEventGroupWaitBits(ctx->rx_event, cdc_def::EVT_NEW_PACKET, pdTRUE, pdFALSE, portMAX_DELAY) == pdTRUE) {
            // Pause Rx
            tinyusb_cdcacm_unregister_callback(ctx->cdc_channel, CDC_EVENT_RX);

            auto len = ctx->rx_buf_bb.ReadAcquire().second;
            ctx->rx_buf_bb.ReadRelease(0);
            ESP_LOGI(TAG, "Now in buffer, len: %u", len);

            // Now do parsing
            ctx->parse_pkt();

            // Restart Rx
            tinyusb_cdcacm_register_callback(ctx->cdc_channel, CDC_EVENT_RX, serial_rx_cb);
        }
    }
}

esp_err_t cdc_acm::send_ack(uint16_t crc, uint32_t timeout_ms)
{
    return send_pkt(cdc_def::PKT_ACK, nullptr, 0);
}

esp_err_t cdc_acm::send_nack(uint32_t timeout_ms)
{
    return send_pkt(cdc_def::PKT_NACK, nullptr, 0);
}

esp_err_t cdc_acm::send_dev_info(uint32_t timeout_ms)
{
    const char *idf_ver = IDF_VER;
    const char *dev_model = SI_DEVICE_MODEL;
    const char *dev_build = SI_DEVICE_BUILD;

    cdc_def::device_info dev_info = {};
    auto ret = esp_efuse_mac_get_default(dev_info.mac_addr);
    ret = ret ?: esp_flash_read_unique_chip_id(esp_flash_default_chip, (uint64_t *)dev_info.flash_id);
    strcpy(dev_info.esp_idf_ver, idf_ver);
    strcpy(dev_info.dev_build, dev_build);
    strcpy(dev_info.dev_model, dev_model);

    ret = ret ?: send_pkt(cdc_def::PKT_DEVICE_INFO, (uint8_t *)&dev_info, sizeof(dev_info), timeout_ms);

    return ret;
}

esp_err_t cdc_acm::send_chunk_ack(cdc_def::chunk_ack state, uint32_t aux, uint32_t timeout_ms)
{
    cdc_def::chunk_ack_pkt pkt = {};
    pkt.aux_info = aux;
    pkt.state = state;

    return send_pkt(cdc_def::PKT_CHUNK_ACK, (uint8_t *)&pkt, sizeof(pkt), timeout_ms);
}

esp_err_t cdc_acm::send_pkt(cdc_def::pkt_type type, const uint8_t *buf, size_t len, uint32_t timeout_tick)
{
    if (buf == nullptr && len > 0) return ESP_ERR_INVALID_ARG;

    cdc_def::header header = {};
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

esp_err_t cdc_acm::send_buf_with_header(const uint8_t *header_buf, size_t header_len,
                                        const uint8_t *buf, size_t len, uint32_t timeout_tick)
{
    if (header_buf == nullptr || header_len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    auto ret = encode_and_send(header_buf, header_len, true, false, timeout_tick);
    ret = ret ?: encode_and_send(buf, len, false, true, timeout_tick);

    return ret;
}

uint16_t cdc_acm::get_crc16(const uint8_t *buf, size_t len, uint16_t init)
{
//  * CRC-16/XMODEM, poly= 0x1021, init = 0x0000, refin = false, refout = false, xorout = 0x0000
// *     crc = ~crc16_be((uint16_t)~0x0000, buf, length);
    if (buf == nullptr || len < 1) {
        return 0;
    }

    return ~esp_crc16_be((uint16_t)~init, buf, len);
}

void cdc_acm::parse_pkt()
{
    auto queue_ptr = rx_buf_bb.ReadAcquire();
    size_t decoded_len = queue_ptr.second;

    if (decoded_len < sizeof(cdc_def::header)) {
        ESP_LOGW(TAG, "Packet too short, failed to decode header: %u", decoded_len);
        recv_state = cdc_def::FILE_RECV_NONE;
        send_nack();

        rx_buf_bb.ReadRelease(decoded_len);
        return;
    }

    auto *header = (cdc_def::header *)queue_ptr.first;

    uint16_t expected_crc = header->crc;
    header->crc = 0;

    uint16_t actual_crc = get_crc16(queue_ptr.first, decoded_len);
    if (actual_crc != expected_crc) {
        ESP_LOGW(TAG, "Incoming packet CRC corrupted, expect 0x%x, actual 0x%x", expected_crc, actual_crc);
        send_nack();
        rx_buf_bb.ReadRelease(decoded_len);
        return;
    }

    if (recv_state != cdc_def::FILE_RECV_NONE && header->type != cdc_def::PKT_DATA_CHUNK) {
        ESP_LOGW(TAG, "Invalid state - data chunk expected while received type 0x%x", header->type);
        send_nack();
        rx_buf_bb.ReadRelease(decoded_len);
        return;
    }

    rx_buf_bb.ReadRelease(sizeof(cdc_def::header));

    switch (header->type) {
        case cdc_def::PKT_PING: {
            send_ack();
            break;
        }

        case cdc_def::PKT_DEVICE_INFO: {
            send_dev_info();
            break;
        }

        case cdc_def::PKT_STORE_FILE: {
            handle_store_file_req();
            break;
        }

        case cdc_def::PKT_FETCH_FILE:{
            handle_fetch_file_req();
            break;
        }

        case cdc_def::PKT_DATA_CHUNK: {
            if (recv_state != cdc_def::FILE_RECV_NONE) {
                parse_chunk();
            } else {
                ESP_LOGW(TAG, "Invalid state - no chunk expected to come or should have EOL'ed??");
                send_chunk_ack(cdc_def::CHUNK_ERR_INTERNAL, 0);
            }
            break;
        }

        default: {
            ESP_LOGW(TAG, "Unknown packet type 0x%x received", header->type);
            send_nack();
            break;
        }
    }
}

void cdc_acm::handle_fetch_file_req()
{

}

void cdc_acm::handle_store_file_req()
{
    auto queue_ptr = rx_buf_bb.ReadAcquire();
    uint8_t *buf = queue_ptr.first;
    size_t buf_len = queue_ptr.second;

    auto *fw_info = (cdc_def::fw_info *)(buf);
    if (fw_info->len > CFG_MGR_FW_MAX_SIZE || heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < fw_info->len) {
        ESP_LOGE(TAG, "Firmware metadata len too long: %lu, free heap: %u", fw_info->len, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        heap_caps_dump(MALLOC_CAP_INTERNAL);
        send_nack();
        rx_buf_bb.ReadRelease(buf_len);
        return;
    }

    file_expect_len = fw_info->len;
    file_crc = fw_info->crc;

    memset(file_name, 0, sizeof(file_name));
    strncpy(file_name, fw_info->name, fw_info->name_len);
    file_name[sizeof(file_name) - 1] = '\0';

    file_handle = fopen(file_name, "wb");
    if (file_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to open firmware path");
        memset(file_name, 0, sizeof(file_name));
        rx_buf_bb.ReadRelease(buf_len);
        return;
    }
    recv_state = cdc_def::FILE_RECV_FW;
    send_chunk_ack(cdc_def::CHUNK_XFER_NEXT, 0);
    rx_buf_bb.ReadRelease(buf_len);
}

void cdc_acm::parse_chunk()
{
    auto queue_ptr = rx_buf_bb.ReadAcquire();
    uint8_t *buf = queue_ptr.first;
    size_t buf_len = queue_ptr.second;

    auto *chunk = (cdc_def::chunk_pkt *)(buf);

    // Scenario 0: if len == 0 then that's force abort, discard the buffer and set back the states
    if (chunk->len == 0) {
        ESP_LOGE(TAG, "Zero len chunk - force abort!");
        file_expect_len = 0;
        file_curr_offset = 0;
        file_crc = 0;

        recv_state = cdc_def::FILE_RECV_NONE;
        send_chunk_ack(cdc_def::CHUNK_ERR_ABORT_REQUESTED, 0);
        rx_buf_bb.ReadRelease(buf_len);
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

        recv_state = cdc_def::FILE_RECV_NONE;
        send_chunk_ack(cdc_def::CHUNK_ERR_NAME_TOO_LONG, chunk->len + file_curr_offset);
        rx_buf_bb.ReadRelease(buf_len);
        return;
    }

    // Scenario 2: Normal recv
    if (fwrite(chunk->buf, 1, chunk->len, file_handle) < chunk->len) {
        ESP_LOGE(TAG, "Error occur when processing recv buffer - write failed");
        send_chunk_ack(cdc_def::CHUNK_ERR_INTERNAL, ESP_ERR_NO_MEM);
        rx_buf_bb.ReadRelease(buf_len);
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
            recv_state = cdc_def::FILE_RECV_NONE;
            memset(file_name, 0, sizeof(file_name));
            send_chunk_ack(cdc_def::CHUNK_XFER_DONE, file_curr_offset);
        } else {
            ESP_LOGE(TAG, "Chunk recv CRC mismatched!");
            send_chunk_ack(cdc_def::CHUNK_ERR_CRC32_FAIL, 0);
        }
    } else {
        ESP_LOGI(TAG, "Chunk recv - await next @ %u, total %u", file_curr_offset, file_expect_len);
        send_chunk_ack(cdc_def::CHUNK_XFER_NEXT, file_curr_offset);
    }

    rx_buf_bb.ReadRelease(buf_len);
}

esp_err_t cdc_acm::pause_recv()
{
    if (!tusb_inited() || paused) return ESP_ERR_INVALID_STATE;
    xEventGroupClearBits(rx_event, cdc_def::EVT_NEW_PACKET);

    paused = true;
    return tinyusb_cdcacm_unregister_callback(cdc_channel, CDC_EVENT_RX);
}

esp_err_t cdc_acm::resume_recv()
{
    if (!tusb_inited() || !paused) return ESP_ERR_INVALID_STATE;
    rx_buf_bb.ReadRelease(rx_buf_bb.ReadAcquire().second);
    paused = false;
    return tinyusb_cdcacm_register_callback(cdc_channel, CDC_EVENT_RX, serial_rx_cb);
}

esp_err_t cdc_acm::encode_and_send(const uint8_t *buf, size_t len, bool send_start, bool send_end, uint32_t timeout_ticks)
{
    const uint8_t slip_esc_start[] = { SLIP_ESC, SLIP_ESC_START };
    const uint8_t slip_esc_end[] = { SLIP_ESC, SLIP_ESC_END };
    const uint8_t slip_esc_esc[] = { SLIP_ESC, SLIP_ESC_ESC };

    if (buf == nullptr || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t start = SLIP_START;
    const uint8_t end = SLIP_END;

    if (send_start) {
        if (tinyusb_cdcacm_write_queue(cdc_channel, &start, 1) < 1) {
            ESP_LOGE(TAG, "Failed to encode and tx start char");
            return ESP_ERR_NOT_FINISHED;
        }
    }

    size_t idx = 0;
    while (idx < len) {
        switch (buf[idx]) {
            case SLIP_START: {
                if (tinyusb_cdcacm_write_queue(cdc_channel, slip_esc_start, sizeof(slip_esc_end)) < sizeof(slip_esc_end)) {
                    ESP_LOGE(TAG, "Failed to encode and tx SLIP_START");
                    return ESP_ERR_NOT_FINISHED;
                }

                break;
            }

            case SLIP_ESC: {
                if (tinyusb_cdcacm_write_queue(cdc_channel, slip_esc_esc, sizeof(slip_esc_esc)) < sizeof(slip_esc_esc)) {
                    ESP_LOGE(TAG, "Failed to encode and tx SLIP_ESC");
                    return ESP_ERR_NOT_FINISHED;
                }

                break;
            }

            case SLIP_END: {
                if (tinyusb_cdcacm_write_queue(cdc_channel, slip_esc_end, sizeof(slip_esc_end)) < sizeof(slip_esc_end)) {
                    ESP_LOGE(TAG, "Failed to encode and tx SLIP_END");
                    return ESP_ERR_NOT_FINISHED;
                }

                break;
            }

            default: {
                if (tinyusb_cdcacm_write_queue(cdc_channel, &buf[idx], 1) < 1) {
                    ESP_LOGE(TAG, "Failed to encode and tx data");
                    return ESP_ERR_NOT_FINISHED;
                }

                break;
            }
        }

        idx += 1;
    }

    if (send_end) {
        if (tinyusb_cdcacm_write_queue(cdc_channel, &end, 1) < 1) {
            ESP_LOGE(TAG, "Failed to encode and tx end char");
            return ESP_ERR_NOT_FINISHED;
        }
    }

    return tinyusb_cdcacm_write_flush(cdc_channel, timeout_ticks);
}

esp_err_t cdc_acm::write_to_rx_buf(uint8_t data)
{
    uint8_t *buf = rx_buf_bb.WriteAcquire(1);
    if (buf == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    *buf = data;
    rx_buf_bb.WriteRelease(1);
    return ESP_OK;
}

esp_err_t cdc_acm::decode_and_recv(uint8_t *buf, size_t buf_len, size_t *len_decoded, uint32_t timeout_ticks)
{
    if (buf == nullptr || buf_len < 1 || len_decoded == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto ret = xEventGroupWaitBits(rx_event, cdc_def::EVT_NEW_PACKET, pdTRUE, pdFALSE, timeout_ticks);
    if ((ret & cdc_def::EVT_NEW_PACKET) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    auto queue_ptr = rx_buf_bb.ReadAcquire();
    size_t actual_len = std::min(buf_len, queue_ptr.second);

    if (queue_ptr.first == nullptr) {
        return ESP_ERR_NO_MEM; // Do we need this??
    }

    memcpy(buf, queue_ptr.first, actual_len);
    *len_decoded = actual_len;

    rx_buf_bb.WriteRelease(actual_len);
    return ESP_OK;
}

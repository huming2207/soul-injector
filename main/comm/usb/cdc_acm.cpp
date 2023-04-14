#include <esp_log.h>
#include <esp_crc.h>
#include <esp_flash.h>
#include <esp_mac.h>

#include "cdc_acm.hpp"
#include "config_manager.hpp"
#include "file_utils.hpp"


esp_err_t cdc_acm::init()
{
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
    acm_cfg.cdc_port = TINYUSB_CDC_ACM_0;
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

    uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE] = { 0 };
    size_t rx_size = 0;
    auto ret = tinyusb_cdcacm_read(static_cast<tinyusb_cdcacm_itf_t>(itf), rx_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB read fail: 0x%x", ret);
        return;
    }

    if (rx_size < 1) {
        return;
    }

    // Start parsing
    size_t idx = 0;
    while (idx < std::min(rx_size, (size_t)CONFIG_TINYUSB_CDC_RX_BUFSIZE)) {
        switch (rx_buf[idx]) {
            case SLIP_START: {
                xEventGroupClearBits(ctx->rx_event, cdc_def::EVT_NEW_PACKET);
                xEventGroupSetBits(ctx->rx_event, cdc_def::EVT_READING_PKT);

                idx += 1;
                break;
            }

            case SLIP_ESC: {
                // During receive: if it's not started, just ignore it
                if ((xEventGroupGetBits(ctx->rx_event) & cdc_def::EVT_READING_PKT) == 0) {
                    break;
                }

                // Skip the ESC byte
                idx += 1;

                // Handle the second bytes
                switch (rx_buf[idx]) {
                    case SLIP_ESC_START: {
                        uint8_t *buf = ctx->rx_buf_bb.WriteAcquire(1);
                        *buf = SLIP_START;
                        ctx->rx_buf_bb.WriteRelease(1);
                        break;
                    }

                    case SLIP_ESC_END: {
                        uint8_t *buf = ctx->rx_buf_bb.WriteAcquire(1);
                        *buf = SLIP_END;
                        ctx->rx_buf_bb.WriteRelease(1);
                        break;
                    }

                    case SLIP_ESC_ESC: {
                        uint8_t *buf = ctx->rx_buf_bb.WriteAcquire(1);
                        *buf = SLIP_ESC;
                        ctx->rx_buf_bb.WriteRelease(1);
                        break;
                    }

                    default: {
                        xEventGroupClearBits(ctx->rx_event, cdc_def::EVT_READING_PKT);
                        xEventGroupClearBits(ctx->rx_event, cdc_def::EVT_NEW_PACKET);
                        ESP_LOGE(TAG, "Unexpected SLIP ESC: 0x%02x", rx_buf[idx]);
                        return;
                    }
                }

                idx += 1;
                break;
            }

            case SLIP_END: {
                // During receive: if it's not started, just ignore it
                if ((xEventGroupGetBits(ctx->rx_event) & cdc_def::EVT_READING_PKT) == 0) {
                    break;
                }

                xEventGroupSetBits(ctx->rx_event, cdc_def::EVT_NEW_PACKET);
                xEventGroupClearBits(ctx->rx_event, cdc_def::EVT_READING_PKT);

                break;
            }

            default: {
                // During receive: if it's not started, just ignore it
                if ((xEventGroupGetBits(ctx->rx_event) & cdc_def::EVT_READING_PKT) == 0) {
                    break;
                }

                uint8_t *buf = ctx->rx_buf_bb.WriteAcquire(1);
                *buf = rx_buf[idx];
                ctx->rx_buf_bb.WriteRelease(1);
                break;
            }
        }
    }
}

[[noreturn]] void cdc_acm::rx_handler_task(void *_ctx)
{
    ESP_LOGI(TAG, "Rx handler task started");
    auto *ctx = cdc_acm::instance();
    while(true) {
        if (xEventGroupWaitBits(ctx->rx_event, cdc_def::EVT_NEW_PACKET, pdTRUE, pdFALSE, portMAX_DELAY) == pdTRUE) {
            // Pause Rx
            tinyusb_cdcacm_unregister_callback(TINYUSB_CDC_ACM_0, CDC_EVENT_RX);

            auto len = ctx->rx_buf_bb.ReadAcquire().second;
            ctx->rx_buf_bb.ReadRelease(0);
            ESP_LOGI(TAG, "Now in buffer, len: %u", len);

            // Now do parsing
            ctx->parse_pkt();

            // Restart Rx
            tinyusb_cdcacm_register_callback(TINYUSB_CDC_ACM_0, CDC_EVENT_RX, serial_rx_cb);
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

    auto ret = encode_slip_and_tx(header_buf, header_len, true, false, timeout_tick);
    ret = ret ?: encode_slip_and_tx(buf, len, false, true, timeout_tick);

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

        case cdc_def::PKT_CURR_CONFIG: {
            send_curr_config();
            break;
        }

        case cdc_def::PKT_SET_CONFIG: {
            parse_set_config();
            break;
        }

        case cdc_def::PKT_SET_ALGO_METADATA: {
            parse_set_algo_metadata();
            break;
        }

        case cdc_def::PKT_GET_ALGO_METADATA: {
            parse_get_algo_info();
            break;
        }

        case cdc_def::PKT_SET_FW_METADATA: {
            parse_set_fw_metadata();
            break;
        }

        case cdc_def::PKT_GET_FW_METADATA:{
            parse_get_fw_info();
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

void cdc_acm::send_curr_config()
{
    auto &cfg_mgr = config_manager::instance();

    if (!cfg_mgr.has_valid_cfg()) {
        if(cfg_mgr.load_default_cfg() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load default config");
            send_nack();
            return;
        }
    }

    uint8_t buf[sizeof(cfg_def::config_pkt)] = { 0 };
    cfg_mgr.read_cfg(buf, sizeof(cfg_def::config_pkt));
    send_pkt(cdc_def::PKT_CURR_CONFIG, buf, sizeof(cfg_def::config_pkt));
}

void cdc_acm::parse_set_config()
{
    auto queue_ptr = rx_buf_bb.ReadAcquire();
    uint8_t *buf = queue_ptr.first;
    size_t buf_len = queue_ptr.second;

    auto &cfg_mgr = config_manager::instance();
    auto ret = cfg_mgr.save_cfg(buf, buf_len);

    rx_buf_bb.ReadRelease(buf_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set config failed, returned 0x%x: %s", ret, esp_err_to_name(ret));
        send_nack();
    } else {
        send_ack();
    }
}

void cdc_acm::parse_get_algo_info()
{

}

void cdc_acm::parse_set_algo_metadata()
{
    auto queue_ptr = rx_buf_bb.ReadAcquire();
    uint8_t *buf = queue_ptr.first;
    size_t buf_len = queue_ptr.second;

    auto *algo_info = (cdc_def::algo_info *)(buf);
    if (algo_info->len > CFG_MGR_FLASH_ALGO_MAX_SIZE || heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < algo_info->len) {
        ESP_LOGE(TAG, "Flash algo metadata len too long: %lu, free block: %u", algo_info->len, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        send_nack();
        return;
    }

    file_expect_len = algo_info->len;
    file_crc = algo_info->crc;
    algo_buf = static_cast<uint8_t *>(heap_caps_malloc(algo_info->len, MALLOC_CAP_INTERNAL));
    memset(algo_buf, 0, algo_info->len);
    recv_state = cdc_def::FILE_RECV_ALGO;
    send_chunk_ack(cdc_def::CHUNK_XFER_NEXT, 0);

    rx_buf_bb.ReadRelease(buf_len);
}

void cdc_acm::parse_get_fw_info()
{

}

void cdc_acm::parse_set_fw_metadata()
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
    file_handle = fopen(config_manager::FIRMWARE_PATH, "wb");
    if (file_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to open firmware path");
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
        if (algo_buf != nullptr) {
            free(algo_buf);
            algo_buf = nullptr;
        }
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
        if (algo_buf != nullptr) {
            free(algo_buf);
            algo_buf = nullptr;
        }

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
    if (recv_state == cdc_def::FILE_RECV_ALGO) {
        ESP_LOGD(TAG, "Copy to: %p; len: %u, off: %u, base: %p", algo_buf + file_curr_offset, chunk->len, file_curr_offset, algo_buf);
        memcpy(algo_buf + file_curr_offset, chunk->buf, chunk->len);
        file_curr_offset += chunk->len; // Add offset
    } else if(recv_state == cdc_def::FILE_RECV_FW) {
        if (fwrite(chunk->buf, 1, chunk->len, file_handle) < chunk->len) {
            ESP_LOGE(TAG, "Error occur when processing recv buffer - write failed");
            send_chunk_ack(cdc_def::CHUNK_ERR_INTERNAL, ESP_ERR_NO_MEM);
            rx_buf_bb.ReadRelease(buf_len);
            return;
        }

        fflush(file_handle);
        file_curr_offset += chunk->len; // Add offset
    }

    if (file_curr_offset == file_expect_len) {
        bool crc_match = false;
        if (recv_state == cdc_def::FILE_RECV_ALGO) {
            crc_match = (esp_crc32_le(0, algo_buf, file_expect_len) == file_crc);
        } else if(recv_state == cdc_def::FILE_RECV_FW) {
            if (file_handle != nullptr) {
                fflush(file_handle);
                fclose(file_handle);
                file_handle = nullptr;
            }

            crc_match = (file_utils::validate_firmware_file(config_manager::FIRMWARE_PATH, file_crc) == ESP_OK);
        }

        if (crc_match) {
            ESP_LOGI(TAG, "Chunk recv successful, got %u bytes", file_expect_len);

            auto ret = ESP_OK;
            auto &cfg_mgr = config_manager::instance();
            if (recv_state == cdc_def::FILE_RECV_ALGO) {
                ret = cfg_mgr.save_algo(algo_buf, file_expect_len);

                if (algo_buf != nullptr) {
                    free(algo_buf);
                    algo_buf = nullptr;
                }
            } else if(recv_state == cdc_def::FILE_RECV_FW) {
                ret = cfg_mgr.set_fw_crc(file_crc);
            }

            file_expect_len = 0;
            file_curr_offset = 0;
            file_crc = 0;
            recv_state = cdc_def::FILE_RECV_NONE;

            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Error occur when processing recv buffer, returned 0x%x: %s", ret, esp_err_to_name(ret));
                send_chunk_ack(cdc_def::CHUNK_ERR_INTERNAL, ret);
            } else {
                ESP_LOGI(TAG, "Chunk transfer done!");
                send_chunk_ack(cdc_def::CHUNK_XFER_DONE, file_curr_offset);
            }
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

esp_err_t cdc_acm::pause_usb()
{
    if (!tusb_inited() || paused) return ESP_ERR_INVALID_STATE;
    xEventGroupClearBits(rx_event, cdc_def::EVT_NEW_PACKET);

    paused = true;
    return tinyusb_cdcacm_unregister_callback(TINYUSB_CDC_ACM_0, CDC_EVENT_RX);
}

esp_err_t cdc_acm::unpause_usb()
{
    if (!tusb_inited() || !paused) return ESP_ERR_INVALID_STATE;
    rx_buf_bb.ReadRelease(rx_buf_bb.ReadAcquire().second);
    paused = false;
    return tinyusb_cdcacm_register_callback(TINYUSB_CDC_ACM_0, CDC_EVENT_RX, serial_rx_cb);
}

esp_err_t cdc_acm::encode_slip_and_tx(const uint8_t *buf, size_t len, bool send_start, bool send_end, uint32_t timeout_ticks)
{
    const uint8_t slip_esc_start[] = { SLIP_ESC, SLIP_ESC_START };
    const uint8_t slip_esc_end[] = { SLIP_ESC, SLIP_ESC_START };
    const uint8_t slip_esc_esc[] = { SLIP_ESC, SLIP_ESC_ESC };

    if (buf == nullptr || len < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t start = SLIP_START;
    const uint8_t end = SLIP_END;

    if (send_start) {
        if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, &start, 1) < 1) {
            ESP_LOGE(TAG, "Failed to encode and tx start char");
            return ESP_ERR_NOT_FINISHED;
        }
    }

    size_t idx = 0;
    while (idx < len) {
        switch (buf[idx]) {
            case SLIP_START: {
                if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, slip_esc_end, sizeof(slip_esc_end)) < sizeof(slip_esc_end)) {
                    ESP_LOGE(TAG, "Failed to encode and tx SLIP_START");
                    return ESP_ERR_NOT_FINISHED;
                }

                break;
            }

            case SLIP_ESC: {
                if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, slip_esc_esc, sizeof(slip_esc_esc)) < sizeof(slip_esc_esc)) {
                    ESP_LOGE(TAG, "Failed to encode and tx SLIP_ESC");
                    return ESP_ERR_NOT_FINISHED;
                }

                break;
            }

            case SLIP_END: {
                if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, slip_esc_end, sizeof(slip_esc_end)) < sizeof(slip_esc_end)) {
                    ESP_LOGE(TAG, "Failed to encode and tx SLIP_END");
                    return ESP_ERR_NOT_FINISHED;
                }

                break;
            }

            default: {
                if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, &buf[idx], 1) < 1) {
                    ESP_LOGE(TAG, "Failed to encode and tx data");
                    return ESP_ERR_NOT_FINISHED;
                }

                break;
            }
        }

        idx += 1;
    }

    if (send_end) {
        if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, &end, 1) < 1) {
            ESP_LOGE(TAG, "Failed to encode and tx end char");
            return ESP_ERR_NOT_FINISHED;
        }
    }

    return tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, timeout_ticks);
}





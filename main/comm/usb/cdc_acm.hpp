#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <tinyusb.h>
#include <tusb_cdc_acm.h>
#include <esp_err.h>

#include "lfbb.hpp"

#define SLIP_START              0xa5
#define SLIP_END                0xc0
#define SLIP_ESC                0xdb
#define SLIP_ESC_START          0xdc
#define SLIP_ESC_ESC            0xdd
#define SLIP_ESC_END            0xde

#ifndef CONFIG_SI_DEVICE_MODEL
#define SI_DEVICE_MODEL "Soul Injector"
#else
#define SI_DEVICE_MODEL CONFIG_SI_DEVICE_MODEL
#endif

#ifndef CONFIG_SI_DEVICE_BUILD
#define SI_DEVICE_BUILD "0.0.0-UNKNOWN"
#else
#define SI_DEVICE_BUILD CONFIG_SI_DEVICE_BUILD
#endif

namespace cdc_def
{
    enum file_recv_state : uint8_t {
        FILE_RECV_NONE = 0,
        FILE_RECV_FW = 1,
        FILE_RECV_ALGO = 2,
    };

    enum event : uint32_t {
        EVT_NEW_PACKET = BIT(0),
        EVT_READING_PKT = BIT(1),
        EVT_READING_FILE = BIT(2),
    };

    enum pkt_type : uint8_t {
        PKT_ACK = 0,
        PKT_DEVICE_INFO = 0x01,
        PKT_PING = 0x02,
        PKT_FETCH_FILE = 0x10,
        PKT_SEND_FILE = 0x11,
        PKT_DELETE_FILE = 0x12,
        PKT_DATA_CHUNK = 0x13,
        PKT_CHUNK_ACK = 0x14,
        PKT_GET_FILE_INFO = 0x15,
        PKT_NUKE_STORAGE = 0x16,
        PKT_NACK = 0xff,
    };

    enum chunk_ack : uint8_t {
        CHUNK_XFER_DONE = 0,
        CHUNK_XFER_NEXT = 1,
        CHUNK_ERR_CRC32_FAIL = 2,
        CHUNK_ERR_INTERNAL = 3,
        CHUNK_ERR_ABORT_REQUESTED = 4,
        CHUNK_ERR_NAME_TOO_LONG = 5,
    };

    struct __attribute__((packed)) chunk_ack_pkt {
        chunk_ack state;

        // Can be:
        // 1. Next chunk offset (when state == 1)
        // 2. Expected CRC32 (when state == 2)
        // 3. Max length allowed (when state == 3)
        // 4. Just 0 (when state == anything else?)
        uint32_t aux_info;
    };

    struct __attribute__((packed)) header {
        pkt_type type;
        uint8_t len;
        uint16_t crc;
    };

    struct __attribute__((packed)) ack_pkt {
        pkt_type type;
        uint8_t len;
        uint16_t crc;
    };

    struct __attribute__((packed)) device_info {
        uint8_t mac_addr[6];
        uint8_t flash_id[8];
        char esp_idf_ver[32];
        char dev_model[32];
        char dev_build[32];
    };

    struct __attribute__((packed)) fw_info {
        uint32_t crc; // 4
        uint32_t len; // 4
        uint8_t name_len; // 1
        char name[UINT8_MAX - 9];
    }; // 255 bytes

    struct __attribute__((packed)) chunk_pkt {
        uint8_t len;
        uint8_t buf[UINT8_MAX];
    }; // 256 bytes
}

class cdc_acm
{
public:
    static cdc_acm *instance()
    {
        static cdc_acm _instance;
        return &_instance;
    }

    cdc_acm(cdc_acm const &) = delete;
    void operator=(cdc_acm const &) = delete;

private:
    cdc_acm() = default;
    static void serial_rx_cb(int itf, cdcacm_event_t *event);
    [[noreturn]] static void rx_handler_task(void *ctx);
    static esp_err_t send_pkt(cdc_def::pkt_type type, const uint8_t *buf, size_t len, uint32_t timeout_tick = portMAX_DELAY);
    static esp_err_t send_buf_with_header(const uint8_t *header_buf, size_t header_len, const uint8_t *buf, size_t len, uint32_t timeout_tick = portMAX_DELAY);
    static inline uint16_t get_crc16(const uint8_t *buf, size_t len, uint16_t init = 0x0000);

public:
    esp_err_t init();
    esp_err_t pause_usb();
    esp_err_t unpause_usb();

private:
    void parse_pkt();
    void handle_fetch_file_req();
    void handle_send_file_req();
    void parse_chunk();

private:
    static esp_err_t send_ack(uint16_t crc = 0, uint32_t timeout_ms = portMAX_DELAY);
    static esp_err_t send_nack(uint32_t timeout_ms = portMAX_DELAY);
    static esp_err_t send_dev_info(uint32_t timeout_ms = portMAX_DELAY);
    static esp_err_t send_chunk_ack(cdc_def::chunk_ack state, uint32_t aux = 0, uint32_t timeout_ms = portMAX_DELAY);
    static esp_err_t encode_slip_and_tx(const uint8_t *buf, size_t len, bool send_start, bool send_end, uint32_t timeout_ticks = portMAX_DELAY);

private:
    static const constexpr char *TAG = "cdc_acm";
    static const constexpr char USB_DESC_MANUFACTURER[] = "Jackson M Hu";
    static const constexpr char USB_DESC_PRODUCT[] = "Soul Injector";
    static const constexpr char USB_DESC_CDC_NAME[] = "Soul Injector Programmer";

private:
    LfBb<uint8_t, 32768> rx_buf_bb {};
    cdc_def::file_recv_state recv_state = cdc_def::FILE_RECV_NONE;
    EventGroupHandle_t rx_event = nullptr;
    volatile bool paused = false;
    size_t file_expect_len = 0;
    size_t file_curr_offset = 0;
    uint32_t file_crc = 0;
    FILE *file_handle = nullptr;
};


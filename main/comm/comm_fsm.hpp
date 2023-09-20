#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>
#include "comm_interface.hpp"

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

namespace comm_def
{
    static const constexpr size_t max_path_len = 220;
    enum file_recv_state : uint8_t {
        FILE_RECV_NONE = 0,
        FILE_RECV_FW = 1,
        FILE_RECV_ALGO = 2,
    };

    enum pkt_type : uint8_t {
        PKT_ACK = 0,
        PKT_DEVICE_INFO = 0x01,
        PKT_PING = 0x02,
        PKT_FETCH_FILE = 0x10,
        PKT_STORE_FILE = 0x11,
        PKT_DELETE_FILE = 0x12,
        PKT_DATA_CHUNK = 0x13,
        PKT_CHUNK_ACK = 0x14,
        PKT_GET_FILE_INFO = 0x15,
        PKT_FORMAT_PARTITION = 0x16,
        PKT_ERROR = 0xfe,
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

    struct __attribute__((packed)) file_attr_info {
        uint32_t crc; // 4
        uint32_t len; // 4
        char path[max_path_len + 1];
    }; // 255 bytes

    struct __attribute__((packed)) file_op_req {
        char path[max_path_len + 1];
    };

    struct __attribute__((packed)) part_format_req {
        char partition_label[max_path_len + 1];
    };

    struct __attribute__((packed)) chunk_pkt {
        uint16_t len;
        uint8_t buf[max_path_len + 1];
    }; // 255 bytes

    static const constexpr size_t max_pkt_len = UINT8_MAX;
}

class comm_fsm
{
public:
    static comm_fsm *instance()
    {
        static comm_fsm _instance;
        return &_instance;
    }

    comm_fsm(comm_fsm const &) = delete;
    void operator=(comm_fsm const &) = delete;

public:
    esp_err_t init(comm_interface *_interface);

private:
    comm_fsm() = default;

private:
    esp_err_t send_pkt(comm_def::pkt_type type, const uint8_t *buf, size_t len, uint32_t timeout_tick = portMAX_DELAY);
    esp_err_t send_buf_with_header(const uint8_t *header_buf, size_t header_len, const uint8_t *buf, size_t len, uint32_t timeout_tick = portMAX_DELAY);
    static inline uint16_t get_crc16(const uint8_t *buf, size_t len, uint16_t init = 0x0000);
    static void rx_handler_task(void *ctx);

private:
    esp_err_t send_ack(uint16_t crc = 0, uint32_t timeout_ms = portMAX_DELAY);
    esp_err_t send_nack(uint32_t timeout_ms = portMAX_DELAY);
    esp_err_t send_error(esp_err_t err);
    esp_err_t send_dev_info(uint32_t timeout_ms = portMAX_DELAY);
    esp_err_t send_chunk_ack(comm_def::chunk_ack state, uint32_t aux = 0, uint32_t timeout_ms = portMAX_DELAY);

    void parse_pkt();
    void handle_fetch_file_req();
    void handle_store_file_req();
    void handle_file_chunk();
    void handle_get_file_info();
    void handle_delete_file();
    void handle_format_partition();

private:
    static const constexpr char TAG[] = "comm_fsm";

private:
    volatile bool expect_file_chunk = false;
    uint8_t *rx_buf_ptr = nullptr;
    size_t rx_buf_len = 0;
    comm_interface *comm_if = nullptr;
    size_t file_expect_len = 0;
    size_t file_curr_offset = 0;
    uint32_t file_crc = 0;
    FILE *file_handle = nullptr;
    char file_name[sizeof(comm_def::file_attr_info::path) + 1] = {0 };
};

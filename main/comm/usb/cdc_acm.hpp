#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <tinyusb.h>
#include <tusb_cdc_acm.h>
#include <esp_err.h>

#include "lfbb.hpp"
#include "comm_interface.hpp"

#define SLIP_START              0xa5
#define SLIP_END                0xc0
#define SLIP_ESC                0xdb
#define SLIP_ESC_START          0xdc
#define SLIP_ESC_ESC            0xdd
#define SLIP_ESC_END            0xde

namespace cdc_def
{
    enum event : uint32_t {
        EVT_NEW_PACKET = BIT(0),
        EVT_READING_SLIP_FRAME = BIT(1),
        EVT_CONSUMING_PKT = BIT(2),
        EVT_READING_FILE = BIT(3),
    };
}

class cdc_acm : public comm_interface
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

public:
    esp_err_t init(tinyusb_cdcacm_itf_t channel = TINYUSB_CDC_ACM_0);
    esp_err_t wait_for_recv(uint32_t timeout_ticks) override;
    esp_err_t encode_and_send(const uint8_t *buf, size_t len, bool send_start, bool send_end, uint32_t timeout_ticks) override;
    esp_err_t pause_recv() final;
    esp_err_t resume_recv() final;
    esp_err_t acquire_read_buf(uint8_t **out, size_t *actual_len) override;
    esp_err_t release_read_buf(size_t buf_read) override;

private:
    esp_err_t write_to_rx_buf(uint8_t data);

private:
    static const constexpr char *TAG = "cdc_acm";
    static const constexpr char USB_DESC_MANUFACTURER[] = "Jackson M Hu";
    static const constexpr char USB_DESC_PRODUCT[] = "Soul Injector";
    static const constexpr char USB_DESC_CDC_NAME[] = "Soul Injector Programmer";

private:
    static uint8_t rx_raw_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    LfBb<uint8_t, 512> rx_buf_bb {};

private:
    tinyusb_cdcacm_itf_t cdc_channel = TINYUSB_CDC_ACM_0;
    EventGroupHandle_t rx_event = nullptr;
    volatile bool paused = false;
};


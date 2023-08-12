#pragma once

#include <esp_err.h>

class comm_interface
{
public:
    virtual esp_err_t begin_send(size_t len, uint32_t timeout_ticks) = 0;
    virtual esp_err_t send_buf(const uint8_t *buf, size_t len, uint32_t timeout_ticks) = 0;
    virtual esp_err_t finish_send(uint32_t timeout_ticks) = 0;
    virtual esp_err_t flush_send_queue(uint32_t timeout_ticks) = 0;

    virtual esp_err_t wait_for_recv(uint32_t timeout_ticks) = 0;
    virtual esp_err_t pause_recv() = 0;
    virtual esp_err_t resume_recv() = 0;
    virtual esp_err_t acquire_read_buf(uint8_t **out, size_t req_len, size_t *actual_len, uint32_t timeout_ticks) = 0;
    virtual esp_err_t release_read_buf(uint8_t *buf, size_t buf_read) = 0;
    virtual esp_err_t clear_recv_buf() = 0;
};
#pragma once

#include <esp_err.h>

class comm_interface
{
public:
    virtual esp_err_t decode_and_recv(uint8_t *buf, size_t buf_len, size_t *len_decoded, uint32_t timeout_ticks) = 0;
    virtual esp_err_t encode_and_send(const uint8_t *buf, size_t len, bool send_start, bool send_end, uint32_t timeout_ticks) = 0;
    virtual esp_err_t pause_recv() = 0;
    virtual esp_err_t resume_recv() = 0;
    virtual esp_err_t acquire_read_buf(uint8_t **out, size_t *actual_len) = 0;
    virtual esp_err_t release_read_buf(size_t buf_read) = 0;
};
#pragma once

#include <freertos/FreeRTOS.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lvgl_disp_init();
esp_err_t lvgl_take_lock(TickType_t ticks);
void lvgl_give_lock();

#ifdef __cplusplus
}
#endif
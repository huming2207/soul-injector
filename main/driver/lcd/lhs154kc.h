#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include <esp_err.h>
#include <lvgl.h>

#define SI_DISP_BUF_SIZE CONFIG_SI_DISP_PANEL_BUFFER_SIZE
#define SI_DISP_HOR_SIZE 240
#define SI_DISP_VER_SIZE 240

#define CONFIG_LCD_IO_DC CONFIG_SI_DISP_PANEL_IO_DC
#define CONFIG_LCD_SPI_CS CONFIG_SI_DISP_PANEL_IO_CS
#define CONFIG_LCD_SPI_SCLK CONFIG_SI_DISP_PANEL_IO_SCLK
#define CONFIG_LCD_SPI_MOSI CONFIG_SI_DISP_PANEL_IO_MOSI
#define CONFIG_LCD_IO_RST CONFIG_SI_DISP_PANEL_IO_RST

typedef struct {
    uint8_t reg;
    uint8_t data[6];
    uint8_t len;
} st7789_seq_t;

esp_err_t lv_st7789_init();
void lv_st7789_fill(int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color);
void lv_st7789_flush(lv_disp_drv_t *disp_drv, const lv_area_t * area, lv_color_t * color_p);

#ifdef __cplusplus
}
#endif

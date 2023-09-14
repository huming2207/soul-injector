#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include "lhs154kc_panel.hpp"

#define LHS154KC_CMD    0
#define LHS154KC_DATA   1

#ifdef CONFIG_SI_DISP_PANEL_IO_DC
#define CONFIG_LCD_IO_DC CONFIG_SI_DISP_PANEL_IO_DC
#else
#define CONFIG_LCD_IO_DC (-1)
#endif

#ifdef CONFIG_SI_DISP_PANEL_IO_CS
#define CONFIG_LCD_SPI_CS CONFIG_SI_DISP_PANEL_IO_CS
#else
#define CONFIG_LCD_SPI_CS (-1)
#endif

#ifdef CONFIG_SI_DISP_PANEL_IO_SCLK
#define CONFIG_LCD_SPI_SCLK CONFIG_SI_DISP_PANEL_IO_SCLK
#else
#define CONFIG_LCD_SPI_SCLK (-1)
#endif

#ifdef CONFIG_SI_DISP_PANEL_IO_MOSI
#define CONFIG_LCD_SPI_MOSI CONFIG_SI_DISP_PANEL_IO_MOSI
#else
#define CONFIG_LCD_SPI_MOSI (-1)
#endif

#ifdef CONFIG_SI_DISP_PANEL_IO_RST
#define CONFIG_LCD_IO_RST CONFIG_SI_DISP_PANEL_IO_RST
#else
#define CONFIG_LCD_IO_RST (-1)
#endif

esp_err_t lhs154kc_panel::init()
{
    ESP_LOGI(TAG, "GPIO Init");

    // For some special pins like JTAG pins, we need to reset them before use
    esp_err_t ret = gpio_reset_pin(static_cast<gpio_num_t>(CONFIG_LCD_IO_RST));
    ret = ret ?: gpio_reset_pin(static_cast<gpio_num_t>(CONFIG_LCD_SPI_SCLK));
    ret = ret ?: gpio_reset_pin(static_cast<gpio_num_t>(CONFIG_LCD_IO_DC));
    ret = ret ?: gpio_reset_pin(static_cast<gpio_num_t>(CONFIG_LCD_SPI_CS));
    ret = ret ?: gpio_reset_pin(static_cast<gpio_num_t>(CONFIG_LCD_SPI_MOSI));

    ret = ret ?: gpio_set_direction(static_cast<gpio_num_t>(CONFIG_LCD_IO_DC), GPIO_MODE_OUTPUT);
    ret = ret ?: gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_LCD_IO_DC), GPIO_PULLUP_ONLY);
    ret = ret ?: gpio_set_direction(static_cast<gpio_num_t>(CONFIG_LCD_IO_RST), GPIO_MODE_OUTPUT);
    ret = ret ?: gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_LCD_IO_RST), GPIO_PULLUP_ONLY);
    ESP_LOGI(TAG, "Reset display");

    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)(CONFIG_LCD_IO_RST), 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)(CONFIG_LCD_IO_RST), 1));
    vTaskDelay(pdMS_TO_TICKS(100));

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = CONFIG_LCD_SPI_MOSI;
    bus_config.sclk_io_num = CONFIG_LCD_SPI_SCLK;
    bus_config.miso_io_num = -1; // We don't need to get any bullshit from the panel, so no MISO needed.
    bus_config.quadhd_io_num = -1;
    bus_config.quadwp_io_num = -1;
    bus_config.max_transfer_sz = 240 * 240 * 2;

    spi_device_interface_config_t device_config = {};
#ifndef CONFIG_SI_DISP_SLOW_CLK
    device_config.clock_speed_hz = SPI_MASTER_FREQ_40M;
#else
            .clock_speed_hz = SPI_MASTER_FREQ_8M,
#endif
    device_config.mode = 0; // CPOL = 0, CPHA = 0???
    device_config.spics_io_num = CONFIG_LCD_SPI_CS;
    device_config.queue_size = 7;

    ESP_LOGI(TAG, "SPI Init");
    ret = ret ?: spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    ret = ret ?: spi_bus_add_device(SPI2_HOST, &device_config, &device_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Init failed");
        return ret;
    }

    // Wake up
    ret = spi_send(REG_SLEEP_OUT, true);
    vTaskDelay(pdMS_TO_TICKS(120)); // Do we need this?

    // Send init sequence
    for(uint32_t curr_pos = 0; curr_pos < sizeof(INIT_SEQ)/sizeof(INIT_SEQ[0]); curr_pos += 1) {
        ret = ret ?: spi_send(&INIT_SEQ[curr_pos]);
    }

    // Throw in positive gamma
    ret = ret ?: spi_send(REG_POS_GAMMA, true);
    ret = ret ?: spi_send(POS_GAMMA_VAL, sizeof(POS_GAMMA_VAL), false);

    // Throw in negative gamma to RE1h
    ret = ret ?: spi_send(REG_NEG_GAMMA, true);
    ret = ret ?: spi_send(NEG_GAMMA_VAL, sizeof(NEG_GAMMA_VAL), false);

    // Display inversion on
    ret = ret ?: spi_send(REG_DISP_INVERSION, true);

    // Turn on the panel
    ret = ret ?: spi_send(REG_DISP_ON, true);

    return ret;
}

esp_err_t lhs154kc_panel::set_backlight(uint8_t level)
{
    return 0;
}

void lhs154kc_panel::flush_display(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    ESP_ERROR_CHECK(set_pos(area->x1, area->x2, area->y1, area->y2));

    const uint8_t write_fb_reg = 0x2C;
    ESP_ERROR_CHECK(spi_send(&write_fb_reg, 1, true));

    uint32_t line_size = area->x2 - area->x1 + 1;

    for(uint32_t curr_y = area->y1; curr_y <= area->y2; curr_y++) {
        spi_send(&color_p->full, line_size * 2);
        color_p += line_size;
    }

    lv_disp_flush_ready(disp_drv);
}

esp_err_t lhs154kc_panel::deinit()
{
    return 0;
}

esp_err_t lhs154kc_panel::spi_send(const uint8_t *payload, size_t len, bool is_cmd)
{
    if(!payload) {
        ESP_LOGE(TAG, "Payload is null!");
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t spi_tract = {};

    spi_tract.tx_buffer = payload;
    spi_tract.length = len * 8;
    spi_tract.rxlength = 0;

    auto ret = gpio_set_level(static_cast<gpio_num_t>(CONFIG_LCD_IO_DC), is_cmd ? LHS154KC_CMD : LHS154KC_DATA);
    ret = ret ?: spi_device_polling_transmit(device_handle, &spi_tract);
    return ret;
}

esp_err_t lhs154kc_panel::spi_send(const uint16_t *pixel, size_t len)
{
    if(!pixel) {
        ESP_LOGE(TAG, "Payload is null!");
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t spi_tract = {};

    spi_tract.tx_buffer = pixel;
    spi_tract.length = len * 8 * 2;
    spi_tract.rxlength = 0;

    auto ret = gpio_set_level(static_cast<gpio_num_t>(CONFIG_LCD_IO_DC), LHS154KC_DATA);
    ret = ret ?: spi_device_polling_transmit(device_handle, &spi_tract); // Use blocking transmit for now (easier to debug)

    return ret;
}

esp_err_t lhs154kc_panel::spi_send(const uint8_t payload, bool is_cmd)
{
    return spi_send(&payload, 1, is_cmd);
}

esp_err_t lhs154kc_panel::spi_send(const lhs154kc::seq_t *seq)
{
    ESP_LOGD(TAG, "Writing to register 0x%02X...", seq->reg);
    auto ret = spi_send(seq->reg, true);

    if(seq->len > 0) {
        ret = ret ?: spi_send(seq->data, seq->len, false);
        ESP_LOGD(TAG, "Seq data: 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x; len: %u", seq->data[0], seq->data[1], seq->data[2], seq->data[3], seq->data[4], seq->len);
    }

    return ret;
}

esp_err_t lhs154kc_panel::set_pos(uint16_t x1, uint16_t x2, uint16_t y1, uint16_t y2)
{
    const uint8_t x_pos_cmd = 0x2A, y_pos_cmd = 0x2B;
    uint8_t x_start[] = {(uint8_t)(x1 >> 8u), (uint8_t)(x1 & 0xffU)};
    uint8_t x_end[] = {(uint8_t)(x2 >> 8u), (uint8_t)(x2 & 0xffU)};
    uint8_t y_start[] = {(uint8_t)(y1 >> 8u), (uint8_t)(y1 & 0xffU)};
    uint8_t y_end[] = {(uint8_t)(y2 >> 8u), (uint8_t)(y2 & 0xffU)};

    ESP_LOGD(TAG, "Setting position in x1: %u, x2: %u; y1: %u, y2: %u", x1, x2, y1, y2);

    auto ret = spi_send(&x_pos_cmd, 1, true);
    ret = ret ?: spi_send(x_start, 2, false);
    ret = ret ?: spi_send(x_end, 2, false);

    ret = ret ?: spi_send(&y_pos_cmd, 1, true);
    ret = ret ?: spi_send(y_start, 2, false);
    ret = ret ?: spi_send(y_end, 2, false);

    ESP_LOGD(TAG, "Position set!");
    return ret;
}

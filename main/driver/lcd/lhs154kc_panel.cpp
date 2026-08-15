#include <driver/gpio.h>
#include <esp_log.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>

#include "lhs154kc_panel.hpp"

esp_err_t lhs154kc_panel::init()
{
    gpio_config_t pwr_io_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_SI_DISP_PANEL_BKL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    auto ret = gpio_config(&pwr_io_cfg);
    ret = ret ?: gpio_set_level((gpio_num_t)CONFIG_SI_DISP_PANEL_BKL, 0);
    ret = ret ?: gpio_reset_pin((gpio_num_t)CONFIG_SI_DISP_PANEL_IO_CS);
    ret = ret ?: gpio_reset_pin((gpio_num_t)CONFIG_SI_DISP_PANEL_IO_DC);
    ret = ret ?: gpio_reset_pin((gpio_num_t)CONFIG_SI_DISP_PANEL_IO_MOSI);
    ret = ret ?: gpio_reset_pin((gpio_num_t)CONFIG_SI_DISP_PANEL_IO_RST);
    ret = ret ?: gpio_reset_pin((gpio_num_t)CONFIG_SI_DISP_PANEL_IO_SCLK);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO setup failed: 0x%x", ret);
        return ret;
    }

    spi_bus_config_t spi_bus_cfg = {};
    spi_bus_cfg.sclk_io_num = CONFIG_SI_DISP_PANEL_IO_SCLK;
    spi_bus_cfg.mosi_io_num = CONFIG_SI_DISP_PANEL_IO_MOSI;
    spi_bus_cfg.miso_io_num = GPIO_NUM_NC;
    spi_bus_cfg.quadhd_io_num = GPIO_NUM_NC;
    spi_bus_cfg.quadwp_io_num = GPIO_NUM_NC;
    ret = spi_bus_initialize(LCD_SPI_HOST, &spi_bus_cfg, SPI_DMA_CH_AUTO);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Bus init failed: 0x%x", ret);
        return ret;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num = static_cast<gpio_num_t>(CONFIG_SI_DISP_PANEL_IO_DC);
    io_cfg.cs_gpio_num = static_cast<gpio_num_t>(CONFIG_SI_DISP_PANEL_IO_CS);

#ifndef CONFIG_SI_DISP_SLOW_CLK
    io_cfg.pclk_hz = SPI_MASTER_FREQ_40M;
#else
    io_cfg.pclk_hz = SPI_MASTER_FREQ_8M;
#endif

    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.spi_mode = 0;
    io_cfg.trans_queue_depth = 10;
    io_cfg.user_ctx = this;
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI dev init failed: 0x%x", ret);
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(CONFIG_SI_DISP_PANEL_IO_RST);
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel create failed: 0x%x", ret);
        return ret;
    }

    ret = set_backlight(0);
    ret = ret ?: esp_lcd_panel_reset(panel_handle);
    ret = ret ?: esp_lcd_panel_init(panel_handle);
    ret = ret ?: send_sequence(LCD_INIT_SEQ, sizeof(LCD_INIT_SEQ) / sizeof(lhs154kc::seq_t));
    ret = ret ?: esp_lcd_panel_disp_on_off(panel_handle, true);
    ret = ret ?: esp_lcd_panel_invert_color(panel_handle, true);
    ret = ret ?: set_backlight(100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lhs154kc: can't setup LCD");
        return ret;
    }

    ret = lvgl_port_init(&LVGL_CFG);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP LVGL init failed, 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    lvgl_port_display_cfg_t disp_cfg = {.io_handle = io_handle,
                                        .panel_handle = panel_handle,
                                        .control_handle = nullptr,
                                        .buffer_size = 240 * 240,
                                        .double_buffer = true,
                                        .trans_size = 0,
                                        .hres = 240,
                                        .vres = 240,
                                        .monochrome = false,
                                        .rotation =
                                            {
                                                .swap_xy = false,
                                                .mirror_x = false,
                                                .mirror_y = false,
                                            },
                                        .rounder_cb = nullptr,
                                        .color_format = LV_COLOR_FORMAT_RGB565,
                                        .flags = {
                                            .buff_dma = true,
                                            .buff_spiram = true,
                                            .sw_rotate = false,
                                            .swap_bytes = true,
                                            .full_refresh = false,
                                            .direct_mode = false,
                                        }};

    display = lvgl_port_add_disp(&disp_cfg);
    if (display == nullptr) {
        ESP_LOGE(TAG, "Can't add display!");
        return ESP_ERR_INVALID_STATE;
    }

    return ret;
}

esp_err_t lhs154kc_panel::set_backlight(uint8_t level)
{
    if (level < 1) {
        gpio_set_level((gpio_num_t)CONFIG_SI_DISP_PANEL_BKL, 0);
    } else {
        gpio_set_level((gpio_num_t)CONFIG_SI_DISP_PANEL_BKL, 1);
    }

    return ESP_OK;
}

esp_err_t lhs154kc_panel::deinit()
{
    return ESP_ERR_NOT_SUPPORTED;
}

size_t lhs154kc_panel::get_hor_size() const
{
    return 240;
}

size_t lhs154kc_panel::get_ver_size() const
{
    return 240;
}

esp_err_t lhs154kc_panel::lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void lhs154kc_panel::unlock()
{
    lvgl_port_unlock();
}

lv_display_t *lhs154kc_panel::get_lv_disp()
{
    return display;
}

esp_err_t lhs154kc_panel::send_sequence(const lhs154kc::seq_t *seq, size_t seq_cnt)
{
    esp_err_t ret = ESP_OK;

    for (size_t idx = 0; idx < seq_cnt; idx += 1) {
        ret =
            ret ?: esp_lcd_panel_io_tx_param(io_handle, seq[idx].reg, (seq[idx].len > 0 ? seq[idx].data : nullptr), seq[idx].len);
    }

    return ret;
}


#include <esp_log.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include "nfp190b_panel.hpp"

#define NFP190B_DISP_PIN_DATA0 CONFIG_SI_DISP_PANEL_IO_D0  // 39
#define NFP190B_DISP_PIN_DATA1 CONFIG_SI_DISP_PANEL_IO_D1    // 40
#define NFP190B_DISP_PIN_DATA2 CONFIG_SI_DISP_PANEL_IO_D2    // 41
#define NFP190B_DISP_PIN_DATA3 CONFIG_SI_DISP_PANEL_IO_D3    // 42
#define NFP190B_DISP_PIN_DATA4 CONFIG_SI_DISP_PANEL_IO_D4    // 45
#define NFP190B_DISP_PIN_DATA5 CONFIG_SI_DISP_PANEL_IO_D5    // 46
#define NFP190B_DISP_PIN_DATA6 CONFIG_SI_DISP_PANEL_IO_D6    // 47
#define NFP190B_DISP_PIN_DATA7 CONFIG_SI_DISP_PANEL_IO_D7    // 48
#define NFP190B_DISP_PIN_PCLK CONFIG_SI_DISP_PANEL_IO_SCLK      // 8
#define NFP190B_DISP_PIN_CS CONFIG_SI_DISP_PANEL_IO_CS        // 6
#define NFP190B_DISP_PIN_DC CONFIG_SI_DISP_PANEL_IO_DC        // 7
#define NFP190B_DISP_PIN_RST CONFIG_SI_DISP_PANEL_IO_RST       // 5
#define NFP190B_DISP_PIN_BK_LIGHT CONFIG_SI_DISP_PANEL_BKL // 1
#define NFP190B_DISP_PIN_POWER CONFIG_SI_DISP_PANEL_PWR
#define NFP190B_DISP_PIN_RD CONFIG_SI_DISP_PANEL_IO_RD
#ifdef CONFIG_SI_DISP_SLOW_CLK
#define NFP190B_CLK_SPEED_HZ (1*1000*1000)
#else
#define NFP190B_CLK_SPEED_HZ (10*1000*1000)
#endif

#define SI_DISP_BUF_SIZE CONFIG_SI_DISP_PANEL_BUFFER_SIZE
#define SI_DISP_HOR_SIZE 320
#define SI_DISP_VER_SIZE 170

esp_err_t nfp190b_panel::init()
{
    gpio_config_t pwr_io_cfg = {
            .pin_bit_mask = (1ULL << NFP190B_DISP_PIN_POWER) | (1ULL << NFP190B_DISP_PIN_BK_LIGHT),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
    };

    auto ret = gpio_config(&pwr_io_cfg);

    gpio_config_t rd_io_cfg = {
            .pin_bit_mask = (1ULL << NFP190B_DISP_PIN_RD),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
    };

    ret = ret ?: gpio_config(&rd_io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO setup failed: 0x%x", ret);
        return ret;
    }

    esp_lcd_i80_bus_config_t bus_cfg = {};
    bus_cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    bus_cfg.dc_gpio_num = NFP190B_DISP_PIN_DC;
    bus_cfg.wr_gpio_num = NFP190B_DISP_PIN_PCLK;
    bus_cfg.data_gpio_nums[0] = NFP190B_DISP_PIN_DATA0;
    bus_cfg.data_gpio_nums[1] = NFP190B_DISP_PIN_DATA1;
    bus_cfg.data_gpio_nums[2] = NFP190B_DISP_PIN_DATA2;
    bus_cfg.data_gpio_nums[3] = NFP190B_DISP_PIN_DATA3;
    bus_cfg.data_gpio_nums[4] = NFP190B_DISP_PIN_DATA4;
    bus_cfg.data_gpio_nums[5] = NFP190B_DISP_PIN_DATA5;
    bus_cfg.data_gpio_nums[6] = NFP190B_DISP_PIN_DATA6;
    bus_cfg.data_gpio_nums[7] = NFP190B_DISP_PIN_DATA7;
    bus_cfg.bus_width = 8;
    bus_cfg.max_transfer_bytes = SI_DISP_BUF_SIZE * sizeof(uint16_t);

    ret = esp_lcd_new_i80_bus(&bus_cfg, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I80 bus setup failed: 0x%x", ret);
        return ret;
    }

    esp_lcd_panel_io_i80_config_t io_config = {};
    io_config.cs_gpio_num = NFP190B_DISP_PIN_CS;
    io_config.pclk_hz = NFP190B_CLK_SPEED_HZ;
    io_config.trans_queue_depth = 20;
    io_config.dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
    };
    io_config.on_color_trans_done = handle_fb_trans_finish;
    io_config.user_ctx = this;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;

    ret = esp_lcd_new_panel_io_i80(bus_handle, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I80 port setup failed: 0x%x", ret);
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.reset_gpio_num = NFP190B_DISP_PIN_RST;
    panel_cfg.rgb_endian = LCD_RGB_ENDIAN_RGB;

    ret = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 setup failed: 0x%x", ret);
        return ret;
    }

    ret = esp_lcd_panel_reset(panel_handle);
    ret = ret ?: esp_lcd_panel_init(panel_handle);
    ret = ret ?: esp_lcd_panel_invert_color(panel_handle, true);
    ret = ret ?: esp_lcd_panel_swap_xy(panel_handle, true);
    ret = ret ?: esp_lcd_panel_mirror(panel_handle, false, true);
    // the gap is LCD panel specific, even panels with the same driver IC, can have different gap value
    ret = ret ?: esp_lcd_panel_set_gap(panel_handle, 0, 35);

    ret = ret ?: esp_lcd_panel_io_tx_param(io_handle, 0xF2, (uint8_t[]){0}, 1); // 3Gamma function disable
    ret = ret ?: esp_lcd_panel_io_tx_param(io_handle, 0x26, (uint8_t[]){1}, 1); // Gamma curve 1 selected
    ret = ret ?: esp_lcd_panel_io_tx_param(io_handle, 0xE0, (uint8_t[]){        // Set positive gamma
                                      0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00},
                              15);
    ret = ret ?: esp_lcd_panel_io_tx_param(io_handle, 0xE1, (uint8_t[]){// Set negative gamma
                                      0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F},
                              15);

    ret = ret ?: esp_lcd_panel_disp_on_off(panel_handle, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed: 0x%x", ret);
        return ret;
    } else {
        ESP_LOGI(TAG, "Init OK");
    }

    return ret;
}

esp_err_t nfp190b_panel::set_backlight(uint8_t level)
{
    return 0;
}

void nfp190b_panel::flush_display(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    auto x1 = area->x1;
    auto x2 = area->x2;
    auto y1 = area->y1;
    auto y2 = area->y2;

    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, color_p);
}

esp_err_t nfp190b_panel::deinit()
{
    return 0;
}

bool nfp190b_panel::handle_fb_trans_finish(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    auto *ctx = (nfp190b_panel *)user_ctx;
    lv_disp_flush_ready(ctx->lv_drv);
    return false;
}

void nfp190b_panel::set_lv_disp_drv(lv_disp_drv_t *drv)
{
    lv_drv = drv;
}

size_t nfp190b_panel::get_hor_size() const
{
    return SI_DISP_HOR_SIZE;
}

size_t nfp190b_panel::get_ver_size() const
{
    return SI_DISP_VER_SIZE;
}

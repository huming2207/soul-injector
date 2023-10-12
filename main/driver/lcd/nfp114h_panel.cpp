#include <driver/gpio.h>
#include <esp_log.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>

#include "nfp114h_panel.hpp"

esp_err_t nfp114h_panel::init()
{
    gpio_config_t pwr_io_cfg = {
            .pin_bit_mask = (1ULL << CONFIG_SI_DISP_PANEL_BKL),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
    };

    auto ret = gpio_config(&pwr_io_cfg);
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
    io_cfg.dc_gpio_num = CONFIG_SI_DISP_PANEL_IO_DC;
    io_cfg.cs_gpio_num = CONFIG_SI_DISP_PANEL_IO_CS;

#ifndef CONFIG_SI_DISP_SLOW_CLK
    io_cfg.pclk_hz = SPI_MASTER_FREQ_20M;
#else
    io_cfg.pclk_hz = SPI_MASTER_FREQ_8M;
#endif

    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.spi_mode = 0;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = handle_fb_trans_done;
    io_cfg.user_ctx = this;
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI dev init failed: 0x%x", ret);
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = CONFIG_SI_DISP_PANEL_IO_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel create failed: 0x%x", ret);
        return ret;
    }

    ret = set_backlight(0);
    ret = ret ?: esp_lcd_panel_reset(panel_handle);
    ret = ret ?: esp_lcd_panel_init(panel_handle);
    ret = ret ?: esp_lcd_panel_disp_on_off(panel_handle, true);
    ret = ret ?: esp_lcd_panel_invert_color(panel_handle, true);
    ret = ret ?: esp_lcd_panel_swap_xy(panel_handle, false);
    ret = ret ?: esp_lcd_panel_set_gap(panel_handle, 53, 40); // This is probably wrong - try 40, 53 and 52 combos
   // ret = ret ?: send_sequence(LCD_INIT_SEQ, sizeof(LCD_INIT_SEQ) / sizeof(nfp114h::seq_t));

    return ret;
}

esp_err_t nfp114h_panel::set_backlight(uint8_t level)
{
    return ESP_OK;
}

void nfp114h_panel::flush_display(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    auto x1 = area->x1;
    auto x2 = area->x2;
    auto y1 = area->y1;
    auto y2 = area->y2;

    ESP_LOGI(TAG, "Draw at x1 %d x2 %d; y1 %d y2 %d", x1, x2, y1, y2);
    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, color_p);
}

esp_err_t nfp114h_panel::deinit()
{
    return ESP_ERR_NOT_SUPPORTED;
}

size_t nfp114h_panel::get_hor_size() const
{
    return 135;
}

size_t nfp114h_panel::get_ver_size() const
{
    return 240;
}

lv_disp_drv_t *nfp114h_panel::get_lv_disp_drv()
{
    return &lv_drv;
}

esp_err_t nfp114h_panel::setup_lvgl(lv_disp_draw_buf_t *draw_buf)
{
    lv_disp_drv_init(&lv_drv);
    lv_drv.flush_cb = handle_flush;
    lv_drv.hor_res = (int16_t)get_hor_size();
    lv_drv.ver_res = (int16_t)get_ver_size();
    lv_drv.draw_buf = draw_buf;
    lv_drv.antialiasing = 1;
    lv_drv.user_data = this;

    lv_disp = lv_disp_drv_register(&lv_drv);
    if (lv_disp == nullptr) {
        ESP_LOGE(TAG, "LVGL display register failed!");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

bool nfp114h_panel::handle_fb_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    auto *ctx = (nfp114h_panel *)user_ctx;
    lv_disp_flush_ready(&ctx->lv_drv);
    return false;
}

esp_err_t nfp114h_panel::send_sequence(const nfp114h::seq_t *seq, size_t seq_cnt)
{
    esp_err_t ret = ESP_OK;

    for (size_t idx = 0; idx < seq_cnt; idx += 1) {
        ret = ret ?: esp_lcd_panel_io_tx_param(io_handle, seq[idx].reg, (seq[idx].len > 0 ? seq[idx].data : nullptr), seq[idx].len);
    }

    return ret;
}

void nfp114h_panel::handle_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    auto *ctx = static_cast<nfp114h_panel *>(disp_drv->user_data);
    ctx->flush_display(disp_drv, area, color_p);
}

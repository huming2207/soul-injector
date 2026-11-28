#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_ops.h>

#include "esp_lcd_panel_nv3007.h"
#include "nt279vj_panel.hpp"

// Light sleep isolates GPIOs (CONFIG_PM_SLP_DISABLE_GPIO): without this the
// supply switch and reset float to their pulls at each sleep and the panel
// loses power or resets.
static void keep_in_sleep(int gpio)
{
    if (gpio >= 0) {
        gpio_sleep_sel_dis(static_cast<gpio_num_t>(gpio));
    }
}

esp_err_t nt279vj_panel::init()
{
#if CONFIG_SI_DISP_PANEL_PWR >= 0
    // Power the panel before driving any of its lines, so they don't feed it
    // through its input protection, and let the supply settle before reset.
    const auto pwr_gpio = static_cast<gpio_num_t>(CONFIG_SI_DISP_PANEL_PWR);
    gpio_set_level(pwr_gpio, CONFIG_SI_DISP_PANEL_PWR_ON_LEVEL); // Level first: no glitch when the output turns on
    gpio_config_t supply_io_cfg = {};
    supply_io_cfg.pin_bit_mask = 1ULL << CONFIG_SI_DISP_PANEL_PWR;
    supply_io_cfg.mode = GPIO_MODE_OUTPUT;
    auto pwr_ret = gpio_config(&supply_io_cfg);
    if (pwr_ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel supply GPIO%d setup failed: 0x%x", CONFIG_SI_DISP_PANEL_PWR, pwr_ret);
        return pwr_ret;
    }
    keep_in_sleep(CONFIG_SI_DISP_PANEL_PWR);
    ESP_LOGI(TAG, "Panel supply on (GPIO%d = %d)", CONFIG_SI_DISP_PANEL_PWR, CONFIG_SI_DISP_PANEL_PWR_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(20));
#endif

    // The backlight is active high: off (level first, as for the supply) and no
    // pull-up feeding it while it's driven low.
    gpio_config_t pwr_io_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_SI_DISP_PANEL_BKL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    auto ret = gpio_set_level((gpio_num_t)CONFIG_SI_DISP_PANEL_BKL, 0);
    ret = ret ?: gpio_config(&pwr_io_cfg);
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
    if (LCD_SPI_HOST == SPI3_HOST) {
        // ESP-IDF v6.1 checks every host's pins against GP-SPI2's second IO MUX
        // set on the S31 (CLK 53, MOSI 54, ...), so on SPI3 it takes these pins
        // for IO MUX pins and gives them SPI3's nonexistent IO MUX function:
        // SCLK and MOSI never leave the chip. Use the GPIO matrix, as good as
        // IO MUX up to 40 MHz.
        spi_bus_cfg.flags = SPICOMMON_BUSFLAG_GPIO_PINS;
    }
    ret = spi_bus_initialize(LCD_SPI_HOST, &spi_bus_cfg, SPI_DMA_CH_AUTO);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Bus init failed: 0x%x", ret);
        return ret;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num = static_cast<gpio_num_t>(CONFIG_SI_DISP_PANEL_IO_DC);
    io_cfg.cs_gpio_num = static_cast<gpio_num_t>(CONFIG_SI_DISP_PANEL_IO_CS);

    io_cfg.pclk_hz = PCLK_HZ;

    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.spi_mode = 0;
    io_cfg.trans_queue_depth = 10;
    io_cfg.user_ctx = this;
    // The draw buffer is in PSRAM: DMA reads it directly instead of copying each
    // frame into an internal bounce buffer, which WiFi leaves no room for.
    io_cfg.flags.psram_dma_direct = true;
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI dev init failed: 0x%x", ret);
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(CONFIG_SI_DISP_PANEL_IO_RST);
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
    panel_cfg.bits_per_pixel = 16;
    ret = esp_lcd_new_panel_nv3007(io_handle, &panel_cfg, &panel_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel create failed: 0x%x", ret);
        return ret;
    }

    ret = set_backlight(0);
    ret = ret ?: esp_lcd_panel_reset(panel_handle);
    ret = ret ?: esp_lcd_panel_init(panel_handle);
    ret = ret ?: esp_lcd_panel_swap_xy(panel_handle, true);
    ret = ret ?: esp_lcd_panel_mirror(panel_handle, true, false);
    ret = ret ?: esp_lcd_panel_set_gap(panel_handle, 0, 14);
    // Backlight stays off: the panel is showing its uninitialised memory until
    // LVGL's first frame (display_manager::init()).
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nt279vj: can't setup LCD");
        return ret;
    }

    ret = lvgl_port_init(&LVGL_CFG);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP LVGL init failed, 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    // The PPA only writes to buffers whose address and size are whole cache
    // lines (LV_DRAW_BUF_ALIGN); 428 x 142 x 2 bytes is not.
    constexpr size_t px_bytes = 2;
    constexpr size_t buf_px = LV_ROUND_UP(LCD_H_RES * LCD_V_RES * px_bytes, LV_DRAW_BUF_ALIGN) / px_bytes;

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .control_handle = nullptr,
        .buffer_size = buf_px,
        .double_buffer = true,
        .trans_size = 0,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {.swap_xy = true, .mirror_x = true, .mirror_y = false},
        .rounder_cb = nullptr,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = false,
            .swap_bytes = true,
            .full_refresh = false,
            .direct_mode = false,
        }
    };

    display = lvgl_port_add_disp(&disp_cfg);
    if (display == nullptr) {
        ESP_LOGE(TAG, "Can't add display!");
        return ESP_ERR_INVALID_STATE;
    }

    keep_in_sleep(CONFIG_SI_DISP_PANEL_BKL);
    keep_in_sleep(CONFIG_SI_DISP_PANEL_IO_RST);
    keep_in_sleep(CONFIG_SI_DISP_PANEL_IO_CS);
    keep_in_sleep(CONFIG_SI_DISP_PANEL_IO_DC);
    keep_in_sleep(CONFIG_SI_DISP_PANEL_IO_SCLK);
    keep_in_sleep(CONFIG_SI_DISP_PANEL_IO_MOSI);
    return ret;
}

esp_err_t nt279vj_panel::set_backlight(uint8_t level)
{
    if (level < 1) {
        gpio_set_level((gpio_num_t)CONFIG_SI_DISP_PANEL_BKL, 0);
    } else {
        gpio_set_level((gpio_num_t)CONFIG_SI_DISP_PANEL_BKL, 1);
    }

    return ESP_OK;
}

esp_err_t nt279vj_panel::deinit()
{
    return ESP_ERR_NOT_SUPPORTED;
}

size_t nt279vj_panel::get_hor_size() const
{
    return LCD_H_RES;
}

size_t nt279vj_panel::get_ver_size() const
{
    return LCD_V_RES;
}

esp_err_t nt279vj_panel::lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void nt279vj_panel::unlock()
{
    lvgl_port_unlock();
}

lv_display_t *nt279vj_panel::get_lv_disp()
{
    return display;
}

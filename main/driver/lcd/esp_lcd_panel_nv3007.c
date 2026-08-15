#include <stdlib.h>
#include <sys/cdefs.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_lcd_panel_commands.h>
#include <esp_lcd_panel_interface.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>

#include "esp_lcd_panel_nv3007.h"

static const char *TAG = "lcd_panel.nv3007";

typedef struct {
    uint8_t cmd;
    uint8_t data[4];
    uint8_t data_bytes;
    uint16_t delay_ms;
} nv3007_init_cmd_t;

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    gpio_num_t reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t madctl_val;
} nv3007_panel_t;

static esp_err_t panel_nv3007_del(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3007_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3007_init(esp_lcd_panel_t *panel);
static esp_err_t
panel_nv3007_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_nv3007_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_nv3007_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_nv3007_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_nv3007_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

/*
 * NT279VJ-C10-01-V1 sequence transcribed from the manufacturer's STM32
 * example. The example sends 0x17 immediately after command 0xf1 and data
 * 0x0e, so it is retained here as the second parameter of command 0xf1.
 */
static const nv3007_init_cmd_t LCD_INIT_SEQ[] = {
    {0xff, {0xa5}, 1, 0},
    {0x9a, {0x08}, 1, 0},
    {0x9b, {0x08}, 1, 0},
    {0x9c, {0xb0}, 1, 0},
    {0x9d, {0x16}, 1, 0},
    {0x9e, {0xc4}, 1, 0},
    {0x8f, {0x55, 0x04}, 2, 0},
    {0x84, {0x90}, 1, 0},
    {0x83, {0x7b}, 1, 0},
    {0x85, {0x33}, 1, 0},
    {0x60, {0x00}, 1, 0},
    {0x70, {0x00}, 1, 0},
    {0x61, {0x02}, 1, 0},
    {0x71, {0x02}, 1, 0},
    {0x62, {0x04}, 1, 0},
    {0x72, {0x04}, 1, 0},
    {0x6c, {0x29}, 1, 0},
    {0x7c, {0x29}, 1, 0},
    {0x6d, {0x31}, 1, 0},
    {0x7d, {0x31}, 1, 0},
    {0x6e, {0x0f}, 1, 0},
    {0x7e, {0x0f}, 1, 0},
    {0x66, {0x21}, 1, 0},
    {0x76, {0x21}, 1, 0},
    {0x68, {0x3a}, 1, 0},
    {0x78, {0x3a}, 1, 0},
    {0x63, {0x07}, 1, 0},
    {0x73, {0x07}, 1, 0},
    {0x64, {0x05}, 1, 0},
    {0x74, {0x05}, 1, 0},
    {0x65, {0x02}, 1, 0},
    {0x75, {0x02}, 1, 0},
    {0x67, {0x23}, 1, 0},
    {0x77, {0x23}, 1, 0},
    {0x69, {0x08}, 1, 0},
    {0x79, {0x08}, 1, 0},
    {0x6a, {0x13}, 1, 0},
    {0x7a, {0x13}, 1, 0},
    {0x6b, {0x13}, 1, 0},
    {0x7b, {0x13}, 1, 0},
    {0x6f, {0x00}, 1, 0},
    {0x7f, {0x00}, 1, 0},
    {0x50, {0x00}, 1, 0},
    {0x52, {0xd6}, 1, 0},
    {0x53, {0x08}, 1, 0},
    {0x54, {0x08}, 1, 0},
    {0x55, {0x1e}, 1, 0},
    {0x56, {0x1c}, 1, 0},
    {0xa0, {0x2b, 0x24, 0x00}, 3, 0},
    {0xa1, {0x87}, 1, 0},
    {0xa2, {0x86}, 1, 0},
    {0xa5, {0x00}, 1, 0},
    {0xa6, {0x00}, 1, 0},
    {0xa7, {0x00}, 1, 0},
    {0xa8, {0x36}, 1, 0},
    {0xa9, {0x7e}, 1, 0},
    {0xaa, {0x7e}, 1, 0},
    {0xb9, {0x85}, 1, 0},
    {0xba, {0x84}, 1, 0},
    {0xbb, {0x83}, 1, 0},
    {0xbc, {0x82}, 1, 0},
    {0xbd, {0x81}, 1, 0},
    {0xbe, {0x80}, 1, 0},
    {0xbf, {0x01}, 1, 0},
    {0xc0, {0x02}, 1, 0},
    {0xc1, {0x00}, 1, 0},
    {0xc2, {0x00}, 1, 0},
    {0xc3, {0x00}, 1, 0},
    {0xc4, {0x33}, 1, 0},
    {0xc5, {0x7e}, 1, 0},
    {0xc6, {0x7e}, 1, 0},
    {0xc8, {0x33, 0x33}, 2, 0},
    {0xc9, {0x68}, 1, 0},
    {0xca, {0x69}, 1, 0},
    {0xcb, {0x6a}, 1, 0},
    {0xcc, {0x6b}, 1, 0},
    {0xcd, {0x33, 0x33}, 2, 0},
    {0xce, {0x6c}, 1, 0},
    {0xcf, {0x6d}, 1, 0},
    {0xd0, {0x6e}, 1, 0},
    {0xd1, {0x6f}, 1, 0},
    {0xab, {0x03, 0x67}, 2, 0},
    {0xac, {0x03, 0x6b}, 2, 0},
    {0xad, {0x03, 0x68}, 2, 0},
    {0xae, {0x03, 0x6c}, 2, 0},
    {0xb3, {0x00}, 1, 0},
    {0xb4, {0x00}, 1, 0},
    {0xb5, {0x00}, 1, 0},
    {0xb6, {0x32}, 1, 0},
    {0xb7, {0x7e}, 1, 0},
    {0xb8, {0x7e}, 1, 0},
    {0xe0, {0x00}, 1, 0},
    {0xe1, {0x03, 0x0f}, 2, 0},
    {0xe2, {0x04}, 1, 0},
    {0xe3, {0x01}, 1, 0},
    {0xe4, {0x0e}, 1, 0},
    {0xe5, {0x01}, 1, 0},
    {0xe6, {0x19}, 1, 0},
    {0xe7, {0x10}, 1, 0},
    {0xe8, {0x10}, 1, 0},
    {0xea, {0x12}, 1, 0},
    {0xeb, {0xd0}, 1, 0},
    {0xec, {0x04}, 1, 0},
    {0xed, {0x07}, 1, 0},
    {0xee, {0x07}, 1, 0},
    {0xef, {0x09}, 1, 0},
    {0xf0, {0xd0}, 1, 0},
    {0xf1, {0x0e, 0x17}, 2, 0},
    {0xf2, {0x2c, 0x1b, 0x0b, 0x20}, 4, 0},
    {0xe9, {0x29}, 1, 0},
    {0xec, {0x04}, 1, 0},
    {0x35, {0x00}, 1, 0},
    {0x44, {0x00, 0x10}, 2, 0},
    {0x46, {0x10}, 1, 0},
    {0xff, {0x00}, 1, 0},
    {LCD_CMD_COLMOD, {0x05}, 1, 0},
    {LCD_CMD_SLPOUT, {0}, 0, 220},
    {LCD_CMD_DISPON, {0}, 0, 200},
};

esp_err_t esp_lcd_new_panel_nv3007(
    const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel
)
{
    esp_err_t ret = ESP_OK;
    nv3007_panel_t *nv3007 = NULL;

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    ESP_GOTO_ON_FALSE(
        panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_RGB, ESP_ERR_NOT_SUPPORTED, err, TAG,
        "unsupported RGB element order"
    );
    ESP_GOTO_ON_FALSE(
        panel_dev_config->data_endian == LCD_RGB_DATA_ENDIAN_BIG, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB data endian"
    );
    ESP_GOTO_ON_FALSE(panel_dev_config->bits_per_pixel == 16, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");

    nv3007 = calloc(1, sizeof(nv3007_panel_t));
    ESP_GOTO_ON_FALSE(nv3007, ESP_ERR_NO_MEM, err, TAG, "no mem for NV3007 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    nv3007->io = io;
    nv3007->reset_gpio_num = panel_dev_config->reset_gpio_num;
    nv3007->reset_level = panel_dev_config->flags.reset_active_high;
    nv3007->base.del = panel_nv3007_del;
    nv3007->base.reset = panel_nv3007_reset;
    nv3007->base.init = panel_nv3007_init;
    nv3007->base.draw_bitmap = panel_nv3007_draw_bitmap;
    nv3007->base.mirror = panel_nv3007_mirror;
    nv3007->base.swap_xy = panel_nv3007_swap_xy;
    nv3007->base.set_gap = panel_nv3007_set_gap;
    nv3007->base.disp_on_off = panel_nv3007_disp_on_off;
    *ret_panel = &nv3007->base;

    ESP_LOGD(TAG, "new NV3007 panel @%p", nv3007);
    return ESP_OK;

err:
    if (nv3007) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(nv3007);
    }
    return ret;
}

static esp_err_t panel_nv3007_del(esp_lcd_panel_t *panel)
{
    nv3007_panel_t *nv3007 = __containerof(panel, nv3007_panel_t, base);

    if (nv3007->reset_gpio_num >= 0) {
        gpio_reset_pin(nv3007->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del NV3007 panel @%p", nv3007);
    free(nv3007);
    return ESP_OK;
}

static esp_err_t panel_nv3007_reset(esp_lcd_panel_t *panel)
{
    nv3007_panel_t *nv3007 = __containerof(panel, nv3007_panel_t, base);

    if (nv3007->reset_gpio_num >= 0) {
        gpio_set_level(nv3007->reset_gpio_num, nv3007->reset_level);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(nv3007->reset_gpio_num, !nv3007->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(nv3007->io, LCD_CMD_SWRESET, NULL, 0), TAG, "io tx param failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_nv3007_init(esp_lcd_panel_t *panel)
{
    nv3007_panel_t *nv3007 = __containerof(panel, nv3007_panel_t, base);

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(nv3007->io, LCD_CMD_MADCTL, &nv3007->madctl_val, 1), TAG, "io tx param failed");

    for (size_t idx = 0; idx < sizeof(LCD_INIT_SEQ) / sizeof(LCD_INIT_SEQ[0]); idx += 1) {
        const nv3007_init_cmd_t *entry = &LCD_INIT_SEQ[idx];
        const void *data = entry->data_bytes > 0 ? entry->data : NULL;
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_io_tx_param(nv3007->io, entry->cmd, data, entry->data_bytes), TAG, "io tx param failed"
        );
        if (entry->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(entry->delay_ms));
        }
    }

    return ESP_OK;
}

static esp_err_t
panel_nv3007_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    nv3007_panel_t *nv3007 = __containerof(panel, nv3007_panel_t, base);

    x_start += nv3007->x_gap;
    x_end += nv3007->x_gap;
    y_start += nv3007->y_gap;
    y_end += nv3007->y_gap;

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(
            nv3007->io, LCD_CMD_CASET,
            (uint8_t[]){
                (x_start >> 8) & 0xff,
                x_start & 0xff,
                ((x_end - 1) >> 8) & 0xff,
                (x_end - 1) & 0xff,
            },
            4
        ),
        TAG, "io tx param failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(
            nv3007->io, LCD_CMD_RASET,
            (uint8_t[]){
                (y_start >> 8) & 0xff,
                y_start & 0xff,
                ((y_end - 1) >> 8) & 0xff,
                (y_end - 1) & 0xff,
            },
            4
        ),
        TAG, "io tx param failed"
    );

    size_t len = (x_end - x_start) * (y_end - y_start) * 2;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(nv3007->io, LCD_CMD_RAMWR, color_data, len), TAG, "io tx color failed");
    return ESP_OK;
}

static esp_err_t panel_nv3007_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3007_panel_t *nv3007 = __containerof(panel, nv3007_panel_t, base);

    if (mirror_x) {
        nv3007->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        nv3007->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        nv3007->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        nv3007->madctl_val &= ~LCD_CMD_MY_BIT;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(nv3007->io, LCD_CMD_MADCTL, &nv3007->madctl_val, 1), TAG, "io tx param failed");
    return ESP_OK;
}

static esp_err_t panel_nv3007_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    nv3007_panel_t *nv3007 = __containerof(panel, nv3007_panel_t, base);

    if (swap_axes) {
        nv3007->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        nv3007->madctl_val &= ~LCD_CMD_MV_BIT;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(nv3007->io, LCD_CMD_MADCTL, &nv3007->madctl_val, 1), TAG, "io tx param failed");
    return ESP_OK;
}

static esp_err_t panel_nv3007_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    nv3007_panel_t *nv3007 = __containerof(panel, nv3007_panel_t, base);
    nv3007->x_gap = x_gap;
    nv3007->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_nv3007_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    nv3007_panel_t *nv3007 = __containerof(panel, nv3007_panel_t, base);
    int command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(nv3007->io, command, NULL, 0), TAG, "io tx param failed");
    return ESP_OK;
}

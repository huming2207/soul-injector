#pragma once

#include <esp_lcd_panel_dev.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create an LCD panel for the NV3007 controller used by NT279VJ
 *
 * @param[in] io LCD panel IO handle
 * @param[in] panel_dev_config General panel device configuration
 * @param[out] ret_panel Returned LCD panel handle
 * @return
 *          - ESP_ERR_INVALID_ARG if a parameter is invalid
 *          - ESP_ERR_NOT_SUPPORTED if the requested pixel format is unsupported
 *          - ESP_ERR_NO_MEM if panel state allocation fails
 *          - ESP_OK on success
 */
esp_err_t esp_lcd_new_panel_nv3007(
    const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel
);

#ifdef __cplusplus
}
#endif

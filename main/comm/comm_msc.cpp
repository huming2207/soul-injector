#include "comm_msc.hpp"
#include <tusb_console.h>
#include "tusb_msc_storage.h"
#include "tinyusb.h"
#include <tusb_cdc_acm.h>
#include <tusb_console.h>
#include <esp_mac.h>
#include <esp_flash.h>

esp_err_t comm_msc::init()
{
    data_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, PART_NAME);
    if (data_part == nullptr) {
        ESP_LOGE(TAG, "Failed to find partition: %s", PART_NAME);
        return ESP_ERR_NOT_FOUND;
    }

    auto ret = wl_mount(data_part, &wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "Wear level mount error: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Mount data partition");
    const tinyusb_msc_spiflash_config_t spiflash_cfg = { .wl_handle = wl_handle };
    ret = tinyusb_msc_storage_init_spiflash(&spiflash_cfg);
    ret = ret ?: tinyusb_msc_storage_mount(PART_PATH);
    ESP_LOGI(TAG, "Mount OK, now do CDC");

    static char sn_str[32] = {};
    static char lang[2] = {0x09, 0x04};

    static char *desc_str[5] = {
            lang,                // 0: is supported language is English (0x0409)
            const_cast<char *>(USB_DESC_MANUFACTURER), // 1: Manufacturer
            const_cast<char *>(USB_DESC_PRODUCT),      // 2: Product
            sn_str,       // 3: Serials, should use chip ID
            const_cast<char *>(USB_DESC_CDC_NAME),          // 4: CDC Interface
    };

    tinyusb_config_t tusb_cfg = {}; // the configuration using default values
    tusb_cfg.string_descriptor = (const char **)desc_str;
    tusb_cfg.device_descriptor = nullptr;
    tusb_cfg.self_powered = false;
    tusb_cfg.external_phy = false;

    uint8_t sn_buf[16] = { 0 };
    ret = ret ?: esp_efuse_mac_get_default(sn_buf);
    ret = ret ?: esp_flash_read_unique_chip_id(esp_flash_default_chip, reinterpret_cast<uint64_t *>(sn_buf + 6));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Can't read UID: 0x%x", ret);
        return ret;
    }

    tinyusb_config_cdcacm_t acm_cfg = {
            .usb_dev = TINYUSB_USBDEV_0,
            .cdc_port = CDC_CHANNEL,
            .callback_rx = nullptr,
            .callback_rx_wanted_char = nullptr,
            .callback_line_state_changed = nullptr,
            .callback_line_coding_changed = nullptr
    };

    ret = tusb_cdc_acm_init(&acm_cfg);
    ret = ret ?: esp_tusb_init_console(CDC_CHANNEL);

    return ret;
}

esp_err_t comm_msc::unmount_and_expose()
{
    if (tinyusb_msc_storage_in_use_by_usb_host()) {
        ESP_LOGE(TAG, "Already exposed to USB");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Unmount & expose to USB now");
    return tinyusb_msc_storage_unmount();
}

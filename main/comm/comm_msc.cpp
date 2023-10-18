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
    const tinyusb_msc_spiflash_config_t spiflash_cfg = {
            .wl_handle = wl_handle,
            .callback_mount_changed = nullptr,
            .callback_premount_changed = nullptr
    };

    ret = tinyusb_msc_storage_init_spiflash(&spiflash_cfg);
    ret = ret ?: tinyusb_msc_storage_mount(PART_PATH);

    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "Mount failed: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Mount OK, now install USB");

    static char sn_str[32] = {};
    static char lang[2] = {0x09, 0x04};

    static const char *desc_str[5] = {
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
    tusb_cfg.string_descriptor_count = sizeof(desc_str) / sizeof(desc_str[0]);

    uint8_t sn_buf[16] = { 0 };
    ret = ret ?: esp_efuse_mac_get_default(sn_buf);
    ret = ret ?: esp_flash_read_unique_chip_id(esp_flash_default_chip, reinterpret_cast<uint64_t *>(sn_buf + 6));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Can't read UID: 0x%x", ret);
        return ret;
    }

    snprintf(sn_str, 32, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             sn_buf[0], sn_buf[1], sn_buf[2], sn_buf[3], sn_buf[4], sn_buf[5], sn_buf[6], sn_buf[7],
             sn_buf[8], sn_buf[9], sn_buf[10], sn_buf[11], sn_buf[12], sn_buf[13]);

    ESP_LOGI(TAG, "Initialised with SN: %s", sn_str);

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB driver install failed: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "USB install OK");

    msc_evt_group = xEventGroupCreate();
    if (msc_evt_group == nullptr) {
        ESP_LOGE(TAG, "MSC event group init fail: 0x%x", ret);
        return ESP_ERR_NO_MEM;
    }

    return ret;
}

esp_err_t comm_msc::unmount_and_start_msc()
{
    if (tinyusb_msc_storage_in_use_by_usb_host()) {
        ESP_LOGE(TAG, "Already exposed to USB");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Unmount & expose to USB now");
    auto ret = tinyusb_msc_storage_unmount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Unmount/Expose failed");
        return ret;
    }

    xEventGroupClearBits(msc_evt_group, comm::MSC_MOUNTED);
    return ret;
}

esp_err_t comm_msc::mount_and_stop_msc()
{
    auto ret = tinyusb_msc_storage_mount(PART_PATH);
    if (ret == ESP_OK) {
        xEventGroupSetBits(msc_evt_group, comm::MSC_MOUNTED);
    }

    return ret;
}

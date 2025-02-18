#include <esp_event.h>
#include <nvs_flash.h>
#include "bootstrap_fsm.hpp"
#include "fw_asset_manager.hpp"
#include "http_downloader.hpp"
#include "thumbconfig/tcfg_client.hpp"
#include "thumbconfig/tcfg_wire_usb_cdc.hpp"

esp_err_t bootstrap_fsm::init()
{
    ESP_LOGI(TAG, "Setting up display");
    display = display_manager::instance();
    composer = display->get_composer();
    esp_err_t ret = display->init();
    ret = ret ?: composer->display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up display: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Setting up storage");
    ret = setup_storage();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up storage: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Set up provisioning");
    auto *prov_client = tcfg_client::instance();
    auto *usb_cdc = tcfg_wire_usb_cdc::instance();
    ret = usb_cdc->init();
    ret = ret ?: prov_client->init(usb_cdc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up TCFG provisioning: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Loading asset");
    auto *asset = fw_asset_manager::instance();
    ret = asset->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load assets: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t bootstrap_fsm::setup_storage()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        ret = ret ?: nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up NVS: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_vfs_littlefs_register(&lfs_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up storage: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    return ret;
}


#include <cstring>
#include <esp_log.h>
#include "lcd/display_manager.hpp"
#include "thumbconfig/tcfg_client.hpp"
#include "thumbconfig/tcfg_wire_usb_cdc.hpp"
#include "esp_littlefs.h"
#include "fw_asset_manager.hpp"

extern "C" void app_main(void)
{
    static const char *TAG = "main";
    ESP_LOGI(TAG, "Started");


//    auto &flasher = swd_headless_flasher::instance();
//    flasher.init();

    // ESP_ERROR_CHECK(comm_msc::instance()->init());

//    ESP_ERROR_CHECK(local_mission_manager::instance().init());
//
//    ESP_ERROR_CHECK(button_manager::instance()->init());
//    button_manager::instance()->set_handler(nullptr, 5000, 50);
//
//    uint32_t pc_init = 0;
//    ESP_ERROR_CHECK(local_mission_manager::instance().get_pc_init(&pc_init));
//    ESP_LOGI(TAG, "pc_init: 0x%08lx", pc_init);
//

    auto *display = display_manager::instance();
    ESP_ERROR_CHECK(display->init());

    auto *composer = display->get_composer();
    ESP_ERROR_CHECK(composer->display_init());


    auto *prov_client = tcfg_client::instance();
    auto *usb_cdc = tcfg_wire_usb_cdc::instance();
    ESP_ERROR_CHECK(usb_cdc->init());
    ESP_ERROR_CHECK(prov_client->init(usb_cdc));

    auto *asset = fw_asset_manager::instance();
    ESP_ERROR_CHECK(asset->init());
    for (auto &it : asset->get_test_items()) {
        ESP_LOGI(TAG, "Test item type %u: %u %s", it.type, it.id, it.name);
    }

//
//
//    vTaskDelay(pdMS_TO_TICKS(3000));
//    ESP_ERROR_CHECK(composer->display_error("ERROR", "Stop eating banana"));
//
//    vTaskDelay(pdMS_TO_TICKS(3000));
//    for (uint32_t idx = 0; idx <= 100; idx += 1) {
//        composer->display_program(idx);
//        vTaskDelay(pdMS_TO_TICKS(60));
//    }
//
//
//    vTaskDelay(pdMS_TO_TICKS(3000));
////
////
//    for (uint32_t idx = 0; idx <= 100; idx += 1) {
//        composer->display_test(idx, 100, "Something I don't know");
//        vTaskDelay(pdMS_TO_TICKS(80)) ;
//    }
//
//    vTaskDelay(pdMS_TO_TICKS(3000));
//    composer->display_done();
//
//    vTaskDelay(pdMS_TO_TICKS(3000));
//    composer->display_current(3.23456, 567.890, "PASS", lv_color_make(0x00, 0xff, 0));

    vTaskDelay(portMAX_DELAY);
}
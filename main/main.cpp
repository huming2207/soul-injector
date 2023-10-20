#include <cstring>
#include <esp_log.h>
#include <swd_headless_flasher.hpp>
#include "lcd/display_manager.hpp"
#include "comm_msc.hpp"

extern "C" void app_main(void)
{
    static const char *TAG = "main";
    ESP_LOGI(TAG, "Started");

//    auto &ble_serv = ble_serv_mgr::instance();
//    ESP_ERROR_CHECK(ble_serv.init());
//
//    auto &flasher = swd_headless_flasher::instance();
//    flasher.init();

    ESP_ERROR_CHECK(comm_msc::instance()->init());

    ESP_ERROR_CHECK(local_mission_manager::instance().init());

    uint32_t pc_init = 0;
    ESP_ERROR_CHECK(local_mission_manager::instance().get_pc_init(&pc_init));
    ESP_LOGI(TAG, "pc_init: 0x%08lx", pc_init);

    auto *display = display_manager::instance();
    ESP_ERROR_CHECK(display->init());

    vTaskDelay(portMAX_DELAY);
}
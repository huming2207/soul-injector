#include <cstring>
#include <esp_log.h>
#include <swd_headless_flasher.hpp>
#include "lcd/display_manager.hpp"
#include "comm_msc.hpp"
#include "button_manager.hpp"

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

    ESP_ERROR_CHECK(button_manager::instance()->init());
    button_manager::instance()->set_handler(nullptr, 5000, 50);

    uint32_t pc_init = 0;
    ESP_ERROR_CHECK(local_mission_manager::instance().get_pc_init(&pc_init));
    ESP_LOGI(TAG, "pc_init: 0x%08lx", pc_init);

    auto *display = display_manager::instance();
    ESP_ERROR_CHECK(display->init());

    auto *ui_cmder = ui_commander::instance();
    ui_cmder->init();
    ui_cmder->display_init();
    vTaskDelay(pdMS_TO_TICKS(3000));
    ui_cmder->display_chip_erase();

    for (uint32_t idx = 0; idx <= 100; idx += 1) {
        ui_state::flash_screen screen {};
        screen.percentage = idx;
        ui_cmder->display_flash(&screen);
        vTaskDelay(pdMS_TO_TICKS(60));
    }


    for (uint32_t idx = 0; idx <= 32; idx += 1) {
        ui_state::test_screen screen {};
        screen.total_test = 32;
        screen.done_test = idx;
        strcpy(screen.subtitle, "Radio Tx");
        ui_cmder->display_test(&screen);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
    ui_cmder->display_done();
    vTaskDelay(pdMS_TO_TICKS(3000));
    ui_cmder->display_usb();

    vTaskDelay(portMAX_DELAY);
}
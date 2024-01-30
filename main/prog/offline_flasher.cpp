#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_log.h>
#include <led_ctrl.hpp>
#include <esp_crc.h>
#include <esp_timer.h>

#include "offline_flasher.hpp"

esp_err_t offline_flasher::init()
{
    auto ret = disp->init();
    ret = ret ?: ui_cmder->init();

    if (ret != ESP_OK) return ret;

    while (true) {
        switch (state) {
            case flasher::DETECT: {
                on_detect();
                break;
            }

            case flasher::ERASE: {
                on_erase();
                break;
            }

            case flasher::PROGRAM: {
                on_program();
                break;
            }

            case flasher::ERROR: {
                on_error();
                break;
            }

            case flasher::DONE: {
                on_done();
                break;
            }

            case flasher::VERIFY: {
                on_verify();
                break;
            }

            case flasher::SELF_TEST: {
                on_self_test();
                break;
            }
        }
    }

    return ret;
}

void offline_flasher::on_error()
{

}

void offline_flasher::on_erase()
{
    ESP_LOGI(TAG, "Erasing");
    ui_cmder->display_chip_erase();
    uint32_t start_addr = 0, end_addr = 0;
    auto ret = asset->get_flash_start_addr(&start_addr);
    ret = ret ?: asset->get_flash_end_addr(&end_addr);
    if (ret != ESP_OK) {
        ui_state::error_screen error = {};
        strcpy(error.subtitle, "No flash address");
        ui_cmder->display_error(&error);

        state = flasher::ERROR;
        ESP_LOGE(TAG, "Failed to read flash addresses");
    } else {
        ret = swd->erase_chip();
        if (ret != ESP_OK) {
            ret = swd->erase_sector(start_addr, end_addr);
        }

        if (ret != ESP_OK) {
            ui_state::error_screen error = {};
            snprintf(error.subtitle, sizeof(error.subtitle), "Erase failed\nCode: 0x%x", ret);
            ui_cmder->display_error(&error);
            state = flasher::ERROR;
        }
    }

    state = flasher::PROGRAM;
}

void offline_flasher::on_program()
{
    int64_t ts = esp_timer_get_time();

    ui_state::flash_screen flash = {};
    ui_cmder->display_flash(&flash);
    auto ret = swd->program_file(offline_asset_manager::FIRMWARE_PATH, &written_len);
    if (ret != ESP_OK) {
        ui_state::error_screen error = {};
        snprintf(error.subtitle, sizeof(error.subtitle), "Prog failed\nCode: 0x%x", ret);
        ui_cmder->display_error(&error);
        state = flasher::ERROR;
    } else {
        ts = esp_timer_get_time() - ts;
        double speed = written_len / ((double)ts / 1000000.0);
        ESP_LOGI(TAG, "Firmware written, len: %lu, speed: %.2f bytes per sec", written_len, speed);
        state = flasher::VERIFY;
    }

}

void offline_flasher::on_detect()
{
    ESP_LOGI(TAG, "Detecting");
    auto ret = swd->init(asset);
    while (ret != ESP_OK) {
        ui_cmder->display_init();
        ESP_LOGE(TAG, "Detect failed, retrying");
        ret = swd->init(asset);
    }

    state = flasher::ERASE; // To erase
}

void offline_flasher::on_done()
{
    ui_cmder->display_done();
}

void offline_flasher::on_verify()
{
    uint32_t crc = 0;
    if (offline_asset_manager::instance()->get_fw_crc(&crc) != ESP_OK) {
        state = flasher::ERROR;
        return;
    }

    ui_state::test_screen test = {};
    test.done_test = 0;
    test.total_test = 0;
    strcpy(test.subtitle, "Verify prog");
    ui_cmder->display_test(&test);
    auto ret = swd->verify(crc, UINT32_MAX, written_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to verify!");
        ui_state::error_screen error = {};
        snprintf(error.subtitle, sizeof(error.subtitle), "Verify failed\nCode: 0x%x", ret);
        ui_cmder->display_error(&error);
        state = flasher::ERROR;
    } else {
        ESP_LOGI(TAG, "Firmware verified");
        state = flasher::SELF_TEST;
    }
}


void offline_flasher::on_self_test()
{
    ESP_LOGI(TAG, "Run self test");

    const std::vector<flash_algo::test_item> &items = asset->get_test_items();
    for (size_t idx = 0; idx < items.size(); idx += 1) {
        ui_state::test_screen test = {};
        test.total_test = items.size();
        test.done_test = idx;
        ui_cmder->display_test(&test);

        if (items[idx].type == flash_algo::INTERNAL_SIMPLE_TEST) {
            uint32_t func_ret = UINT32_MAX;
            auto ret = swd->self_test(items[idx].id, nullptr, 0, &func_ret);
            ESP_LOGW(TAG, "Self test OK, host returned 0x%x, function returned 0x%lx", ret, func_ret);
            if (ret == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "No self test config found, skipping");
                state = flasher::DONE;
                return;
            } else if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Self test failed, host returned 0x%x, function returned 0x%lx", ret, func_ret);
                state = flasher::ERROR;
                return;
            }
        } else if (items[idx].type == flash_algo::INTERNAL_EXTEND_TEST) {
            ESP_LOGW(TAG, "Unsupported InternalExtendTest type!");
        } else if (items[idx].type == flash_algo::EXTERNAL_TEST) {
            ESP_LOGW(TAG, "Unsupported ExternalTest type!");
        }

    }

    swd_prog::trigger_nrst();

    state = flasher::DONE;
}

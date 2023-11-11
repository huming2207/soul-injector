#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <led_ctrl.hpp>
#include <esp_crc.h>
#include <esp_timer.h>

#include "offline_flasher.hpp"
#include "cdc_acm.hpp"

esp_err_t offline_flasher::init()
{
    auto ret = disp->init();


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
    uint32_t start_addr = 0, end_addr = 0;
    auto ret = asset->get_flash_start_addr(&start_addr);
    ret = ret ?: asset->get_flash_end_addr(&end_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read flash addresses");
    } else {
        ret = swd->erase_chip();
        if (ret != ESP_OK) {
            ret = swd->erase_sector(start_addr, end_addr);
        }
    }

    if (ret != ESP_OK) {
        for (uint32_t idx = 0; idx < 32; idx++) {
            on_error();
        }

        state = flasher::DETECT;
    }

    state = flasher::PROGRAM;
}

void offline_flasher::on_program()
{
    int64_t ts = esp_timer_get_time();
    auto ret = swd->program_file(offline_asset_manager::FIRMWARE_PATH, &written_len);
    if (ret != ESP_OK) {
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
        on_error();
        ESP_LOGE(TAG, "Detect failed, retrying");
        ret = swd->init(asset);
    }

    state = flasher::ERASE; // To erase
}

void offline_flasher::on_done()
{

}

void offline_flasher::on_verify()
{
    uint32_t crc = 0;
    if (offline_asset_manager::instance()->get_fw_crc(&crc) != ESP_OK) {
        state = flasher::ERROR;
        return;
    }

    if (swd->verify(crc, UINT32_MAX, written_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to verify!");
        state = flasher::ERROR;
    } else {
        ESP_LOGI(TAG, "Firmware verified");
        state = flasher::SELF_TEST;
    }
}


void offline_flasher::on_self_test()
{
    ESP_LOGI(TAG, "Run self test");

    // TODO: just testing
    uint32_t func_ret = UINT32_MAX;
    auto ret = swd->self_test(0x004, nullptr, 0, &func_ret);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "No self test config found, skipping");
        state = flasher::DONE;
        return;
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Self test failed, host returned 0x%x, function returned 0x%lx", ret, func_ret);
        state = flasher::ERROR;
        return;
    }

    ESP_LOGW(TAG, "Self test OK, host returned 0x%x, function returned 0x%lx", ret, func_ret);

    ret = swd->self_test(0x003, nullptr, 0, &func_ret);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "No self test config found, skipping");
        state = flasher::DONE;
        return;
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Self test failed, host returned 0x%x, function returned 0x%lx", ret, func_ret);
        state = flasher::ERROR;
        return;
    }

    ESP_LOGW(TAG, "Self test OK, host returned 0x%x, function returned 0x%lx", ret, func_ret);

    swd_prog::trigger_nrst();

    state = flasher::DONE;
}

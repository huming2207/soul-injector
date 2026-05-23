#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_log.h>
#include <led_ctrl.hpp>
#include <fw_asset_manager.hpp>
#include <esp_crc.h>
#include <esp_timer.h>

#include "offline_flasher.hpp"

void offline_flasher::init(bool force_reload_asset)
{
    display = display_manager::instance();
    composer = display->get_composer();
    if (force_reload_asset) {
        asset_loaded = false;
    }

    state = flasher::LOAD_ASSET;
}

void offline_flasher::on_error()
{

}

void offline_flasher::on_erase()
{
    ESP_LOGI(TAG, "Erasing");
    composer->display_erase(UINT8_MAX);
    uint32_t start_addr = 0, end_addr = 0;
    auto *asset = fw_asset_manager::instance();
    auto ret = asset->get_flash_start_addr(&start_addr);
    ret = ret ?: asset->get_flash_end_addr(&end_addr);
    if (ret != ESP_OK) {
        composer->display_error("CFG ERROR", "Flash address\nnot provided");
        state = flasher::ERROR;
        ESP_LOGE(TAG, "Failed to read flash addresses");
    } else {
        ret = swd->erase_chip();
        if (ret != ESP_OK) {
            ret = swd->erase_sector(start_addr, end_addr);
        }

        if (ret != ESP_OK) {
            composer->display_error("ERROR", "Cannot erase target!\nPlease try again!");
            state = flasher::ERROR;
        }
    }

    state = flasher::PROGRAM;
}

void offline_flasher::on_program()
{
    composer->display_program(100);
    int64_t ts = esp_timer_get_time();
    auto ret = swd->program_file(fw_asset_manager::FIRMWARE_PATH, &written_len);
    if (ret != ESP_OK) {
        composer->display_error("ERROR", "Cannot program target!\nPlease try again!");
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

    composer->display_init();
    auto ret = swd->init();
    while (ret != ESP_OK) {
        ESP_LOGE(TAG, "Detect failed, retrying");
        ret = swd->init();
    }

    state = flasher::ERASE; // To erase
}

void offline_flasher::on_done()
{
    composer->display_done();
}

void offline_flasher::on_verify()
{
    composer->display_program(100);
    //ui_cmder->display_test(&test);
    auto ret = swd->verify(fw_asset_manager::FIRMWARE_PATH, UINT32_MAX, written_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to verify!");
        composer->display_error("ERROR", "Failed to verify\nPlease try again");
        state = flasher::ERROR;
    } else {
        ESP_LOGI(TAG, "Firmware verified");
        state = flasher::SELF_TEST;
    }
}


void offline_flasher::on_self_test()
{
    ESP_LOGI(TAG, "Run self test");

    auto *asset = fw_asset_manager::instance();
    const std::vector<flash_algo::test_item> &items = asset->get_test_items();
    for (size_t idx = 0; idx < items.size(); idx += 1) {
        composer->display_test(idx, (items.size() - 1), nullptr);

        if (items[idx].type == flash_algo::INTERNAL_SIMPLE_TEST) {
            uint32_t func_ret = UINT32_MAX;
            ESP_LOGW(TAG, "Self test: %u, %s type %u", items[idx].id, items[idx].name, items[idx].type);
            auto ret = swd->self_test(items[idx].id, nullptr, 0, &func_ret);
            if (ret == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "No self test config found, skipping");
                state = flasher::DONE;
                return;
            } else if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Self test #%u failed, host error 0x%x, function returned 0x%lx", items[idx].id, ret, func_ret);
                char msg[128] = { 0 };
                snprintf(msg, sizeof(msg), "Test failed at\n%u of %u ID %u;\n%s", idx + 1, items.size(), items[idx].id, items[idx].name);
                composer->display_error("ERROR", msg);
                state = flasher::ERROR;
                return;
            }

            if (func_ret != 0) {
                ESP_LOGW(TAG, "Self test failed, host returned 0x%x, target returned error 0x%lx", ret, func_ret);
                char msg[128] = { 0 };
                snprintf(msg, sizeof(msg), "Test failed at ID %u;\n%s\nRet=%lu", items[idx].id, items[idx].name, func_ret);
                composer->display_error("ERROR", msg);
                state = flasher::ERROR;
                return;
            }


            ESP_LOGW(TAG, "Self test OK, host returned 0x%x, function returned 0x%lx", ret, func_ret);

        } else if (items[idx].type == flash_algo::INTERNAL_EXTEND_TEST) {
            ESP_LOGW(TAG, "Unsupported InternalExtendTest type!");
        } else if (items[idx].type == flash_algo::EXTERNAL_TEST) {
            ESP_LOGW(TAG, "Unsupported ExternalTest type!");
        }

    }

    swd_prog::trigger_nrst();

#ifndef CONFIG_SI_SG_PROG_RIG
    state = flasher::DONE;
#else
    state = flasher::SG_CURRENT_TEST;
#endif
}


#ifdef CONFIG_SI_SG_PROG_RIG
void offline_flasher::on_current_test()
{
    // Shut up the SWD to run firmware
    swd_prog::reset_gpio();

    double min = 0, max = 0, avg = 0;
    auto ret = pwr_test.init((gpio_num_t)CONFIG_SI_SG_PWR_TESTER_ALERT);
    ret = ret ?: pwr_test.start_testing(3000, &max, &min, &avg);
    if (ret != ESP_OK) {
        composer->display_error("ERROR", "Current sensor error");
        state = flasher::ERROR;
        ESP_LOGE(TAG, "curr_test: failed to start testing 0x%x", ret);
        return;
    }

    ESP_LOGI(TAG, "Min=%.6f Max=%.6f, Avg=%.6f, ret=0x%x", min * 1000, max * 1000, avg * 1000, ret);
    composer->display_current(min * 1000000, max * 1000000, avg * 1000000, "OK", lv_color_make(0, 0xff, 0));
    state = flasher::DONE;
    vTaskDelay(pdMS_TO_TICKS(10000));
}
#endif

esp_err_t offline_flasher::handle_states()
{
    switch (state) {
        case flasher::LOAD_ASSET: {
            on_load_asset();
            break;
        }

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
            return ESP_FAIL;
        }

        case flasher::DONE: {
            on_done();
            return ESP_OK;
        }

        case flasher::VERIFY: {
            on_verify();
            break;
        }

        case flasher::SELF_TEST: {
            on_self_test();
            break;
        }

#ifdef CONFIG_SI_SG_PROG_RIG
        case flasher::SG_CURRENT_TEST: {
            on_current_test();
            break;
        }
#endif
    }

    return ESP_ERR_NOT_FINISHED;
}

void offline_flasher::on_load_asset()
{
    if (asset_loaded) {
        ESP_LOGW(TAG, "load_asset: already loaded, skipping");
        state = flasher::DETECT;
        return;
    }

    ESP_LOGI(TAG, "load_asset: Loading asset");
    auto *asset = fw_asset_manager::instance();
    auto ret = asset->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load assets: 0x%x %s", ret, esp_err_to_name(ret));
        composer->display_error("ERROR", "No firmware asset\nPlease load firmware on me!");
        state = flasher::ERROR;
        return;
    }

    asset_loaded = true;
    state = flasher::DETECT;
}

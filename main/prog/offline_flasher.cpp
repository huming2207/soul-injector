#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "offline_flasher.hpp"

#include "esp32_serial_backend.hpp"
#include "fw_asset_manager.hpp"
#include "swd_cortexm_backend.hpp"

void offline_flasher::init(bool force_reload_asset)
{
    display = display_manager::instance();
    composer = display->get_composer();
    if (force_reload_asset) {
        asset_loaded = false;
    }

    state = flasher::LOAD_ASSET;
}

void offline_flasher::select_backend()
{
    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    switch (cfg.family) {
    case si::config::target_family::esp32_serial:
        backend = esp32_serial_backend::instance();
        break;
    case si::config::target_family::swd_cortex_m:
    default:
        backend = swd_cortexm_backend::instance();
        break;
    }
    ESP_LOGI(TAG, "select_backend: using %s", backend->name());
}

void offline_flasher::on_error()
{
    led.set_color(0x80, 0x00, 0x00);
}

void offline_flasher::on_erase()
{
    ESP_LOGI(TAG, "Erasing");
    composer->display_erase(UINT8_MAX);

    auto ret = backend->erase();
    if (ret == ESP_ERR_INVALID_STATE) {
        composer->display_error("CFG ERROR", "Flash address\nnot provided");
        state = flasher::ERROR;
        ESP_LOGE(TAG, "Failed to read flash addresses");
    } else if (ret != ESP_OK) {
        composer->display_error("ERROR", "Cannot erase target!\nPlease try again!");
        state = flasher::ERROR;
    } else {
        state = flasher::PROGRAM;
    }
}

void offline_flasher::on_program()
{
    composer->display_program(100);
    auto ret = backend->program(&written_len);
    if (ret != ESP_OK) {
        composer->display_error("ERROR", "Cannot program target!\nPlease try again!");
        state = flasher::ERROR;
    } else {
        state = flasher::VERIFY;
    }
}

void offline_flasher::on_detect()
{
    ESP_LOGI(TAG, "Detecting");

    composer->display_init();
    auto ret = backend->detect();
    uint32_t max_retry = 10;
    while (ret != ESP_OK && max_retry > 0) {
        ESP_LOGE(TAG, "Detect failed, retrying");
        max_retry -= 1;
        vTaskDelay(pdMS_TO_TICKS(30));
        ret = backend->detect();
    }

    if (max_retry == 0 && ret != ESP_OK) {
        state = flasher::ERROR;
    } else {
        state = flasher::ERASE; // To erase
    }
}

void offline_flasher::on_done()
{
    led.set_color(0, 80, 0); // Green
    composer->display_done();
}

void offline_flasher::on_verify()
{
    led.set_color(0x68, 0x26, 0x99); // Purple?
    composer->display_program(100);
    auto ret = backend->verify(written_len);
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
    led.set_color(0, 0xb7, 0xeb); // Cyan??
    ESP_LOGI(TAG, "Run self test");

    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    for (size_t idx = 0; idx < cfg.test_count; idx += 1) {
        const si::config::test_item &item = cfg.tests[idx];
        composer->display_test(idx, (cfg.test_count - 1), nullptr);

        if (item.type == si::config::test_item::INTERNAL_SIMPLE_TEST) {
            uint32_t func_ret = UINT32_MAX;
            ESP_LOGW(TAG, "Self test: 0x%08lx, %s type %u", item.addr, item.name, item.type);
            auto ret = backend->self_test(item, &func_ret);
            if (ret == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "No self test support for this target, skipping");
                state = flasher::DONE;
                return;
            } else if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Self test @ 0x%08lx failed, host error 0x%x, function returned 0x%lx", item.addr, ret, func_ret);
                char msg[128] = {0};
                snprintf(msg, sizeof(msg), "Test failed at\n%u of %u @ 0x%08lx;\n%s", idx + 1, cfg.test_count, item.addr,
                         item.name);
                composer->display_error("ERROR", msg);
                state = flasher::ERROR;
                return;
            }

            if (func_ret != 0) {
                ESP_LOGW(TAG, "Self test failed, host returned 0x%x, target returned error 0x%lx", ret, func_ret);
                char msg[128] = {0};
                snprintf(msg, sizeof(msg), "Test failed @ 0x%08lx;\n%s\nRet=%lu", item.addr, item.name, func_ret);
                composer->display_error("ERROR", msg);
                state = flasher::ERROR;
                return;
            }

            ESP_LOGW(TAG, "Self test OK, host returned 0x%x, function returned 0x%lx", ret, func_ret);

        } else if (item.type == si::config::test_item::INTERNAL_EXTEND_TEST) {
            ESP_LOGW(TAG, "Unsupported InternalExtendTest type!");
        }
    }

    backend->reset_target();
    state = flasher::POST_PROGRAM;
}

void offline_flasher::on_post_program()
{
    backend->begin_session();

    auto ret = post_program_steps.load_yaml(POST_PROG_STEP_FILE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "post_prog: Can't load YAML, skipping");
#ifndef CONFIG_SI_SG_PROG_RIG
        state = flasher::DONE;
#else
        state = flasher::SG_CURRENT_TEST;
#endif
        return;
    }

    ret = post_program_steps.execute(*backend);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "post_prog: execution error: 0x%x", ret);
        composer->display_error("ERROR", "Post-program fail");
        state = flasher::ERROR;
        return;
    }

    ESP_LOGI(TAG, "post_prog: OK");

    backend->release_transport();

#ifndef CONFIG_SI_SG_PROG_RIG
    state = flasher::DONE;
#else
    state = flasher::SG_CURRENT_TEST;
#endif
}

#ifdef CONFIG_SI_SG_PROG_RIG
void offline_flasher::on_current_test()
{
    // Shut up the target transport to run firmware
    backend->release_transport();

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
    case flasher::PRE_PROGRAM: {
        on_pre_program();
        break;
    }

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

    case flasher::POST_PROGRAM: {
        on_post_program();
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

void offline_flasher::on_pre_program()
{
    led.set_color(0, 0xb7, 0xeb); // Cyan??
    backend->begin_session();

    auto ret = pre_program_steps.load_yaml(PRE_PROG_STEP_FILE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pre_prog: Can't load YAML, skipping");
        state = flasher::DETECT; // To detect
        return;
    }

    ret = pre_program_steps.execute(*backend);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "pre_prog: execution error: 0x%x", ret);
        composer->display_error("ERROR", "Pre-program fail");
        state = flasher::ERROR;
        return;
    }

    ESP_LOGI(TAG, "pre_prog: OK");
    state = flasher::DETECT; // To detect
}

void offline_flasher::on_load_asset()
{
    if (asset_loaded) {
        ESP_LOGW(TAG, "load_asset: already loaded, skipping");
        state = flasher::PRE_PROGRAM;
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

    select_backend();
    asset_loaded = true;
    state = flasher::PRE_PROGRAM;
}

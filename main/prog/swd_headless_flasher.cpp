#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <led_ctrl.hpp>
#include <esp_crc.h>
#include <esp_timer.h>

#include "swd_headless_flasher.hpp"
#include "cdc_acm.hpp"

esp_err_t swd_headless_flasher::init()
{
    flasher_evt = xEventGroupCreate();
    if (flasher_evt == nullptr) {
        ESP_LOGE(TAG, "Failed to create flasher event group!");
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(button_intr_handler, "button_intr", 3072, this, tskIDLE_PRIORITY + 1, nullptr);

    auto ret = gpio_install_isr_service(0);
    ret = ret ?: gpio_set_intr_type(GPIO_NUM_0, GPIO_INTR_NEGEDGE);
    ret = ret ?: gpio_intr_enable(GPIO_NUM_0);
    ret = ret ?: gpio_isr_handler_add(GPIO_NUM_0, button_isr, this);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up button GPIO");
        return ret;
    }

    ret = led.init((gpio_num_t)(CONFIG_SI_LED_SIGNAL_PIN));
    led.set_color(60,0,0,30);

    //ret = ret ?: lcd->init();

    ret = ret ?: cfg_manager.init();
    ret = ret ?: cdc->init();

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

void swd_headless_flasher::on_error()
{
    led.set_color(50, 0, 0, 50);
    vTaskDelay(pdMS_TO_TICKS(300));
    led.set_color(0, 0, 0, 50);
    vTaskDelay(pdMS_TO_TICKS(300));
}

void swd_headless_flasher::on_erase()
{
    ESP_LOGI(TAG, "Erasing");
    uint32_t start_addr = 0, end_addr = 0;
    auto ret = cfg_manager.get_flash_start_addr(&start_addr);
    ret = ret ?: cfg_manager.get_flash_end_addr(&end_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read flash addresses");
    } else {
        ret = swd.erase_chip();
        if (ret != ESP_OK) {
            ret = swd.erase_sector(start_addr, end_addr);
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

void swd_headless_flasher::on_program()
{
    int64_t ts = esp_timer_get_time();
    auto ret = swd.program_file(manifest_manager::FIRMWARE_PATH, &written_len);
    if (ret != ESP_OK) {
        state = flasher::ERROR;
    } else {
        ts = esp_timer_get_time() - ts;
        double speed = written_len / ((double)ts / 1000000.0);
        ESP_LOGI(TAG, "Firmware written, len: %lu, speed: %.2f bytes per sec", written_len, speed);
        state = flasher::VERIFY;
    }

    cdc->resume_recv();
}

void swd_headless_flasher::on_detect()
{
    ESP_LOGI(TAG, "Detecting");
    auto ret = swd.init(&cfg_manager);
    while (ret != ESP_OK) {
        on_error();
        ESP_LOGE(TAG, "Detect failed, retrying");
        ret = swd.init(&cfg_manager);
    }

    cdc->pause_recv();
    state = flasher::ERASE; // To erase
}

void swd_headless_flasher::on_done()
{
    led.set_color(0, 50, 0, 50);
    vTaskDelay(pdMS_TO_TICKS(50));
    led.set_color(0, 0, 0, 50);
    vTaskDelay(pdMS_TO_TICKS(500));
}

void swd_headless_flasher::on_verify()
{
    uint32_t crc = 0;
    if (manifest_manager::instance().get_fw_crc(&crc) != ESP_OK) {
        state = flasher::ERROR;
        return;
    }

    if (swd.verify(crc, UINT32_MAX, written_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to verify!");
        state = flasher::ERROR;
    } else {
        ESP_LOGI(TAG, "Firmware verified");
        state = flasher::SELF_TEST;
    }
}


void swd_headless_flasher::on_self_test()
{
    ESP_LOGI(TAG, "Run self test");

    led.set_color(100, 0, 100, 0);

    // TODO: just testing
    uint32_t func_ret = UINT32_MAX;
    auto ret = swd.self_test(0x004, nullptr, 0, &func_ret);
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

    ret = swd.self_test(0x003, nullptr, 0, &func_ret);
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

void swd_headless_flasher::button_isr(void *_ctx)
{
    auto *ctx = static_cast<swd_headless_flasher *>(_ctx);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    BaseType_t ret = xEventGroupSetBitsFromISR(ctx->flasher_evt, flasher::CLEAR_BUTTON_PRESSED, &xHigherPriorityTaskWoken);
    if (ret == pdPASS) {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void swd_headless_flasher::button_intr_handler(void *_ctx)
{
    auto *ctx = static_cast<swd_headless_flasher *>(_ctx);
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(ctx->flasher_evt,
                                               flasher::CLEAR_BUTTON_PRESSED, // Wait for button pressed bit
                                               pdTRUE, pdTRUE,       // Clear on exit, wait for all
                                               portMAX_DELAY);         // Timeout


        if((bits & flasher::CLEAR_BUTTON_PRESSED) != 0) {
            ESP_LOGI(TAG, "Button pressed!");
            if (ctx->state == flasher::DONE || ctx->state == flasher::ERROR) {
                ctx->state = flasher::DETECT;
            }
        } else {
            vTaskDelete(nullptr);
            return; // Won't happen
        }
    }
}

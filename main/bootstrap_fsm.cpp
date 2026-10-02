#include <esp_event.h>
#include <nvs_flash.h>
#include <sys/stat.h>
#include "bootstrap_fsm.hpp"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "fw_asset_manager.hpp"
#include "http_downloader.hpp"
#include "job_controller.hpp"
#include "offline_flasher.hpp"
#include "sidp_service.hpp"
#include "sidp_transport_cdc.hpp"
#include "driver/i2c_master.h"

esp_err_t bootstrap_fsm::init()
{
    ESP_LOGI(TAG, "Setting up display");
    display = display_manager::instance();
    auto &led = led_ctrl::instance();
    esp_err_t ret = display->init();
    composer = display->get_composer();
    ret = ret ?: composer->init();
    ret = ret ?: led.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up display: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    evt_group = xEventGroupCreate();
    if (evt_group == nullptr) {
        ESP_LOGE(TAG, "Can't create event group");
        return ESP_FAIL;
    }

    det_debounce_timer = xTimerCreate("target_det", pdMS_TO_TICKS(50), pdFALSE, this, det_pin_debounce_timer);
    if (det_debounce_timer == nullptr) {
        ESP_LOGE(TAG, "Can't create debounce timer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Setting up detection pin");
    gpio_config_t det_io_cfg = {};
    det_io_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    det_io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    det_io_cfg.pin_bit_mask = (1 << DET_IO_PIN);
    det_io_cfg.intr_type = GPIO_INTR_ANYEDGE;
    det_io_cfg.mode = GPIO_MODE_INPUT;

    ret = gpio_config(&det_io_cfg);
    if (ret == ESP_OK) {
        last_det_state = gpio_get_level(DET_IO_PIN);
    }
    gpio_install_isr_service(0);
    ret = ret ?: gpio_set_intr_type(DET_IO_PIN, GPIO_INTR_ANYEDGE);
    ret = ret ?: gpio_intr_enable(DET_IO_PIN);
    ret = ret ?: gpio_isr_handler_add(DET_IO_PIN, det_io_isr_handler, det_debounce_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button setup failed: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Setting up storage");
    ret = setup_storage();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up storage: 0x%x %s", ret, esp_err_to_name(ret));
        composer->display_error("ERROR", "Storage partition error\nPlease try factory reset");
        return ret;
    }

    // No stored job is fine: the host pushes one over SIDP.
    fw_asset_manager::instance()->init();
    ret = job_controller::instance()->init(evt_group, BIT_RUN_REQUEST);
    ret = ret ?: setup_usb();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up USB/SIDP: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    BaseType_t task_ret = xTaskCreate(fsm_task_handler, "flasher", 8192, this, tskIDLE_PRIORITY + 10, &fsm_task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Can't create FSM task");
        return ESP_FAIL;
    }

    // Now set the initial detection state
    if (last_det_state == 0) {
        ESP_LOGI(TAG, "Target detected at boot time!");
        xEventGroupSetBits(evt_group, BIT_TARGET_CONNECTED);
    } else {
        ESP_LOGI(TAG, "No target detected at boot time.");
    }

    ESP_LOGI(TAG, "Bootstrap init OK");
    return ESP_OK;
}

esp_err_t bootstrap_fsm::setup_storage()
{
    uint8_t sn_buf[16] = {0};
    uint64_t flash_uid = 0;
    esp_efuse_mac_get_default(sn_buf);
    esp_flash_read_unique_chip_id(esp_flash_default_chip, &flash_uid);
    memcpy(sn_buf + 6, &flash_uid, sizeof(uint64_t));
    snprintf(
        sn_str, sizeof(sn_str), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", sn_buf[0], sn_buf[1], sn_buf[2], sn_buf[3], sn_buf[4],
        sn_buf[5], sn_buf[6], sn_buf[7], sn_buf[8], sn_buf[9], sn_buf[10], sn_buf[11], sn_buf[12], sn_buf[13]
    );

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        ret = ret ?: nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_storage: Failed to set up NVS: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 10,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    ret = esp_vfs_fat_spiflash_mount_rw_wl(DATA_PARTITION_PATH, DATA_PARTITION_LABEL, &mount_cfg, &wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_storage: failed to mount %s: 0x%x %s", DATA_PARTITION_PATH, ret, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "setup_storage: init OK");
    return ret;
}

esp_err_t bootstrap_fsm::setup_usb()
{
    static char lang[2] = {0x09, 0x04};
    static const char *desc_str[5] = {
        lang,                                                        // 0: is supported language is English (0x0409)
        const_cast<char *>(CONFIG_TINYUSB_DESC_MANUFACTURER_STRING), // 1: Manufacturer
        const_cast<char *>(CONFIG_TINYUSB_DESC_PRODUCT_STRING),      // 2: Product
        sn_str,                                                      // 3: Serials, should use chip ID
        const_cast<char *>(CONFIG_TINYUSB_DESC_PRODUCT_STRING),      // 4: CDC Interface (SIDP)
    };

    ESP_LOGI(TAG, "USB CDC initialization");
    // SIDP needs device attach/detach events to notice a replugged host.
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(sidp::cdc_slip_transport::device_event_callback);
    tusb_cfg.task.size = 8192;
    tusb_cfg.descriptor.string = static_cast<const char **>(desc_str);
    tusb_cfg.descriptor.string_count = std::size(desc_str);

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_usb: failed at tinyusb_driver_install: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    return sidp_service::instance()->init(sn_str);
}

void bootstrap_fsm::fsm_task_handler(void *_ctx)
{
    if (_ctx == nullptr) {
        return;
    }

    auto *ctx = static_cast<bootstrap_fsm *>(_ctx);

    ctx->composer->display_init();

    while (true) {
        ctx->run_fsm_task();
    }
}

void bootstrap_fsm::run_fsm_task()
{
    // Both bits are edges: a run request from SIDP, or a target plugged in.
    EventBits_t bits = xEventGroupWaitBits(evt_group, BIT_TARGET_CONNECTED | BIT_RUN_REQUEST, pdTRUE, pdFALSE, portMAX_DELAY);

    // job_controller has already claimed a requested run; a plugged-in
    // target only runs when the job is armed for automatic runs.
    if ((bits & BIT_RUN_REQUEST) == 0 && !job_controller::instance()->begin_auto_run()) {
        ESP_LOGI(TAG, "Target connected, no automatic job armed");
        return;
    }

    run_job();
}

void bootstrap_fsm::run_job()
{
    auto *flasher = offline_flasher::instance();
    int64_t start_us = esp_timer_get_time();

    flasher->init();
    esp_err_t ret = ESP_ERR_NOT_FINISHED;
    while (ret == ESP_ERR_NOT_FINISHED) {
        ret = flasher->handle_states();
        vTaskDelay(1);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Done flashing!");
    } else {
        ESP_LOGE(TAG, "Something went wrong");
    }

    uint32_t duration_ms = (esp_timer_get_time() - start_us) / 1000;
    job_controller::instance()->finish_run(ret, flasher->get_failed_state(), duration_ms);
}

void bootstrap_fsm::det_io_isr_handler(void *_ctx)
{
    auto *timer = (TimerHandle_t)_ctx;
    BaseType_t higher_priority_waken = pdFALSE;
    xTimerStartFromISR(timer, &higher_priority_waken);

    if (higher_priority_waken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void bootstrap_fsm::det_pin_debounce_timer(TimerHandle_t timer_handle)
{
    auto *ctx = static_cast<bootstrap_fsm *>(pvTimerGetTimerID(timer_handle));
    bool state = gpio_get_level(DET_IO_PIN);
    if (state != ctx->last_det_state) {
        ctx->last_det_state = state;
        if (!state) {
            ESP_LOGW(TAG, "Tag connected!");
            xEventGroupSetBits(ctx->evt_group, BIT_TARGET_CONNECTED);
        } else {
            ESP_LOGW(TAG, "Tag DISCONNECTED!");
            // A connect edge not yet picked up must not start a run later.
            xEventGroupClearBits(ctx->evt_group, BIT_TARGET_CONNECTED);
        }
    }
}

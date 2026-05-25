#include <esp_event.h>
#include <nvs_flash.h>
#include "bootstrap_fsm.hpp"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "fw_asset_manager.hpp"
#include "http_downloader.hpp"
#include "offline_flasher.hpp"
#include "driver/i2c_master.h"
#include "tinyusb_msc.h"

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

    ESP_LOGI(TAG, "Setting up storage");
    ret = setup_storage();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up storage: 0x%x %s", ret, esp_err_to_name(ret));
        composer->display_error("ERROR", "Storage partition error\nPlease try factory reset");
        return ret;
    }

    evt_group = xEventGroupCreate();
    if (evt_group == nullptr) {
        ESP_LOGE(TAG, "Can't create event group");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Setting up detection pin");
    gpio_config_t det_io_cfg = {};
    det_io_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    det_io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    det_io_cfg.pin_bit_mask = (1 << DET_IO_PIN);
    det_io_cfg.intr_type = GPIO_INTR_ANYEDGE;
    det_io_cfg.mode = GPIO_MODE_INPUT;

    BaseType_t task_ret = xTaskCreate(fsm_task_handler, "flasher", 8192, this, tskIDLE_PRIORITY + 10, &fsm_task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Can't create FSM task");
        return ESP_FAIL;
    }

    det_debounce_timer = xTimerCreate("target_det", pdMS_TO_TICKS(50), pdFALSE, this, det_pin_debounce_timer);
    if (det_debounce_timer == nullptr) {
        ESP_LOGE(TAG, "Can't create debounce timer");
        return ESP_FAIL;
    }

    ret = gpio_config(&det_io_cfg);
    if (ret == ESP_OK) {
        last_det_state = gpio_get_level(DET_IO_PIN);
        if (last_det_state == 0) {
            ESP_LOGI(TAG, "Target detected at boot time!");
            xEventGroupSetBits(evt_group, BIT_TARGET_CONNECTED);
        } else {
            ESP_LOGI(TAG, "No target detected at boot time.");
            xEventGroupSetBits(evt_group, BIT_TARGET_DISCONNECTED);
        }
    }
    gpio_install_isr_service(0);
    ret = ret ?: gpio_set_intr_type(DET_IO_PIN, GPIO_INTR_ANYEDGE);
    ret = ret ?: gpio_intr_enable(DET_IO_PIN);
    ret = ret ?: gpio_isr_handler_add(DET_IO_PIN, det_io_isr_handler, det_debounce_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button setup failed: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Bootstrap init OK");
    return ESP_OK;
}

esp_err_t bootstrap_fsm::setup_storage()
{
    uint8_t sn_buf[16] = { 0 };
    uint64_t flash_uid = 0;
    esp_efuse_mac_get_default(sn_buf);
    esp_flash_read_unique_chip_id(esp_flash_default_chip, &flash_uid);
    memcpy(sn_buf + 6, &flash_uid, sizeof(uint64_t));
    snprintf(sn_str, sizeof(sn_str), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             sn_buf[0], sn_buf[1], sn_buf[2], sn_buf[3], sn_buf[4], sn_buf[5], sn_buf[6], sn_buf[7],
             sn_buf[8], sn_buf[9], sn_buf[10], sn_buf[11], sn_buf[12], sn_buf[13]);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        ret = ret ?: nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_storage: Failed to set up NVS: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, nullptr);
    if (part == nullptr) {
        ESP_LOGE(TAG, "setup_storage: Failed to find storage partition: 0x%x %s", ret, esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    ret = wl_mount(part, &wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_storage: failed on wl_mount: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium = { .wl_handle = wl_handle },
        .fat_fs = {
            .base_path = const_cast<char *>(DATA_PARTITION_PATH),
            .config = {
                .format_if_mount_failed = true,
                .max_files = 10,
                .allocation_unit_size = 0,
                .disk_status_check_enable = false, .use_one_fat = false
            },
            .do_not_format = false,
            .format_flags = FM_ANY,
        },

        // Expected logic:
        // 1. when device starts from power-on reset, it exposes data partition to USB;
        // 2. when a target is connected, it takes back the partition to itself (the app).
        // 3. Here we expose to app first to let the ESP FAT library to automatically detect the partition and see whether a format is needed or not.
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };

    ret = tinyusb_msc_new_storage_spiflash(&storage_cfg, &tusb_msc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_storage: failed on TinyUSB mount: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    static char lang[2] = {0x09, 0x04};
    static const char *desc_str[6] = {
        lang,                // 0: is supported language is English (0x0409)
        const_cast<char *>(CONFIG_TINYUSB_DESC_MANUFACTURER_STRING), // 1: Manufacturer
        const_cast<char *>(CONFIG_TINYUSB_DESC_PRODUCT_STRING),      // 2: Product
        sn_str,             // 3: Serials, should use chip ID
        const_cast<char *>(CONFIG_TINYUSB_DESC_PRODUCT_STRING),      // 4: CDC Interface
        const_cast<char *>(CONFIG_TINYUSB_DESC_MSC_STRING),          // 5: MSC Interface
};

    ESP_LOGI(TAG, "USB Composite initialization");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.task.size = 8192;
    tusb_cfg.descriptor.string = static_cast<const char **>(desc_str);
    tusb_cfg.descriptor.string_count = std::size(desc_str);

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_storage: failed at tinyusb_driver_install: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = nullptr,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = nullptr,
        .callback_line_coding_changed = nullptr
    };
    ret = tinyusb_cdcacm_init(&acm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_storage: failed at tinyusb_cdcacm_init: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    // Set back to expose to MSC
    ret = tinyusb_msc_set_storage_mount_point(tusb_msc_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_storage: can't expose to MSC: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "setup_storage: init OK");
    return ret;
}

void bootstrap_fsm::fsm_task_handler(void *_ctx)
{
    if (_ctx == nullptr) {
        return;
    }

    auto *ctx = (bootstrap_fsm *)_ctx;

    ctx->composer->display_init();

    // Now we wait till the first target is connected
    xEventGroupWaitBits(ctx->evt_group, BIT_TARGET_CONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);

    // After target is connected, mount the data partition to application instead of USB MSC.
    tinyusb_msc_set_storage_mount_point(ctx->tusb_msc_handle, TINYUSB_MSC_STORAGE_MOUNT_APP);
    offline_flasher::instance()->init();

    while (true) {
        ctx->run_fsm_task();
        vTaskDelay(1);
    }
}

void bootstrap_fsm::run_fsm_task()
{
    auto *flasher = offline_flasher::instance();
    auto ret = flasher->handle_states();
    if (ret == ESP_ERR_NOT_FINISHED) {
        return; // Continue...
    }

    if (ret == ESP_FAIL) {
        ESP_LOGE(TAG, "Something went wrong");
    } else {
        ESP_LOGI(TAG, "Done flashing!");
    }

    xEventGroupWaitBits(evt_group, BIT_TARGET_DISCONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);
    xEventGroupWaitBits(evt_group, BIT_TARGET_CONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);
    flasher->init();
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
    auto *ctx = (bootstrap_fsm *) pvTimerGetTimerID(timer_handle);
    bool state = gpio_get_level(DET_IO_PIN);
    if (state != ctx->last_det_state) {
        ctx->last_det_state = state;
        if (!state) {
            ESP_LOGW(TAG, "Tag connected!");
            xEventGroupSetBits(ctx->evt_group, BIT_TARGET_CONNECTED);
            xEventGroupClearBits(ctx->evt_group, BIT_TARGET_DISCONNECTED);
        } else {
            ESP_LOGW(TAG, "Tag DISCONNECTED!");
            xEventGroupSetBits(ctx->evt_group, BIT_TARGET_DISCONNECTED);
            xEventGroupClearBits(ctx->evt_group, BIT_TARGET_CONNECTED);
        }
    }
}


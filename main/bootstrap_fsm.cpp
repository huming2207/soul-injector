#include <esp_event.h>
#include <nvs_flash.h>
#include "bootstrap_fsm.hpp"
#include "fw_asset_manager.hpp"
#include "http_downloader.hpp"
#include "offline_flasher.hpp"
#include "tcfg_client.hpp"
#include "tcfg_wire_usb_cdc.hpp"
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

    ESP_LOGI(TAG, "Setting up storage");
    ret = setup_storage();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up storage: 0x%x %s", ret, esp_err_to_name(ret));
        composer->display_error("ERROR", "Storage partition error\nPlease try factory reset");
        return ret;
    }

    ESP_LOGI(TAG, "Set up provisioning");
    auto *prov_client = tcfg_client::instance();
    auto *usb_cdc = tcfg_wire_usb_cdc::instance();
    ret = usb_cdc->init();
    ret = ret ?: prov_client->init(usb_cdc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up TCFG provisioning: 0x%x %s", ret, esp_err_to_name(ret));
        composer->display_error("ERROR", "TCFG setup failed\nCheck USB");
        return ret;
    }

    ESP_LOGI(TAG, "Loading asset");
    auto *asset = fw_asset_manager::instance();
    ret = asset->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load assets: 0x%x %s", ret, esp_err_to_name(ret));
        composer->display_error("ERROR", "No firmware asset\nPlease load firmware on me!");
        return ret;
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

    evt_group = xEventGroupCreate();
    if (evt_group == nullptr) {
        ESP_LOGE(TAG, "Can't create event group");
        return ESP_FAIL;
    }

#ifdef CONFIG_SI_SG_PROG_RIG
    ESP_LOGI(TAG, "Set up SG stuff");
    i2c_master_bus_config_t master_cfg = {};
    master_cfg.clk_source = I2C_CLK_SRC_XTAL;
    master_cfg.sda_io_num = (gpio_num_t)CONFIG_SI_SG_I2C_SDA;
    master_cfg.scl_io_num = (gpio_num_t)CONFIG_SI_SG_I2C_SCL;
    master_cfg.i2c_port = (i2c_port_t)CONFIG_SI_SG_I2C_PERIPH;
    master_cfg.glitch_ignore_cnt = 7;

    ESP_LOGI(TAG, "Setting up I2C & ADC");

    ret = i2c_new_master_bus(&master_cfg, &i2c_bus);

    return ret;
#endif

    ret = gpio_config(&det_io_cfg);
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
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        ret = ret ?: nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up NVS: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_vfs_littlefs_register(&lfs_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up storage: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    return ret;
}

void bootstrap_fsm::start_offline_flasher()
{
    offline_flasher::instance()->init();
}

void bootstrap_fsm::fsm_task_handler(void *_ctx)
{
    if (_ctx == nullptr) {
        return;
    }

    auto *ctx = (bootstrap_fsm *)_ctx;

    ctx->start_offline_flasher();

    while (true) {
        ctx->run_fsm_task();
        vTaskDelay(1);
    }
}

void bootstrap_fsm::run_fsm_task()
{
    auto *flasher = offline_flasher::instance();
    auto ret = flasher->handle_states();
    if (ret == ESP_FAIL) {
        ESP_LOGE(TAG, "Something went wrong");
        xEventGroupWaitBits(evt_group, BIT_TARGET_DISCONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);
        xEventGroupWaitBits(evt_group, BIT_TARGET_CONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);
        flasher->init();
    } else if (ret == ESP_ERR_NOT_FINISHED) {
        return; // Continue...
    } else {
        ESP_LOGI(TAG, "Done flashing!");
        xEventGroupWaitBits(evt_group, BIT_TARGET_DISCONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);
        flasher->init();
    }
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
    if (state == ctx->last_det_state) {
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

    ctx->last_det_state = state;
}

void bootstrap_fsm::got_wifi_ip_handler(esp_netif_ip_info_t *)
{

}

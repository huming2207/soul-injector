#include <cstring>
#include <esp_log.h>
#include "lcd/display_manager.hpp"
#include "fw_asset_manager.hpp"
#include "bootstrap_fsm.hpp"
#include <esp_pm.h>
#include <psa/crypto.h>

#include "esp_sleep.h"

extern "C" void app_main(void)
{
    static const char *TAG = "main";
    ESP_LOGI(TAG, "Started");

    psa_crypto_init();

    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 320,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_pm_configure(&pm_cfg);

    auto *main_fsm = bootstrap_fsm::instance();
    main_fsm->init();

    vTaskDelay(portMAX_DELAY);
}

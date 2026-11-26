#include <cstring>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <si_build_info.h>
#include "lcd/display_manager.hpp"
#include "fw_asset_manager.hpp"
#include "bootstrap_fsm.hpp"
#include <esp_pm.h>
#include <psa/crypto.h>

#include "esp_sleep.h"

extern "C" void app_main(void)
{
    static const char *TAG = "main";
    ESP_LOGI(TAG, "Started: firmware %s (%s%s), hardware Rev %s", esp_app_get_description()->version,
             SI_FW_COMMIT[0] ? SI_FW_COMMIT : "commit unknown", SI_FW_MODIFIED ? ", modified" : "", CONFIG_SI_DEVICE_BUILD);

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

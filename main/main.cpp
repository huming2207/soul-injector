#include <cstring>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <si_build_info.h>
#include "lcd/display_manager.hpp"
#include "fw_asset_manager.hpp"
#include "bootstrap_fsm.hpp"
#include <esp_pm.h>
#include <psa/crypto.h>
#include <nvs.h>
#include <nvs_flash.h>
#include "target_lines.hpp"

#include "esp_sleep.h"

extern "C" void app_main(void)
{
    static const char *TAG = "main";
    ESP_LOGI(TAG, "Started: firmware %s (%s%s), hardware Rev %s", esp_app_get_description()->version,
             SI_FW_COMMIT[0] ? SI_FW_COMMIT : "commit unknown", SI_FW_MODIFIED ? ", modified" : "", CONFIG_SI_DEVICE_BUILD);

    // A trap armed with --connect-under-reset wants the target to start under
    // watch: hold it in reset now, the earliest the firmware can, until the
    // trap attaches (agent_service). Before this the target runs freely.
#if CONFIG_SI_JS_ENABLE // traps need scripts; a flag left by another firmware means nothing here
    if (nvs_flash_init() == ESP_OK) {
        nvs_handle_t nvs;
        uint8_t hold = 0;
        if (nvs_open("si_agent", NVS_READONLY, &nvs) == ESP_OK) {
            nvs_get_u8(nvs, "trap_hold", &hold);
            nvs_close(nvs);
        }
        if (hold) {
            target_lines::hold_reset_from_boot();
            ESP_LOGI(TAG, "Holding the target in reset for the armed trap");
        }
    }
#endif

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

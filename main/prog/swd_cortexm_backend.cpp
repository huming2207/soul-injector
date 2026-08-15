#include "swd_cortexm_backend.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <swd_host.h>

#include "fw_asset_manager.hpp"
#include "swd_prog.hpp"

static const char *TAG = "swd_backend";

esp_err_t swd_cortexm_backend::begin_session()
{
    // Same bring-up sequence the old pre/post program handlers used.
    swd_off();
    vTaskDelay(1);
    swd_init();
    vTaskDelay(1);
    swd_trigger_nrst();
    vTaskDelay(1);
    return swd_init_debug() < 1 ? ESP_FAIL : ESP_OK;
}

esp_err_t swd_cortexm_backend::detect()
{
    return swd_prog::instance()->init();
}

esp_err_t swd_cortexm_backend::erase()
{
    const si::config::flash_algorithm &fa = fw_asset_manager::instance()->config().algo;

    if (!fa.flash_start.has_value() || !fa.flash_end.has_value()) {
        ESP_LOGE(TAG, "erase: flash address range not configured");
        return ESP_ERR_INVALID_STATE;
    }

    auto ret = swd_prog::instance()->erase_chip();
    if (ret != ESP_OK) {
        ret = swd_prog::instance()->erase_sector(fa.flash_start.value(), fa.flash_end.value());
    }
    return ret;
}

esp_err_t swd_cortexm_backend::program(uint32_t *written_len)
{
    uint32_t written = 0;
    auto ret = swd_prog::instance()->program_file(fw_asset_manager::FIRMWARE_PATH, &written);
    if (written_len != nullptr) {
        *written_len = written;
    }
    return ret;
}

esp_err_t swd_cortexm_backend::verify(uint32_t written_len)
{
    return swd_prog::instance()->verify(fw_asset_manager::FIRMWARE_PATH, UINT32_MAX, written_len);
}

esp_err_t swd_cortexm_backend::self_test(const si::config::test_item &item, uint32_t *func_return_val)
{
    return swd_prog::instance()->self_test(item.addr, nullptr, 0, func_return_val);
}

esp_err_t swd_cortexm_backend::release_transport()
{
    swd_prog::reset_gpio(); // tri-state the SWD pins so the target can run
    return ESP_OK;
}

esp_err_t swd_cortexm_backend::reset_target()
{
    swd_prog::trigger_nrst();
    return ESP_OK;
}

esp_err_t swd_cortexm_backend::reinit_debug()
{
    swd_off();
    vTaskDelay(1);
    return swd_init_debug() < 1 ? ESP_FAIL : ESP_OK;
}

esp_err_t swd_cortexm_backend::halt_target()
{
    return swd_halt_target() < 1 ? ESP_FAIL : ESP_OK;
}

esp_err_t swd_cortexm_backend::wait_halt()
{
    return swd_wait_until_halted() < 1 ? ESP_FAIL : ESP_OK;
}

esp_err_t swd_cortexm_backend::read_mem32(uint32_t addr, uint32_t *val)
{
    return swd_read_word(addr, val) < 1 ? ESP_FAIL : ESP_OK;
}

esp_err_t swd_cortexm_backend::write_mem32(uint32_t addr, uint32_t val)
{
    return swd_write_word(addr, val) < 1 ? ESP_FAIL : ESP_OK;
}

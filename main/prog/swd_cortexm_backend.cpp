#include "swd_cortexm_backend.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <swd_host.h>

#include "fw_asset_manager.hpp"
#include "swd_prog.hpp"
#include "target_lines.hpp"

static const char *TAG = "swd_backend";
static constexpr uint32_t DHCSR_ADDR = 0xE000EDF0;
static constexpr uint32_t DFSR_ADDR = 0xE000ED30;
static constexpr uint32_t DEMCR_ADDR = 0xE000EDFC;
static constexpr uint32_t DEMCR_VC_ALL = 0x7F1;  // VC_CORERESET and the fault catches; TRCENA stays
static constexpr uint32_t DFSR_CLEAR_ALL = 0x1F;
static constexpr uint8_t DP_CTRL_STAT_ADDR = 0x04;
static constexpr uint32_t END_PULSE_MS = 20;     // nRST low
static constexpr uint32_t END_CATCH_MS = 500;    // for the core to stop at its reset vector

static constexpr uint32_t MVFR0_ADDR = 0xE000EF40;
static constexpr uint32_t REGSEL_FPSCR = 0x21;
static constexpr uint32_t REGSEL_S0 = 0x40;
static constexpr uint32_t REGSEL_S31 = 0x5F;

static esp_err_t check_halted()
{
    uint32_t dhcsr = 0;
    esp_err_t ret = swd_read_word(DHCSR_ADDR, &dhcsr);
    return ret != ESP_OK ? ret : (dhcsr & S_HALT) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

// A core register access: halted, and an FP register (FPSCR, S0-S31) only on a
// core with a single precision FPU. Without one DCRSR still answers, with a
// value that means nothing. MVFR0 reads as 0 (or faults, on ARMv6-M) then.
static esp_err_t check_core_reg(uint32_t id)
{
    esp_err_t ret = check_halted();
    if (ret == ESP_OK && (id == REGSEL_FPSCR || (id >= REGSEL_S0 && id <= REGSEL_S31))) {
        uint32_t mvfr0 = 0;
        if (swd_read_word(MVFR0_ADDR, &mvfr0) != ESP_OK) {
            (void)swd_clear_errors(); // ARMv6-M: no MVFR0
            mvfr0 = 0;
        }
        ret = (mvfr0 & 0xF0) != 0 ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    return ret;
}

esp_err_t swd_cortexm_backend::begin_session()
{
    // Same bring-up sequence the old pre/post program handlers used.
    swd_off();
    vTaskDelay(1);
    auto init_err = swd_init(pdMS_TO_TICKS(1000));
    if (init_err != ESP_OK) {
        return init_err;
    }
    vTaskDelay(1);
    swd_trigger_nrst();
    vTaskDelay(1);

    connected_under_reset = false;
    auto ret = swd_init_debug(pdMS_TO_TICKS(1000));
    if (ret == ESP_OK) {
        return ESP_OK;
    }

    // Firmware that turns SWD off or sleeps at boot wins that race every time.
    ESP_LOGW(TAG, "begin_session: failed with 0x%x, retrying under reset", ret);
    ret = swd_connect_under_reset(pdMS_TO_TICKS(1000));
    connected_under_reset = ret == ESP_OK;
    return ret;
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
    swd_prog::instance()->on_progress = on_progress;
    auto ret = swd_prog::instance()->program_file(fw_asset_manager::FIRMWARE_PATH, &written);
    swd_prog::instance()->on_progress = nullptr;
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
    if (connected_under_reset) {
        // The session left the target halted at its reset vector.
        (void)swd_run_target();
        connected_under_reset = false;
    }
    swd_prog::reset_gpio(); // tri-state the SWD pins so the target can run
    return ESP_OK;
}

esp_err_t swd_cortexm_backend::end_run(run_end how)
{
    if (how == run_end::as_is) {
        return release_transport();
    }

    // Free the core from the debugger: a vector catch left set halts it at the
    // next reset or fault, and with C_DEBUGEN set a BKPT halts it for good.
    esp_err_t ret = free_core(how == run_end::reset);
    if (ret != ESP_OK) {
        // Retried once on a fresh link: the run may have left the debug port in error.
        ESP_LOGW(TAG, "end_run: %s (0x%x), once more after reconnecting", esp_err_to_name(ret), ret);
        ret = reinit_debug();
        ret = ret ?: free_core(how == run_end::reset);
    }
    if (ret == ESP_OK) {
        // Drop the debug power-up requests, or some parts keep their debug domain on in sleep.
        (void)swd_write_dp(DP_CTRL_STAT_ADDR, 0);
        ESP_LOGI(TAG, "end_run: target %s, debug off", how == run_end::reset ? "reset" : "resumed");
    } else {
        ESP_LOGE(TAG, "end_run: couldn't free the target: %s (0x%x)", esp_err_to_name(ret), ret);
    }
    connected_under_reset = false;
    swd_prog::reset_gpio();
    return ret;
}

esp_err_t swd_cortexm_backend::free_core(bool reset)
{
    uint32_t demcr = 0;
    esp_err_t ret = swd_read_word(DEMCR_ADDR, &demcr);
    if (reset) {
        // Stop at the reset vector, so no firmware runs with debug still enabled.
        ret = ret ?: swd_write_word(DHCSR_ADDR, DBGKEY | C_DEBUGEN | C_HALT);
        ret = ret ?: swd_write_word(DEMCR_ADDR, (demcr & ~DEMCR_VC_ALL) | VC_CORERESET);
        ret = ret ?: swd_write_word(DFSR_ADDR, DFSR_CLEAR_ALL);
        if (ret != ESP_OK) {
            return ret;
        }
        target_lines::drive_nrst(true);
        vTaskDelay(pdMS_TO_TICKS(END_PULSE_MS));
        target_lines::drive_nrst(false);

        bool caught = false;
        const TickType_t start = xTaskGetTickCount();
        while (!caught && xTaskGetTickCount() - start < pdMS_TO_TICKS(END_CATCH_MS)) {
            vTaskDelay(1);
            uint32_t dhcsr = 0;
            if (swd_read_word(DHCSR_ADDR, &dhcsr) != ESP_OK) {
                swd_clear_errors(); // the debug port can report errors during the reset
                continue;
            }
            caught = (dhcsr & S_HALT) != 0;
        }
        if (!caught) {
            // Not fatal: debug is turned off below either way.
            ESP_LOGW(TAG, "end_run: the reset wasn't caught at its vector; the firmware ran briefly with debug on");
        }
    } else {
        uint32_t dhcsr = 0;
        ret = ret ?: swd_read_word(DHCSR_ADDR, &dhcsr);
        if (ret == ESP_OK && (dhcsr & S_HALT) != 0) {
            // The interrupt mask and single step can only be cleared while halted.
            ret = swd_write_word(DHCSR_ADDR, DBGKEY | C_DEBUGEN | C_HALT);
        }
    }
    ret = ret ?: swd_write_word(DEMCR_ADDR, demcr & ~DEMCR_VC_ALL);
    ret = ret ?: swd_write_word(DFSR_ADDR, DFSR_CLEAR_ALL);
    // C_DEBUGEN off: a halted core runs, and BKPTs become faults the firmware handles.
    return ret ?: swd_write_word(DHCSR_ADDR, DBGKEY);
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
    return swd_init_debug(pdMS_TO_TICKS(1000));
}

esp_err_t swd_cortexm_backend::halt_target()
{
    return swd_halt_target();
}

esp_err_t swd_cortexm_backend::wait_halt()
{
    return swd_wait_until_halted();
}

esp_err_t swd_cortexm_backend::read_mem32(uint32_t addr, uint32_t *val)
{
    return swd_read_word(addr, val);
}

esp_err_t swd_cortexm_backend::write_mem32(uint32_t addr, uint32_t val)
{
    return swd_write_word(addr, val);
}

esp_err_t swd_cortexm_backend::read_mem(uint32_t addr, uint8_t *buf, size_t len)
{
    return swd_read_memory(addr, buf, len);
}

esp_err_t swd_cortexm_backend::write_mem(uint32_t addr, const uint8_t *buf, size_t len)
{
    return swd_write_memory(addr, const_cast<uint8_t *>(buf), len);
}

esp_err_t swd_cortexm_backend::flash_read(uint32_t addr, uint8_t *buf, size_t len)
{
    return swd_read_memory(addr, buf, len); // flash is memory mapped
}

esp_err_t swd_cortexm_backend::flash_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    return swd_prog::instance()->write_range(addr, buf, len);
}

esp_err_t swd_cortexm_backend::flash_erase(uint32_t addr, size_t len)
{
    return swd_prog::instance()->erase_range(addr, len);
}

esp_err_t swd_cortexm_backend::flash_check(flash_op op, uint32_t addr, size_t len, char *why, size_t why_len)
{
    auto access = op == flash_op::read    ? swd_prog::flash_access::read
                  : op == flash_op::write ? swd_prog::flash_access::write
                                          : swd_prog::flash_access::erase;
    return swd_prog::check_flash_args(access, addr, len, why, why_len);
}

esp_err_t swd_cortexm_backend::call(uint32_t addr, const uint32_t args[4], uint32_t stack_top, uint32_t timeout_ms,
                                    bool (*cancelled)(), uint32_t *r0)
{
    return swd_prog::instance()->call(addr, args, stack_top, timeout_ms, cancelled, r0);
}

esp_err_t swd_cortexm_backend::resume_target()
{
    return swd_write_word(DHCSR_ADDR, DBGKEY | C_DEBUGEN); // debug stays on, unlike swd_run_target()
}

esp_err_t swd_cortexm_backend::read_core_reg(uint32_t id, uint32_t *val)
{
    esp_err_t ret = check_core_reg(id);
    return ret != ESP_OK ? ret : swd_read_core_register(id, val);
}

esp_err_t swd_cortexm_backend::write_core_reg(uint32_t id, uint32_t val)
{
    esp_err_t ret = check_core_reg(id);
    return ret != ESP_OK ? ret : swd_write_core_register(id, val);
}

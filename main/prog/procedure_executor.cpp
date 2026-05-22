#include <cstdint>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "procedure_executor.hpp"
#include "esp_err.h"
#include "swd_host.h"

procedure_executor::procedure_executor(std::vector<procedure_executor::step> const &_steps) : steps(_steps)
{
}

esp_err_t procedure_executor::load_yaml(const char *path)
{
    return ESP_OK;
}

esp_err_t procedure_executor::exec_rw32(procedure_executor::step *curr_step)
{
    if (curr_step == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (curr_step->type == READ_32) {
        return swd_read_word(curr_step->op.rw32.addr, &curr_step->op.rw32.data) < 1 ? ESP_FAIL : ESP_OK;
    } else if (curr_step->type == WRITE_32) {
        return swd_write_word(curr_step->op.rw32.addr, curr_step->op.rw32.data) < 1 ? ESP_FAIL : ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t procedure_executor::exec_rwblob(procedure_executor::step *curr_step)
{
    if (curr_step == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (curr_step->type == READ_BLOB) {
        return swd_read_memory(curr_step->op.rwblob.addr, curr_step->op.rwblob.buf, curr_step->op.rwblob.buf_len) < 1 ? ESP_FAIL : ESP_OK;
    } else if (curr_step->type == WRITE_BLOB) {
        return swd_write_memory(curr_step->op.rwblob.addr, curr_step->op.rwblob.buf, curr_step->op.rwblob.buf_len) < 1 ? ESP_FAIL : ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t procedure_executor::exec_rmw32(procedure_executor::step *curr_step)
{
    if (curr_step == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t val = 0;
    if (swd_read_word(curr_step->op.rmw32.addr, &val) < 1) {
        return ESP_FAIL;
    }

    val = (val & curr_step->op.rmw32.mask) | curr_step->op.rmw32.data;

    return swd_write_word(curr_step->op.rmw32.addr, val) < 1 ? ESP_FAIL : ESP_OK;
}

esp_err_t procedure_executor::exec_poll32(procedure_executor::step *curr_step)
{
    if (curr_step == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t timeout = curr_step->op.poll32.timeout_ms;
    do {
        uint32_t val = 0;
        if (swd_read_word(curr_step->op.poll32.addr, &val) < 1) {
            return ESP_FAIL;
        }

        if ((val & curr_step->op.poll32.mask) == curr_step->op.poll32.expected) {
            return ESP_OK;
        }

        if (timeout > 0) {
            vTaskDelay(1);
            timeout -= portTICK_PERIOD_MS;
        }
    } while (timeout > 0);

    return ESP_ERR_TIMEOUT;
}

esp_err_t procedure_executor::exec_delay_ms(procedure_executor::step *curr_step)
{
    if (curr_step == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t delay = curr_step->op.delay_ms.delay_ms;
    if (delay < portTICK_PERIOD_MS) {
        esp_rom_delay_us(delay * 1000);
    } else {
        vTaskDelay(pdMS_TO_TICKS(delay));
    }

    return ESP_OK;
}

esp_err_t procedure_executor::exec_swd_reinit(procedure_executor::step *curr_step)
{
    swd_off();
    vTaskDelay(1);

    return swd_init_debug() < 1 ? ESP_FAIL : ESP_OK;
}

esp_err_t procedure_executor::exec_swd_reset(procedure_executor::step *curr_step)
{
    swd_trigger_nrst();
    return ESP_OK;
}

esp_err_t procedure_executor::exec_swd_halt(procedure_executor::step *curr_step)
{
    return swd_halt_target() < 1 ? ESP_FAIL : ESP_OK;
}

esp_err_t procedure_executor::exec_swd_wait_halt(procedure_executor::step *curr_step)
{
    return swd_wait_until_halted() < 1 ? ESP_FAIL : ESP_OK;
}

esp_err_t procedure_executor::execute()
{
    if (steps.empty()) {
        ESP_LOGW(TAG, "exec: nothing to execute");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    for (auto &item : steps) {
        switch (item.type) {
            case READ_32: {
                ret = ret ?: exec_rw32(&item);
                break;
            }

            case WRITE_32: {
                ret = ret ?: exec_rw32(&item);
                break;
            }

            case READ_BLOB: {
                ret = ret ?: exec_rwblob(&item);
                break;
            }

            case WRITE_BLOB: {
                ret = ret ?: exec_rwblob(&item);
                break;
            }

            case READ_MOD_WRITE_32: {
                ret = ret ?: exec_rmw32(&item);
                break;
            }

            case SWD_REINIT: {
                ret = ret ?: exec_swd_reinit(&item);
                break;
            }

            case DELAY_MS: {
                ret = ret ?: exec_delay_ms(&item);
                break;
            }

            case SWD_RESET_TARGET: {
                ret = ret ?: exec_swd_reset(&item);
                break;
            }

            case SWD_HALT_TARGET: {
                ret = ret ?: exec_swd_halt(&item);
                break;
            }

            case SWD_WAIT_HALT: {
                ret = ret ?: exec_swd_wait_halt(&item);
                break;
            }

            case POLL_32: {
                ret = ret ?: exec_poll32(&item);
                break;
            }
        }
    }

    return ret;
}

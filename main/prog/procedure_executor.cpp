#include "procedure_executor.hpp"

#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

esp_err_t procedure_executor::exec_read32(const si_job_Read32 &op, target_backend &backend)
{
    uint32_t val = 0;
    auto ret = backend.read_mem32(op.addr, &val);
    ESP_LOGI(TAG, "exec: r32: 0x%08lx @ 0x%08lx", val, op.addr);
    return ret;
}

esp_err_t procedure_executor::exec_read_mod_write32(const si_job_ReadModWrite32 &op, target_backend &backend)
{
    ESP_LOGI(TAG, "exec: rmw32 0x%08lx mask 0x%08lx @ 0x%08lx", op.data, op.mask, op.addr);
    uint32_t val = 0;
    auto ret = backend.read_mem32(op.addr, &val);
    if (ret != ESP_OK) {
        return ret;
    }
    val = (val & op.mask) | op.data;
    return backend.write_mem32(op.addr, val);
}

esp_err_t procedure_executor::exec_poll32(const si_job_Poll32 &op, target_backend &backend)
{
    ESP_LOGI(TAG, "exec: poll32 0x%08lx mask 0x%08lx @ 0x%08lx, timeout %lums", op.expected, op.mask, op.addr, op.timeout_ms);
    int64_t deadline_us = esp_timer_get_time() + (int64_t)op.timeout_ms * 1000;
    while (true) {
        uint32_t val = 0;
        auto ret = backend.read_mem32(op.addr, &val);
        if (ret != ESP_OK) {
            return ret;
        }
        if ((val & op.mask) == op.expected) {
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

esp_err_t procedure_executor::exec_delay(const si_job_Delay &op)
{
    ESP_LOGI(TAG, "exec: delay_ms %lu", op.delay_ms);
    if (op.delay_ms < portTICK_PERIOD_MS) {
        esp_rom_delay_us(op.delay_ms * 1000);
    } else {
        vTaskDelay(pdMS_TO_TICKS(op.delay_ms));
    }
    return ESP_OK;
}

esp_err_t procedure_executor::exec_one(const si_job_Step &step, target_backend &backend)
{
    switch (step.which_op) {
    case si_job_Step_read32_tag:
        return exec_read32(step.op.read32, backend);
    case si_job_Step_write32_tag:
        ESP_LOGI(TAG, "exec: w32: 0x%08lx @ 0x%08lx", step.op.write32.data, step.op.write32.addr);
        return backend.write_mem32(step.op.write32.addr, step.op.write32.data);
    case si_job_Step_read_mod_write32_tag:
        return exec_read_mod_write32(step.op.read_mod_write32, backend);
    case si_job_Step_poll32_tag:
        return exec_poll32(step.op.poll32, backend);
    case si_job_Step_delay_tag:
        return exec_delay(step.op.delay);
    case si_job_Step_swd_reinit_tag:
        ESP_LOGI(TAG, "exec: reinit debug connection");
        return backend.reinit_debug();
    case si_job_Step_reset_target_tag:
        ESP_LOGI(TAG, "exec: reset target");
        return backend.reset_target();
    case si_job_Step_halt_target_tag:
        ESP_LOGI(TAG, "exec: halt target");
        return backend.halt_target();
    case si_job_Step_wait_halt_tag:
        ESP_LOGI(TAG, "exec: wait halt");
        return backend.wait_halt();
    default:
        ESP_LOGE(TAG, "exec: unknown step operation %u", (unsigned)step.which_op);
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t procedure_executor::execute(const si_job_Procedure &procedure, target_backend &backend)
{
    for (pb_size_t i = 0; i < procedure.steps_count; i++) {
        const si_job_Step &step = procedure.steps[i];
        auto ret = exec_one(step, backend);
        if (ret == ESP_OK) {
            continue;
        }

        if (!step.ignore_error) {
            ESP_LOGE(TAG, "exec: step %u/%u failed (0x%x), aborting", (unsigned)i + 1, (unsigned)procedure.steps_count, ret);
            return ret;
        }
        ESP_LOGW(TAG, "exec: step %u/%u failed (0x%x) but ignore_error is set, continuing", (unsigned)i + 1, (unsigned)procedure.steps_count, ret);
    }

    return ESP_OK;
}

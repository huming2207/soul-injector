#include "procedure_executor.hpp"

#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

esp_err_t procedure_executor::load_yaml(const char *path)
{
    if (path == nullptr) {
        ESP_LOGE(TAG, "load_yaml: path is null");
        return ESP_ERR_INVALID_ARG;
    }

    yaml_doc doc;
    auto ret = yaml_doc::load(path, doc);
    if (ret != ESP_OK) {
        return ret;
    }

    ryml::ConstNodeRef root = doc.root();
    if (!root.has_child("steps")) {
        ESP_LOGE(TAG, "load_yaml: root does not contain 'steps'");
        return ESP_ERR_INVALID_STATE;
    }

    ryml::ConstNodeRef steps_node = root["steps"];
    if (!steps_node.is_seq()) {
        ESP_LOGE(TAG, "load_yaml: 'steps' is not a sequence");
        return ESP_ERR_INVALID_STATE;
    }

    size_t count = steps_node.num_children();
    if (count == 0) {
        ESP_LOGW(TAG, "load_yaml: 'steps' is empty");
        clear();
        return ESP_OK;
    }
    if (count > MAX_STEPS) {
        ESP_LOGE(TAG, "load_yaml: %zu steps exceeds capacity %zu", count, MAX_STEPS);
        return ESP_ERR_INVALID_SIZE;
    }

    // Parse into a scratch array so a failure halfway through leaves the
    // previously loaded steps intact. No value-initialiser: clangd rejects
    // default-constructing a std::variant array; entries are only ever read
    // up to the committed step_count.
    step parsed[MAX_STEPS];
    for (size_t i = 0; i < count; i++) {
        ret = parse_step(steps_node[i], i, parsed[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "load_yaml: step %zu failed to parse", i);
            return ret;
        }
    }

    memcpy(steps, parsed, sizeof(step) * count);
    step_count = count;
    ESP_LOGI(TAG, "load_yaml: %zu steps loaded from %s", step_count, path);
    return ESP_OK;
}

esp_err_t procedure_executor::parse_step(ryml::ConstNodeRef node, size_t idx, step &out)
{
    if (!node.is_map()) {
        ESP_LOGE(TAG, "step %zu is not a map", idx);
        return ESP_ERR_INVALID_STATE;
    }
    if (!node.has_child("type")) {
        ESP_LOGE(TAG, "step %zu missing 'type'", idx);
        return ESP_ERR_INVALID_STATE;
    }

    ryml::csubstr type_val = node["type"].val();

    bool ignore_error = false;
    std::optional<bool> ignore;
    auto ret = yaml_doc::get_opt_bool(node, "ignore_error", ignore);
    if (ret != ESP_OK) {
        return ret;
    }
    ignore_error = ignore.value_or(false);

    char ctx[48];
    auto step_ctx = [&]() {
        snprintf(ctx, sizeof(ctx), "step %zu ('%.*s')", idx, (int)type_val.len, type_val.str);
        return ctx;
    };

    if (type_val == "READ_32") {
        read32_step s = {};
        s.ignore_error = ignore_error;
        ret = yaml_doc::get_u32(node, "addr", &s.addr);
        if (ret != ESP_OK)
            return ret;
        out = s;
        return ESP_OK;
    }

    if (type_val == "WRITE_32") {
        write32_step s = {};
        s.ignore_error = ignore_error;
        ret = yaml_doc::get_u32(node, "addr", &s.addr);
        ret = ret ?: yaml_doc::get_u32(node, "data", &s.data);
        if (ret != ESP_OK)
            return ret;
        out = s;
        return ESP_OK;
    }

    if (type_val == "READ_MOD_WRITE_32") {
        read_mod_write32_step s = {};
        s.ignore_error = ignore_error;
        ret = yaml_doc::get_u32(node, "addr", &s.addr);
        ret = ret ?: yaml_doc::get_u32(node, "mask", &s.mask);
        ret = ret ?: yaml_doc::get_u32(node, "data", &s.data);
        if (ret != ESP_OK)
            return ret;
        out = s;
        return ESP_OK;
    }

    if (type_val == "POLL_32") {
        poll32_step s = {};
        s.ignore_error = ignore_error;
        ret = yaml_doc::get_u32(node, "addr", &s.addr);
        ret = ret ?: yaml_doc::get_u32(node, "mask", &s.mask);
        ret = ret ?: yaml_doc::get_u32(node, "expected", &s.expected);
        ret = ret ?: yaml_doc::get_u32(node, "timeout_ms", &s.timeout_ms);
        if (ret != ESP_OK)
            return ret;
        out = s;
        return ESP_OK;
    }

    if (type_val == "DELAY_MS") {
        delay_ms_step s = {};
        s.ignore_error = ignore_error;
        ret = yaml_doc::get_u32(node, "delay_ms", &s.delay_ms);
        if (ret != ESP_OK)
            return ret;
        out = s;
        return ESP_OK;
    }

    if (type_val == "SWD_REINIT") {
        swd_reinit_step s = {};
        s.ignore_error = ignore_error;
        out = s;
        return ESP_OK;
    }

    if (type_val == "SWD_RESET_TARGET") {
        reset_target_step s = {};
        s.ignore_error = ignore_error;
        out = s;
        return ESP_OK;
    }

    if (type_val == "SWD_HALT_TARGET") {
        halt_target_step s = {};
        s.ignore_error = ignore_error;
        out = s;
        return ESP_OK;
    }

    if (type_val == "SWD_WAIT_HALT") {
        wait_halt_step s = {};
        s.ignore_error = ignore_error;
        out = s;
        return ESP_OK;
    }

    // Legacy no-op types: keep old files parsing, do nothing at run time.
    if (type_val == "READ_BLOB" || type_val == "WRITE_BLOB") {
        ESP_LOGW(TAG, "step %zu: type '%.*s' is not supported, skipping", idx, (int)type_val.len, type_val.str);
        delay_ms_step s = {};
        s.ignore_error = true;
        s.delay_ms = 0;
        out = s;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "%s: unknown type", step_ctx());
    return ESP_ERR_INVALID_ARG;
}

esp_err_t procedure_executor::exec_one(const step &s, target_backend &backend)
{
    return std::visit(
        [&](auto &&concrete) -> esp_err_t {
            using T = std::decay_t<decltype(concrete)>;

            if constexpr (std::is_same_v<T, read32_step>) {
                uint32_t val = 0;
                auto ret = backend.read_mem32(concrete.addr, &val);
                ESP_LOGI(TAG, "exec: r32: 0x%08lx @ 0x%08lx", val, concrete.addr);
                return ret;
            } else if constexpr (std::is_same_v<T, write32_step>) {
                ESP_LOGI(TAG, "exec: w32: 0x%08lx @ 0x%08lx", concrete.data, concrete.addr);
                return backend.write_mem32(concrete.addr, concrete.data);
            } else if constexpr (std::is_same_v<T, read_mod_write32_step>) {
                ESP_LOGI(TAG, "exec: rmw32 0x%08lx mask 0x%08lx @ 0x%08lx", concrete.data, concrete.mask, concrete.addr);
                uint32_t val = 0;
                auto ret = backend.read_mem32(concrete.addr, &val);
                if (ret != ESP_OK) {
                    return ret;
                }
                val = (val & concrete.mask) | concrete.data;
                return backend.write_mem32(concrete.addr, val);
            } else if constexpr (std::is_same_v<T, poll32_step>) {
                ESP_LOGI(
                    TAG, "exec: poll32 0x%08lx mask 0x%08lx @ 0x%08lx, timeout %lums", concrete.expected, concrete.mask, concrete.addr,
                    concrete.timeout_ms
                );
                int64_t deadline_us = esp_timer_get_time() + (int64_t)concrete.timeout_ms * 1000;
                do {
                    uint32_t val = 0;
                    auto ret = backend.read_mem32(concrete.addr, &val);
                    if (ret != ESP_OK) {
                        return ret;
                    }
                    if ((val & concrete.mask) == concrete.expected) {
                        return ESP_OK;
                    }
                    if (esp_timer_get_time() >= deadline_us) {
                        break;
                    }
                    vTaskDelay(1);
                } while (true);
                return ESP_ERR_TIMEOUT;
            } else if constexpr (std::is_same_v<T, delay_ms_step>) {
                ESP_LOGI(TAG, "exec: delay_ms %lu", concrete.delay_ms);
                if (concrete.delay_ms == 0) {
                    return ESP_OK; // used by legacy no-op blob steps
                }
                if (concrete.delay_ms < portTICK_PERIOD_MS) {
                    esp_rom_delay_us(concrete.delay_ms * 1000);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(concrete.delay_ms));
                }
                return ESP_OK;
            } else if constexpr (std::is_same_v<T, swd_reinit_step>) {
                ESP_LOGI(TAG, "exec: reinit debug connection");
                return backend.reinit_debug();
            } else if constexpr (std::is_same_v<T, reset_target_step>) {
                ESP_LOGI(TAG, "exec: reset target");
                return backend.reset_target();
            } else if constexpr (std::is_same_v<T, halt_target_step>) {
                ESP_LOGI(TAG, "exec: halt target");
                return backend.halt_target();
            } else if constexpr (std::is_same_v<T, wait_halt_step>) {
                ESP_LOGI(TAG, "exec: wait halt");
                return backend.wait_halt();
            } else {
                static_assert(sizeof(T) == 0, "unhandled step type");
                return ESP_ERR_NOT_SUPPORTED;
            }
        },
        s
    );
}

esp_err_t procedure_executor::execute(target_backend &backend)
{
    if (step_count == 0) {
        ESP_LOGW(TAG, "exec: nothing to execute");
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < step_count; i++) {
        auto step_ret = exec_one(steps[i], backend);

        if (step_ret != ESP_OK) {
            const bool ignore = std::visit(
                [](auto &&concrete) {
                    return concrete.ignore_error;
                },
                steps[i]
            );
            if (ignore) {
                ESP_LOGW(TAG, "exec: step %zu/%zu failed (0x%x) but ignore_error is set, continuing", i + 1, step_count, step_ret);
            } else {
                ESP_LOGE(TAG, "exec: step %zu/%zu failed (0x%x), aborting", i + 1, step_count, step_ret);
                return step_ret;
            }
        }
    }

    return ESP_OK;
}

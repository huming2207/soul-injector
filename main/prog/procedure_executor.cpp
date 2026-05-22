#include <cstdint>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "procedure_executor.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "swd_host.h"
#include <sys/types.h>
#include <utility>

procedure_executor::procedure_executor(std::vector<procedure_executor::step> const &_steps) : steps(_steps)
{
}

esp_err_t procedure_executor::load_yaml(const char *path)
{
    if (path == nullptr) {
        ESP_LOGE(TAG, "load_yaml: path is null");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "load_yaml: failed to open file %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        ESP_LOGE(TAG, "load_yaml: failed to get file size for %s", path);
        return ESP_FAIL;
    }
    fseek(file, 0, SEEK_SET);

    auto *file_buffer = static_cast<char *>(heap_caps_calloc(1, file_size + 1, MALLOC_CAP_SPIRAM));
    if (file_buffer == nullptr) {
        ESP_LOGE(TAG, "load_yaml: can't allocate file buffer");
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(file_buffer, 1, file_size, file);
    fclose(file);

    if (read_bytes == 0 && file_size > 0) {
        ESP_LOGE(TAG, "load_yaml: failed to read file %s", path);
        free(file_buffer);
        return ESP_FAIL;
    }

    std::vector<procedure_executor::step> new_steps;
    try {
        ryml::Tree tree = ryml::parse_in_place(ryml::substr(file_buffer, read_bytes));
        ryml::ConstNodeRef root = tree.rootref();

        if (!root.is_map()) {
            ESP_LOGE(TAG, "load_yaml: YAML root is not a map");
            free(file_buffer);
            return ESP_ERR_INVALID_STATE;
        }

        if (!root.has_child("steps")) {
            ESP_LOGE(TAG, "load_yaml: YAML root does not contain 'steps'");
            free(file_buffer);
            return ESP_ERR_INVALID_STATE;
        }

        ryml::ConstNodeRef steps_node = root["steps"];
        if (!steps_node.is_seq()) {
            ESP_LOGE(TAG, "load_yaml: 'steps' key is not a sequence");
            free(file_buffer);
            return ESP_ERR_INVALID_STATE;
        }

        new_steps.reserve(steps_node.num_children());

        for (auto const& step_node : steps_node.children()) {
            if (!step_node.is_map()) {
                ESP_LOGE(TAG, "load_yaml: step item is not a map");
                free(file_buffer);
                return ESP_ERR_INVALID_STATE;
            }

            if (!step_node.has_child("type")) {
                ESP_LOGE(TAG, "load_yaml: step item missing 'type'");
                free(file_buffer);
                return ESP_ERR_INVALID_STATE;
            }

            ryml::csubstr type_val = step_node["type"].val();
            step_type type = string_to_type(type_val);
            if (type == UNKNOWN_TYPE) {
                free(file_buffer);
                return ESP_ERR_INVALID_ARG;
            }

            procedure_executor::step s = {};
            s.type = type;

            switch (type) {
                case READ_32:
                case WRITE_32: {
                    s.op.rw32.addr = parse_number(step_node["addr"]);
                    s.op.rw32.data = parse_number(step_node["data"]);
                    break;
                }
                case READ_BLOB:
                case WRITE_BLOB: {
                    s.op.rwblob.addr = parse_number(step_node["addr"]);
                    s.op.rwblob.buf = nullptr;
                    s.op.rwblob.buf_len = 0;
                    break;
                }
                case READ_MOD_WRITE_32: {
                    s.op.rmw32.addr = parse_number(step_node["addr"]);
                    s.op.rmw32.mask = parse_number(step_node["mask"]);
                    s.op.rmw32.data = parse_number(step_node["data"]);
                    break;
                }
                case POLL_32: {
                    s.op.poll32.addr = parse_number(step_node["addr"]);
                    s.op.poll32.mask = parse_number(step_node["mask"]);
                    s.op.poll32.expected = parse_number(step_node["expected"]);
                    s.op.poll32.timeout_ms = parse_number(step_node["timeout_ms"]);
                    break;
                }
                case DELAY_MS: {
                    s.op.delay_ms.delay_ms = parse_number(step_node["delay_ms"]);
                    break;
                }
                case SWD_REINIT:
                case SWD_RESET_TARGET:
                case SWD_HALT_TARGET:
                case SWD_WAIT_HALT: {
                    // No arguments needed
                    break;
                }
                default: {
                    break;
                }
            }

            new_steps.push_back(s);
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "load_yaml: exception during YAML parse: %s", e.what());
        free(file_buffer);
        return ESP_FAIL;
    } catch (...) {
        ESP_LOGE(TAG, "load_yaml: unknown exception during YAML parse");
        free(file_buffer);
        return ESP_FAIL;
    }

    free(file_buffer);
    clear();
    steps = std::move(new_steps);
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

            default: {
                ESP_LOGW(TAG, "exec: unimplemented type: %ld", item.type);
                break;
            }
        }
    }

    return ret;
}

void procedure_executor::clear()
{
    steps.clear();
}

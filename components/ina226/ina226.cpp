#include <esp_log.h>

#include "ina226.hpp"

#include <cmath>

ESP_EVENT_DEFINE_BASE(INA226_EVENTS);
static_assert(sizeof(ina226 *) <= CONFIG_ESP_EVENT_POST_FROM_ISR_SIZE);

esp_err_t ina226::init(i2c_master_bus_handle_t i2c_master, gpio_num_t alert, uint8_t addr, uint32_t freq_hz)
{
    if (i2c_dev != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    uint8_t i2c_addr = addr;
    if (addr == 0) {
        for (uint8_t curr_addr = 0x40; curr_addr <= 0x4f; curr_addr += 1) {
            ret = i2c_master_probe(i2c_master, curr_addr, 250);
            if (ret != ESP_OK) {
                if (curr_addr == 0x4f) {
                    ESP_LOGE(TAG, "Address auto-detect fail!");
                    return ret;
                } else {
                    ESP_LOGW(TAG, "Auto-detect address @ 0x%02x seems invalid", curr_addr);
                }
            } else {
                ESP_LOGI(TAG, "Address @ 0x%02x seems OK", (curr_addr));
                i2c_addr = curr_addr;
                break;
            }
        }
    } else {
        ret = i2c_master_probe(i2c_master, i2c_addr, 250);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "No response at addr @ 0x%02x", i2c_addr);
            return ret;
        }
    }

    i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = i2c_addr,
            .scl_speed_hz = freq_hz,
            .scl_wait_us = 0,
            .flags = {},
    };

    ret = i2c_master_bus_add_device(i2c_master, &dev_cfg, &i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: 0x%x", ret);
        return ret;
    }

    ret = reset();
    if (ret != ESP_OK) {
        return init_failed(ret);
    }

    uint16_t manufacturer_id = 0, dev_id = 0;
    ret = ret ?: read_u16(REG_MANUFACTURE_ID, &manufacturer_id);
    ret = ret ?: read_u16(REG_DIE_ID, &dev_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ID read failed: 0x%x", ret);
        return init_failed(ret);
    } else {
        ESP_LOGI(TAG, "Device OK: ID 0x%04x; 0x%04x", manufacturer_id, dev_id);
    }

    gpio_config_t alert_cfg = {
            .pin_bit_mask = (1ULL << (uint32_t)alert),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&alert_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Alert GPIO config failed: 0x%x", ret);
        return init_failed(ret);
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Default event loop init failed: 0x%x", ret);
        return init_failed(ret);
    }

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GPIO ISR service init failed: 0x%x", ret);
        return init_failed(ret);
    }

    alert_event_id = static_cast<int32_t>(alert);
    ret = gpio_isr_handler_add(alert, alert_isr_handler, this);
    ret = ret ?: gpio_set_intr_type(alert, GPIO_INTR_NEGEDGE);

    if (ret != ESP_OK) {
        alert_event_id = -1;
        gpio_isr_handler_remove(alert);
        ESP_LOGE(TAG, "Alert ISR init failed: 0x%x", ret);
        return init_failed(ret);
    }

    return ESP_OK;
}

esp_err_t ina226::init_failed(esp_err_t error)
{
    const esp_err_t cleanup_ret = i2c_master_bus_rm_device(i2c_dev);
    if (cleanup_ret == ESP_OK) {
        i2c_dev = nullptr;
    } else {
        ESP_LOGE(TAG, "I2C device cleanup failed: 0x%x", cleanup_ret);
    }
    return error;
}

esp_err_t ina226::configure_shunt(double r_shunt)
{
    if (!std::isfinite(r_shunt) || r_shunt <= 0.0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * INA226 shunt-voltage ADC:
     *
     *     2.5 uV / LSB
     *
     * Full signed scale:
     *
     *     +/-32768 * 2.5 uV
     *     ~= +/-81.92 mV
     *
     * Because this API does not provide maximum expected current,
     * choose Current_LSB such that one Current Register LSB
     * corresponds exactly to one Shunt Voltage Register LSB:
     *
     *     Current_LSB = 2.5uV / Rshunt
     *
     * From the INA226 calibration equation:
     *
     *     CAL = 0.00512 / (Current_LSB * Rshunt)
     *
     *         = 0.00512 / 2.5e-6
     *
     *         = 2048
     */

    constexpr double SHUNT_VOLTAGE_LSB = 2.5e-6;

    const double new_current_lsb = SHUNT_VOLTAGE_LSB / r_shunt;
    if (!std::isfinite(new_current_lsb) || new_current_lsb <= 0.0) {
        return ESP_ERR_INVALID_ARG;
    }

    constexpr uint16_t new_shunt_cal = 2048;
    const esp_err_t ret = write_u16(REG_CALIBRATION, new_shunt_cal);
    if (ret != ESP_OK) {
        return ret;
    }

    current_lsb = new_current_lsb;
    curr_r_shunt = r_shunt;
    shunt_cal = new_shunt_cal;

    ESP_LOGI(TAG, "Rshunt=%g ohm Current_LSB=%g A CAL=%u", r_shunt, current_lsb, shunt_cal);
    return ESP_OK;
}

esp_err_t ina226::read_voltage(double* volt_out, int32_t wait_ms)
{
    if (volt_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw = 0;

    esp_err_t ret = read_u16(REG_BUS_VOLT, &raw, wait_ms);

    if (ret != ESP_OK) {
        return ret;
    }

    // INA226 VBUS: 1.25 mV / LSB.
    *volt_out = static_cast<double>(raw) * 0.00125;
    return ESP_OK;
}

esp_err_t ina226::read_current(double* amps_out, int32_t wait_ms)
{
    if (amps_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (current_lsb <= 0.0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t raw_u16 = 0;

    esp_err_t ret = read_u16(REG_CURRENT, &raw_u16, wait_ms);

    if (ret != ESP_OK) {
        return ret;
    }

    // Current register is signed two's complement.
    const int16_t raw = static_cast<int16_t>(raw_u16);

    *amps_out = static_cast<double>(raw) * current_lsb;
    return ESP_OK;
}

esp_err_t ina226::read_volt_shunt_mv(double* volt_out, int32_t wait_ms)
{
    if (volt_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw_u16 = 0;

    esp_err_t ret = read_u16(REG_SHUNT_VOLT, &raw_u16, wait_ms);

    if (ret != ESP_OK) {
        return ret;
    }

    // Shunt voltage is a signed two's-complement register.
    const int16_t raw = static_cast<int16_t>(raw_u16);

    // 2.5 uV/LSB = 0.0025 mV/LSB.
    *volt_out = static_cast<double>(raw) * 0.0025;

    return ESP_OK;
}

esp_err_t ina226::read_power(double* power_out, int32_t wait_ms)
{
    if (power_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (current_lsb <= 0.0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t raw = 0;

    esp_err_t ret = read_u16(REG_POWER, &raw, wait_ms);

    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * INA226:
     *
     *     Power_LSB = 25 * Current_LSB
     */
    const double power_lsb = 25.0 * current_lsb;
    *power_out = static_cast<double>(raw) * power_lsb;
    return ESP_OK;
}

esp_err_t ina226::read_alert_flag(uint16_t* flag_out)
{
    if (flag_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    return read_u16(REG_MASK_EN, flag_out);
}

esp_err_t ina226::write_alert_flag(uint16_t flag)
{
    /*
     * Writable bits:
     *
     * 15 SOL
     * 14 SUL
     * 13 BOL
     * 12 BUL
     * 11 POL
     * 10 CNVR
     *  1 APOL
     *  0 LEN
     *
     * AFF/CVRF/OVF are status bits and should not be treated as
     * configuration bits.
     */
    constexpr uint16_t WRITABLE_MASK =
        ALERT_FLAG_SOL |
        ALERT_FLAG_SUL |
        ALERT_FLAG_BOL |
        ALERT_FLAG_BUL |
        ALERT_FLAG_POL |
        ALERT_FLAG_CNVR |
        ALERT_FLAG_APOL |
        ALERT_FLAG_LEN;

    return write_u16(REG_MASK_EN, flag & WRITABLE_MASK);
}

esp_err_t ina226::reset()
{
    // A failed transaction can still have reached the device; require recalibration.
    current_lsb = 0;
    curr_r_shunt = 0;
    shunt_cal = 0;
    auto ret = write_u16(REG_CONFIG, 0x8000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reset failed, check comm? 0x%x", ret);
        return ret;
    }

    return ret;
}

esp_err_t ina226::set_vbus_convert_time(adc_speed speed)
{
    if (speed > ADC_SPEED_8244US) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t config = 0;
    esp_err_t ret = read_u16(REG_CONFIG, &config);

    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * CONFIG:
     *
     * D8..D6 = VBUSCT
     */
    constexpr uint16_t MASK = 0x07 << 6;

    config &= ~MASK;
    config |= (static_cast<uint16_t>(speed) & 0x07) << 6;

    return write_u16(REG_CONFIG, config);
}

esp_err_t ina226::set_vshunt_convert_time(adc_speed speed)
{
    if (speed > ADC_SPEED_8244US) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t config = 0;

    esp_err_t ret = read_u16(REG_CONFIG, &config);

    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * CONFIG:
     *
     * D5..D3 = VSHCT
     */
    constexpr uint16_t MASK = 0x07 << 3;

    config &= ~MASK;
    config |= (static_cast<uint16_t>(speed) & 0x07) << 3;

    return write_u16(REG_CONFIG, config);
}

esp_err_t ina226::set_sample_mode(avg_mode sample_per_avg)
{
    if (sample_per_avg > AVG_SAMPLE_1024) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t config = 0;

    esp_err_t ret = read_u16(REG_CONFIG, &config);

    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * CONFIG:
     *
     * D11..D9 = AVG
     */
    constexpr uint16_t MASK = 0x07 << 9;

    config &= ~MASK;
    config |= (static_cast<uint16_t>(sample_per_avg) & 0x07) << 9;

    return write_u16(REG_CONFIG, config);
}

esp_err_t ina226::write(uint8_t reg, const uint8_t* buf, size_t len, int32_t wait_ms)
{
    if (buf == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_transmit_multi_buffer_info_t data[2] = {
        {
            .write_buffer = &reg,
            .buffer_size = 1,
        },
        {
            .write_buffer = buf,
            .buffer_size = len,
        }
    };

    auto ret = i2c_master_multi_buffer_transmit(i2c_dev, data, 2, wait_ms);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send: 0x%x", ret);
        return ret;
    }

    return ESP_OK;
}

esp_err_t ina226::write_u8(uint8_t reg, uint8_t data, int32_t wait_ms)
{
    return write(reg, (uint8_t *)&data, sizeof(data), wait_ms);
}

esp_err_t ina226::write_u16(uint8_t reg, uint16_t data, int32_t wait_ms)
{
    uint16_t data_send = __bswap16(data);
    return write(reg, reinterpret_cast<uint8_t *>(&data_send), sizeof(data_send), wait_ms);
}

esp_err_t ina226::read(uint8_t reg, uint8_t* buf_out, size_t buf_len, int32_t wait_ms) const
{
    if(buf_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto ret = i2c_master_transmit_receive(i2c_dev, &reg, sizeof(reg), buf_out, buf_len, wait_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read: 0x%x", ret);
        return ret;
    }

    return ESP_OK;
}

esp_err_t ina226::read_u8(uint8_t reg, uint8_t* out, int32_t wait_ms)
{
    return read(reg, out, sizeof(uint8_t), wait_ms);
}

esp_err_t ina226::read_u16(uint8_t reg, uint16_t* out, int32_t wait_ms)
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t data_out = 0;
    auto ret = read(reg, (uint8_t *)&data_out, sizeof(data_out), wait_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    *out = __bswap16(data_out);
    return ESP_OK;
}

esp_err_t ina226::set_alert_event_handler(esp_event_handler_t handler, void* arg)
{
    if (alert_event_id < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (event_instance != nullptr) {
        esp_err_t ret = esp_event_handler_instance_unregister(INA226_EVENTS, alert_event_id, event_instance);
        if (ret != ESP_OK) {
            return ret;
        }
        event_instance = nullptr;
    }

    if (handler == nullptr) {
        return ESP_OK;
    }

    return esp_event_handler_instance_register(INA226_EVENTS, alert_event_id, handler, arg, &event_instance);
}

void ina226::alert_isr_handler(void* _ctx)
{
    auto *ctx = static_cast<ina226 *>(_ctx);
    BaseType_t task_unblocked = pdFALSE;
    // esp_event copies the pointer value; the sensor must outlive queued events.
    if (esp_event_isr_post(INA226_EVENTS, ctx->alert_event_id, &ctx, sizeof(ctx), &task_unblocked) != ESP_OK) {
        ESP_DRAM_LOGE("ina226", "Alert event post failed");
    }
    if (task_unblocked == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

#include <esp_log.h>
#include <esp_timer.h>
#include "current_tester.hpp"

esp_err_t current_tester::init(gpio_num_t alert, uint8_t addr, i2c_master_bus_handle_t _i2c_bus, uint32_t freq_hz)
{
    esp_err_t ret = ESP_OK;

    if (_i2c_bus == nullptr && i2c_bus == nullptr) {
        i2c_master_bus_config_t master_cfg = {};
        master_cfg.clk_source = I2C_CLK_SRC_XTAL;
        master_cfg.sda_io_num = (gpio_num_t)CONFIG_SI_SG_I2C_SDA;
        master_cfg.scl_io_num = (gpio_num_t)CONFIG_SI_SG_I2C_SCL;
        master_cfg.i2c_port = (i2c_port_t)CONFIG_SI_SG_I2C_PERIPH;
        master_cfg.glitch_ignore_cnt = 7;

        ESP_LOGI(TAG, "init: Setting up I2C");
        ret = i2c_new_master_bus(&master_cfg, &i2c_bus);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "init: failed to set up I2C");
            return ret;
        }
    } else if (_i2c_bus != nullptr) {
        i2c_bus = _i2c_bus;
        ESP_LOGW(TAG, "init: using existing I2C master instance: %p", i2c_bus);
    } else {
        ESP_LOGI(TAG, "init: I2C already initialized, skipping bus setup");
    }

    ret = ret ?: adc.init(i2c_bus, alert, addr, freq_hz);
    ret = ret ?: configure();

    return ret;
}

esp_err_t current_tester::configure()
{
    auto ret = adc.reset();
    ret = ret ?: adc.configure_shunt(1);
    ret = ret ?: adc.set_adc_range(ina228::ADC_RANGE_0);
    ret = ret ?: adc.set_temperature_convert_time(ina228::ADC_SPEED_50US);
    ret = ret ?: adc.set_vbus_convert_time(ina228::ADC_SPEED_50US);
    ret = ret ?: adc.set_vshunt_convert_time(ina228::ADC_SPEED_50US);
    ret = ret ?: adc.set_adc_average(ina228::ADC_SAMPLE_1024);

    return ret;
}

esp_err_t current_tester::start_testing(uint32_t millisecs, double *max_ua, double *min_ua, double *avg_ua)
{
    double max = 0, min = UINT32_MAX, curr = 0, avg_sum = 0;
    uint32_t reading_cnt = 0;

    esp_err_t ret = configure();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC reset failed!");
    }

    int64_t finish_ts = esp_timer_get_time() + millisecs * 1000LL;

    while (true) {
        ret = adc.read_current(&curr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Current reading failed: 0x%x", ret);
            return ret;
        }

        reading_cnt += 1;
        avg_sum += curr;

        if (max < curr) {
            max = curr;
        }

        if (min > curr) {
            min = curr;
        }

        if (finish_ts < esp_timer_get_time()) {
            break;
        }

        vTaskDelay(1);
    }

    double avg = avg_sum / (double)reading_cnt;
    if (avg_ua != nullptr) {
        *avg_ua = avg;
    }

    if (min_ua != nullptr) {
        *min_ua = min;
    }

    if (max_ua != nullptr) {
        *max_ua = max;
    }

    return ret;
}

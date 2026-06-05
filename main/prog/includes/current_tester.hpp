#pragma once

#include <esp_err.h>
#include "ina228.hpp"

class current_tester
{
public:
    current_tester() = default;
    esp_err_t init(gpio_num_t alert, uint8_t addr = 0, i2c_master_bus_handle_t _i2c_bus = nullptr, uint32_t freq_hz = 400000);
    esp_err_t start_testing(uint32_t millisecs, double *max_ua, double *min_ua, double *avg_ua);

private:
    esp_err_t configure();
    i2c_master_bus_handle_t i2c_bus = nullptr;
    ina228 adc = {};

private:
    static const constexpr char TAG[] = "pwr_test";
};


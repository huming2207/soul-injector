#pragma once

#include <freertos/FreeRTOS.h>
#include <esp_event.h>
#include <esp_err.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>

ESP_EVENT_DECLARE_BASE(INA226_EVENTS);

class ina226
{
public:
    enum adc_speed : uint8_t {
        ADC_SPEED_140US = 0,
        ADC_SPEED_204US = 1,
        ADC_SPEED_332US = 2,
        ADC_SPEED_588US = 3,
        ADC_SPEED_1100US = 4,
        ADC_SPEED_2116US = 5,
        ADC_SPEED_4156US = 6,
        ADC_SPEED_8244US = 7,
    };

    enum avg_mode : uint8_t {
        AVG_SAMPLE_1 = 0,
        AVG_SAMPLE_4 = 1,
        AVG_SAMPLE_16 = 2,
        AVG_SAMPLE_64 = 3,
        AVG_SAMPLE_128 = 4,
        AVG_SAMPLE_256 = 5,
        AVG_SAMPLE_512 = 6,
        AVG_SAMPLE_1024 = 7,
    };

    enum operate_mode : uint8_t {
        OP_MODE_POWER_DOWN = 0,
        OP_MODE_SHUNT_VOLT_TRIG = 1,
        OP_MODE_BUS_VOLT_TRIG = 2,
        OP_MODE_SHUNT_BUS_TRIG = 3,
        OP_MODE_SHUNT_CONTINUOUS = 5,
        OP_MODE_BUS_CONTINUOUS = 6,
        OP_MODE_SHUNT_BUS_CONT = 7,
    };

    enum alert_flags : uint16_t {
        ALERT_FLAG_SOL  = BIT(15), // Shunt voltage over-limit alert enable
        ALERT_FLAG_SUL  = BIT(14), // Shunt voltage under-limit alert enable
        ALERT_FLAG_BOL  = BIT(13), // Bus voltage over-limit alert enable
        ALERT_FLAG_BUL  = BIT(12), // Bus voltage under-limit alert enable
        ALERT_FLAG_POL  = BIT(11), // Power over-limit alert enable
        ALERT_FLAG_CNVR = BIT(10), // Conversion-ready alert enable

        // Bits 9..5 reserved

        ALERT_FLAG_AFF  = BIT(4),  // Alert function flag
        ALERT_FLAG_CVRF = BIT(3),  // Conversion ready flag
        ALERT_FLAG_OVF  = BIT(2),  // Math overflow flag; current/power may be invalid
        ALERT_FLAG_APOL = BIT(1),  // Alert polarity (0 = active-low, 1 = active-high)
        ALERT_FLAG_LEN  = BIT(0),  // Alert latch enable (0 = transparent, 1 = latched)
    };

public:
    /**
     * Initialise INA228 (and I2C if needed)
     * @param port I2C Port number
     * @param addr INA228 address
     * @param alert INA228 interrupt pin
     * @param i2c_cfg Leave null if this I2C port has been set up elsewhere
     * @return ESP_OK if success
     */
    esp_err_t init(i2c_master_bus_handle_t i2c_master, gpio_num_t alert, uint8_t addr, uint32_t freq_hz = 400000);

    /**
     * Register or replace this sensor's handler on the default event loop after init().
     * Pass nullptr to unregister. Call from task context; serialize setter calls.
     * The handler receives arg, INA226_EVENTS, the alert GPIO as event ID, and
     * event_data pointing to a copied ina226* (valid only during the handler).
     * Read/clear alert flags with read_alert_flag() in the handler.
     * The sensor and arg must outlive the registration and queued events.
     */
    esp_err_t set_alert_event_handler(esp_event_handler_t handler, void *arg = nullptr);

    /**
     * Configure shunt setting
     * @param r_shunt Shunt resistor in ohms
     */
    esp_err_t configure_shunt(double r_shunt);

    /**
     * Read voltage
     * @param volt_out Voltage output
     * @param wait_ms Wait for milliseconds, or timeout if after that (default 1000ms)
     * @return ESP_OK if success
     */
    esp_err_t read_voltage(double *volt_out, int32_t wait_ms = 1000);

    /**
     * Read current
     * @param amps_out Current output
     * @param wait_ms Wait for milliseconds, or timeout if after that (default 1000ms)
     * @return ESP_OK if success
     */
    esp_err_t read_current(double *amps_out, int32_t wait_ms = 1000);

    /**
     * Read Vshut
     * @param volt_out Shunt voltage output
     * @param wait_ms Wait for milliseconds, or timeout if after that (default 1000ms)
     * @return ESP_OK if success
     */
    esp_err_t read_volt_shunt_mv(double *volt_out, int32_t wait_ms = 1000);

    /**
     * Read power output
     * @param power_out Power output
     * @param wait_ms Wait for milliseconds, or timeout if after that (default 1000ms)
     * @return ESP_OK if success
     */
    esp_err_t read_power(double *power_out, int32_t wait_ms = 1000);

    /**
     * Read alert flags
     * @param flag_out Flags output (bit field)
     * @return ESP_OK if success
     */
    esp_err_t read_alert_flag(uint16_t *flag_out);

    esp_err_t write_alert_flag(uint16_t flag);

    /**
     * Perform full reset
     * @return ESP_OK if success
     */
    esp_err_t reset();

    /**
     * Set shunt resistor temperature coefficient in PPM/degC
     *
     * @param ppm ppm/degC
     * @return ESP_OK if success
     */
    esp_err_t set_shunt_coefficient(uint16_t ppm);

    /**
     * Set VBUS conversion time
     * @param speed See datasheet
     * @return ESP_OK if success
     */
    esp_err_t set_vbus_convert_time(adc_speed speed);

    /**
     * Set Vshunt conversion time
     * @param speed See datasheet
     * @return ESP_OK if success
     */
    esp_err_t set_vshunt_convert_time(adc_speed speed);

    /**
     * Set ADC sample mode
     * @param sample_per_avg See datasheet
     * @return ESP_OK if success
     */
    esp_err_t set_sample_mode(avg_mode sample_per_avg);

private:
    esp_err_t init_failed(esp_err_t error);
    esp_err_t write(uint8_t reg, const uint8_t *buf, size_t len, int32_t wait_ms = 1000);
    esp_err_t write_u8(uint8_t reg, uint8_t data, int32_t wait_ms = 1000);
    esp_err_t write_u16(uint8_t reg, uint16_t data, int32_t wait_ms = 1000);
    esp_err_t read(uint8_t reg, uint8_t *buf_out, size_t buf_len, int32_t wait_ms = 1000) const;
    esp_err_t read_u8(uint8_t reg, uint8_t *out, int32_t wait_ms = 1000);
    esp_err_t read_u16(uint8_t reg, uint16_t *out, int32_t wait_ms = 1000);
    static IRAM_ATTR void alert_isr_handler(void *_ctx);

    uint8_t addr_msb = 0;
    uint16_t shunt_cal = 0;
    i2c_master_dev_handle_t i2c_dev = nullptr;
    esp_event_handler_instance_t event_instance = nullptr;
    int32_t alert_event_id = -1;
    double current_lsb = 0;
    double curr_r_shunt = 0;

private:
    static const constexpr char TAG[] = "ina226";

    static const constexpr uint8_t REG_CONFIG =			0x00;
    static const constexpr uint8_t REG_SHUNT_VOLT =		0x01;
    static const constexpr uint8_t REG_BUS_VOLT =		0x02;
    static const constexpr uint8_t REG_POWER =	        0x03;
    static const constexpr uint8_t REG_CURRENT =		0x04;
    static const constexpr uint8_t REG_CALIBRATION = 	0x05;
    static const constexpr uint8_t REG_MASK_EN =		0x06;
    static const constexpr uint8_t REG_ALERT_LIMIT =	0x07;
    static const constexpr uint8_t REG_MANUFACTURE_ID =	0xFE;
    static const constexpr uint8_t REG_DIE_ID =			0xFF;
};

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <power.pb.h>
#include <sidp_defs.hpp>

#include "ina226.hpp"

/**
 * Rev 8 power profiler: three INA226s in series between VPP_INPUT and
 * VPP_OUTPUT, one per range, measured over SIDP (sidp_client's
 * docs/sidp-power.md).
 *
 * Comparators move the range up in hardware; this class moves it down. While
 * measuring, the active range's sensor converts shunt voltage only, so no time
 * is skipped, and an idle sensor converts the bus voltage (VPP_INPUT). The
 * active sensor's conversion-ready alert on the shared /ALERT pin times each
 * sample.
 *
 * The sampler task runs on the core without SWD and owns the I2C bus. It packs
 * samples into POWER_DATA frames and hands them to the data sink, which may
 * drop them. start() and stop() are called from the SIDP service task only.
 */
class power_profiler
{
public:
    /** Queues one complete SIDP frame; the frame is copied before it returns. */
    using frame_sink_t = esp_err_t (*)(std::span<const uint8_t> frame);

    static power_profiler *instance()
    {
        static power_profiler _instance;
        return &_instance;
    }

    power_profiler(power_profiler const &) = delete;
    void operator=(power_profiler const &) = delete;

    /**
     * Set up the range latches and sensors and start the sampler task.
     * @param send_data Sink for POWER_DATA frames (expendable)
     * @param send_event Sink for the POWER_STOPPED event
     * @return ESP_ERR_NOT_SUPPORTED if this board has no profiler
     */
    esp_err_t init(frame_sink_t send_data, frame_sink_t send_event);

    bool available() const { return ready; }

    void get_info(si_power_PowerInfo &info) const;

    /**
     * Start measuring.
     * @return ESP_ERR_INVALID_ARG for an unsupported setting, ESP_ERR_INVALID_STATE while measuring
     */
    esp_err_t start(const si_power_StartRequest &request, si_power_StartResponse &response);

    /** Stop measuring, if running, and report the totals of the last measurement. */
    void stop(si_power_StopResponse &response);

private:
    power_profiler() = default;

    struct range_desc {
        uint8_t i2c_addr;
        float shunt_ohm;
        float chain_ohm; // Series resistance from VPP_INPUT while in this range
        uint32_t settle_us;
    };

    enum command : uint8_t { CMD_NONE, CMD_START, CMD_STOP };

    static void sampler_task(void *ctx);
    static void alert_isr(void *ctx);

    esp_err_t init_sensors();
    void run();
    si_power_StopReason measure();
    esp_err_t configure_sensors();
    bool read_latch(sidp::power_range_t *range) const;
    void clock_latch(bool data) const;
    bool sample(int64_t t_us, si_power_StopReason *reason);
    bool should_step_down(int64_t t_us, float amps, float vin);
    bool step_down(int64_t t_us);

    void append(int64_t t_us, int16_t shunt, uint16_t bus, bool settling);
    void close_run() { run_open = false; }
    void flush();
    void send_stopped(si_power_StopReason reason);

    float trip_min_amps(float vin, sidp::power_range_t into) const;

    static constexpr range_desc RANGES[sidp::POWER_RANGE_COUNT] = {
        {0x44, 162.0f, 166.07f, 20000}, // LOW: 162 ohm x (C17 + target caps) needs the longest
        {0x41, 4.02f, 4.07f, 2000},     // MID
        {0x40, 0.05f, 0.05f, 1000},     // HIGH
    };

    static constexpr uint16_t CONVERSION_US[8] = {140, 204, 332, 588, 1100, 2116, 4156, 8244};
    static constexpr uint16_t AVERAGES[8] = {1, 4, 16, 64, 128, 256, 512, 1024};

    /*
     * Above the INA226's 400 kHz fast mode and the ESP32-S31's rated 800 kHz,
     * without the HS master code. It works on the Rev 8 board tested, and it
     * is what lets 588 us run without missed conversions.
     */
    static constexpr uint32_t I2C_SPEED_HZ = 1000000;

    /* Three register reads per sample. At 332 us the sampler falls behind even at 1 MHz. */
    static constexpr uint32_t MIN_SAMPLE_PERIOD_US = 588;

    static constexpr float SHUNT_LSB_V = 2.5e-6f;
    static constexpr float BUS_LSB_V = 1.25e-3f;
    static constexpr float VPP_MIN_V = 1.7f;
    static constexpr float VPP_MAX_V = 3.7f;
    static constexpr uint32_t NO_SUPPLY_SAMPLES = 3;

    /* Comparator trip drop is VPP_INPUT / 92 nominally, at least 9 mV below that. */
    static constexpr float VTH_DIVIDER = 92.0f;
    static constexpr float TRIP_BELOW_NOMINAL_V = 0.009f;
    static constexpr float STEP_DOWN_MARGIN = 0.7f;
    static constexpr uint32_t STEP_DOWN_SAMPLES = 3;
    static constexpr int64_t STEP_DOWN_HOLDOFF_US = 200000;

    static constexpr int64_t FLUSH_INTERVAL_US = 20000;
    static constexpr uint32_t ALERT_SLACK_MS = 20;
    static constexpr uint32_t ALERT_TIMEOUTS = 2;

    static constexpr uint32_t NOTIFY_ALERT = BIT(0);
    static constexpr uint32_t NOTIFY_COMMAND = BIT(1);

    /* POWER_DATA shares the log queue, so it obeys the log frame size. */
    static constexpr size_t FRAME_SIZE = sizeof(sidp::msg_header_t) + sizeof(sidp::log_data_t) + 1024;

    frame_sink_t send_data = nullptr;
    frame_sink_t send_event = nullptr;
    bool ready = false;

    i2c_master_bus_handle_t i2c_bus = nullptr;
    ina226 sensors[sidp::POWER_RANGE_COUNT] = {};
    TaskHandle_t sampler = nullptr;
    SemaphoreHandle_t done = nullptr; // The sampler took a command, or finished init
    StaticSemaphore_t done_buf = {};
    esp_err_t init_result = ESP_FAIL;

    // Set by start() before CMD_START and read by the sampler afterwards.
    volatile command pending = CMD_NONE;
    ina226::adc_speed conversion = ina226::ADC_SPEED_1100US;
    ina226::avg_mode averaging = ina226::AVG_SAMPLE_1;
    uint32_t period_us = 0;
    sidp::power_range_t lowest = sidp::POWER_RANGE_LOW;
    uint32_t duration_ms = 0;
    volatile bool running = false;

    int64_t start_us = 0;
    int64_t end_us = 0; // 0: no duration

    // Sampler state
    volatile int64_t alert_us = 0;
    sidp::power_range_t active = sidp::POWER_RANGE_HIGH;
    int64_t settle_until_us = 0;
    int64_t holdoff_until_us = 0;
    uint32_t below_limit = 0;
    uint32_t no_supply = 0;
    int64_t last_sample_us = 0;

    // Totals of the current or last measurement
    uint32_t sample_count = 0;
    uint32_t dropped_frames = 0;

    // Frame being built: header, power_data_t, then runs
    uint8_t *frame = nullptr;
    size_t frame_len = 0;
    int64_t frame_t_us = 0;
    uint32_t seq = 0;
    uint16_t run_count = 0;
    bool run_open = false;
    size_t run_offset = 0;

    static const constexpr char *TAG = "power";
};

#include "power_profiler.hpp"

#include <cstring>

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <pb_encode.h>
#include <sidp_transport.hpp>

#include "swd_prog.hpp"

#if CONFIG_SI_POWER_PROFILER_ENABLE
static const constexpr gpio_num_t PIN_SDA = static_cast<gpio_num_t>(CONFIG_SI_POWER_PROF_SDA_PIN);
static const constexpr gpio_num_t PIN_SCL = static_cast<gpio_num_t>(CONFIG_SI_POWER_PROF_SCL_PIN);
static const constexpr gpio_num_t PIN_ALERT = static_cast<gpio_num_t>(CONFIG_SI_POWER_PROF_ALERT_PIN);
static const constexpr gpio_num_t PIN_RANGE_D = static_cast<gpio_num_t>(CONFIG_SI_POWER_PROF_RANGE_D_PIN);
static const constexpr gpio_num_t PIN_RANGE_CLK = static_cast<gpio_num_t>(CONFIG_SI_POWER_PROF_RANGE_CLK_PIN);
static const constexpr gpio_num_t PIN_HIGH_READBACK = static_cast<gpio_num_t>(CONFIG_SI_POWER_PROF_HIGH_RB_PIN);
static const constexpr gpio_num_t PIN_MID_READBACK = static_cast<gpio_num_t>(CONFIG_SI_POWER_PROF_MID_RB_PIN);
#endif

/* The sampler takes the core without SWD, so I2C and sampling never delay a debug request. */
static const constexpr int SAMPLER_CORE = 1 - swd_def::TASK_CORE;

esp_err_t power_profiler::init(frame_sink_t _send_data, frame_sink_t _send_event)
{
#if !CONFIG_SI_POWER_PROFILER_ENABLE
    (void)_send_data;
    (void)_send_event;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (sampler != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    send_data = _send_data;
    send_event = _send_event;
    frame = static_cast<uint8_t *>(heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    done = xSemaphoreCreateBinaryStatic(&done_buf);
    if (frame == nullptr || done == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // Both bypasses on (HIGH range) until a measurement starts.
    gpio_set_level(PIN_RANGE_D, 1);
    gpio_set_level(PIN_RANGE_CLK, 0);
    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask = BIT64(PIN_RANGE_D) | BIT64(PIN_RANGE_CLK);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config_t in_cfg = {};
    in_cfg.pin_bit_mask = BIT64(PIN_HIGH_READBACK) | BIT64(PIN_MID_READBACK);
    in_cfg.mode = GPIO_MODE_INPUT;
    gpio_config_t alert_cfg = {};
    alert_cfg.pin_bit_mask = BIT64(PIN_ALERT);
    alert_cfg.mode = GPIO_MODE_INPUT;
    alert_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    alert_cfg.intr_type = GPIO_INTR_NEGEDGE;

    esp_err_t ret = gpio_config(&out_cfg);
    ret = ret ?: gpio_config(&in_cfg);
    ret = ret ?: gpio_config(&alert_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init: GPIO setup failed: %s", esp_err_to_name(ret));
        return ret;
    }
    clock_latch(true);
    clock_latch(true);

    // The sampler creates the I2C bus so that its interrupt lands on the sampler's core.
    if (xTaskCreatePinnedToCore(sampler_task, "power", 4096, this, tskIDLE_PRIORITY + 9, &sampler, SAMPLER_CORE) != pdPASS) {
        ESP_LOGE(TAG, "init: cannot create the sampler task");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(done, portMAX_DELAY);
    if (init_result != ESP_OK) {
        ESP_LOGE(TAG, "init: sensors not ready: %s", esp_err_to_name(init_result));
        return init_result;
    }

    // bootstrap_fsm installed the GPIO ISR service at startup.
    ret = gpio_isr_handler_add(PIN_ALERT, alert_isr, this);
    if (ret != ESP_OK) {
        return ret;
    }

    ready = true;
    ESP_LOGI(TAG, "init: profiler ready");
    return ESP_OK;
#endif
}

void power_profiler::get_info(si_power_PowerInfo &info) const
{
    info.ranges_count = sidp::POWER_RANGE_COUNT;
    for (uint8_t r = 0; r < sidp::POWER_RANGE_COUNT; ++r) {
        info.ranges[r].range = static_cast<si_power_Range>(r);
        info.ranges[r].shunt_ohm = RANGES[r].shunt_ohm;
        info.ranges[r].chain_ohm = RANGES[r].chain_ohm;
    }
    info.shunt_lsb_v = SHUNT_LSB_V;
    info.bus_lsb_v = BUS_LSB_V;
    info.conversion_us_count = sizeof(CONVERSION_US) / sizeof(CONVERSION_US[0]);
    for (uint8_t i = 0; i < info.conversion_us_count; ++i) {
        info.conversion_us[i] = CONVERSION_US[i];
    }
    info.averages_count = sizeof(AVERAGES) / sizeof(AVERAGES[0]);
    for (uint8_t i = 0; i < info.averages_count; ++i) {
        info.averages[i] = AVERAGES[i];
    }
    info.min_sample_period_us = MIN_SAMPLE_PERIOD_US;
    info.vout_measured = false;
}

esp_err_t power_profiler::start(const si_power_StartRequest &request, si_power_StartResponse &response)
{
    if (!ready) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (running) {
        return ESP_ERR_INVALID_STATE;
    }

    // The INA226 encodes both settings as an index into these tables.
    int8_t conversion_index = -1;
    int8_t averages_index = -1;
    const uint32_t averages = request.averages == 0 ? 1 : request.averages;
    for (uint8_t i = 0; i < 8; ++i) {
        conversion_index = CONVERSION_US[i] == request.conversion_us ? i : conversion_index;
        averages_index = AVERAGES[i] == averages ? i : averages_index;
    }
    if (conversion_index < 0 || averages_index < 0 || request.lowest_range > si_power_Range_RANGE_HIGH) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t period = request.conversion_us * averages;
    if (period < MIN_SAMPLE_PERIOD_US) {
        return ESP_ERR_INVALID_ARG;
    }

    conversion = static_cast<ina226::adc_speed>(conversion_index);
    averaging = static_cast<ina226::avg_mode>(averages_index);
    period_us = period;
    lowest = static_cast<sidp::power_range_t>(request.lowest_range);
    duration_ms = request.duration_ms;

    running = true;
    pending = CMD_START;
    xTaskNotify(sampler, NOTIFY_COMMAND, eSetBits);
    xSemaphoreTake(done, portMAX_DELAY);

    response.sample_period_us = period_us;
    response.start_us = start_us;
    return ESP_OK;
}

void power_profiler::stop(si_power_StopResponse &response)
{
    if (ready) {
        pending = CMD_STOP;
        xTaskNotify(sampler, NOTIFY_COMMAND, eSetBits);
        xSemaphoreTake(done, portMAX_DELAY);
    }
    response.samples = sample_count;
    response.dropped_frames = dropped_frames;
}

void power_profiler::sampler_task(void *ctx)
{
    auto *self = static_cast<power_profiler *>(ctx);
    self->init_result = self->init_sensors();
    xSemaphoreGive(self->done);
    if (self->init_result != ESP_OK) {
        self->sampler = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    self->run();
}

void IRAM_ATTR power_profiler::alert_isr(void *ctx)
{
    auto *self = static_cast<power_profiler *>(ctx);
    self->alert_us = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(self->sampler, NOTIFY_ALERT, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

esp_err_t power_profiler::init_sensors()
{
#if CONFIG_SI_POWER_PROFILER_ENABLE
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = -1;
    bus_cfg.sda_io_num = PIN_SDA;
    bus_cfg.scl_io_num = PIN_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &i2c_bus);
    if (ret != ESP_OK) {
        return ret;
    }

    // Idle sensors are powered down; a measurement configures them.
    for (uint8_t r = 0; r < sidp::POWER_RANGE_COUNT; ++r) {
        ret = sensors[r].init(i2c_bus, GPIO_NUM_NC, RANGES[r].i2c_addr, I2C_SPEED_HZ);
        ret = ret ?: sensors[r].set_config(ina226::AVG_SAMPLE_1, ina226::ADC_SPEED_1100US, ina226::ADC_SPEED_1100US,
                                           ina226::OP_MODE_POWER_DOWN);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "init_sensors: sensor 0x%02x: %s", RANGES[r].i2c_addr, esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void power_profiler::run()
{
    while (true) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        if ((bits & NOTIFY_COMMAND) == 0) {
            continue;
        }

        const command cmd = pending;
        pending = CMD_NONE;
        if (cmd != CMD_START) {
            xSemaphoreGive(done); // A stop while idle has nothing to do.
            continue;
        }

        sample_count = 0;
        dropped_frames = 0;
        seq = 0;
        frame_len = 0;
        run_open = false;
        start_us = esp_timer_get_time();
        end_us = duration_ms == 0 ? 0 : start_us + static_cast<int64_t>(duration_ms) * 1000;
        xSemaphoreGive(done);

        const si_power_StopReason reason = measure();
        flush();

        for (uint8_t r = 0; r < sidp::POWER_RANGE_COUNT; ++r) {
            sensors[r].write_alert_flag(0);
            sensors[r].set_config(averaging, conversion, conversion, ina226::OP_MODE_POWER_DOWN);
        }
        clock_latch(true);
        clock_latch(true);
        running = false;

        if (reason == si_power_StopReason_STOP_REASON_UNKNOWN) {
            xSemaphoreGive(done); // Stopped by request
        } else {
            ESP_LOGW(TAG, "run: stopped on its own, reason %d", reason);
            send_stopped(reason);
        }
    }
}

si_power_StopReason power_profiler::measure()
{
    // Start in HIGH, with both bypasses on, and step down from there.
    clock_latch(true);
    clock_latch(true);
    sidp::power_range_t hw = sidp::POWER_RANGE_LOW;
    if (!read_latch(&hw) || hw != sidp::POWER_RANGE_HIGH) {
        return si_power_StopReason_STOP_RANGE_FAULT;
    }

    active = sidp::POWER_RANGE_HIGH;
    reset_step_average();
    no_supply = 0;
    holdoff_until_us = 0;
    settle_until_us = esp_timer_get_time() + RANGES[active].settle_us;
    if (configure_sensors() != ESP_OK) {
        return si_power_StopReason_STOP_SENSOR_FAILED;
    }

    // A conversion is due every period; allow slack for I2C and scheduling.
    const int64_t alert_timeout_us = period_us + ALERT_SLACK_MS * 1000;
    int64_t last_alert_us = esp_timer_get_time();
    uint32_t timeouts = 0;

    while (true) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(FLUSH_INTERVAL_US / 1000) + 1);
        if ((bits & NOTIFY_COMMAND) != 0 && pending == CMD_STOP) {
            pending = CMD_NONE;
            return si_power_StopReason_STOP_REASON_UNKNOWN;
        }

        const int64_t now = esp_timer_get_time();
        if (end_us != 0 && now >= end_us) {
            return si_power_StopReason_STOP_DURATION;
        }

        if ((bits & NOTIFY_ALERT) != 0) {
            si_power_StopReason reason = si_power_StopReason_STOP_REASON_UNKNOWN;
            if (!sample(alert_us, &reason)) {
                return reason;
            }
            last_alert_us = now;
            timeouts = 0;
        } else if (now - last_alert_us > alert_timeout_us) {
            // No conversion arrived: set the sensors up again once before giving up.
            if (++timeouts >= ALERT_TIMEOUTS) {
                return si_power_StopReason_STOP_SENSOR_FAILED;
            }
            ESP_LOGW(TAG, "measure: no conversion from sensor 0x%02x", RANGES[active].i2c_addr);
            close_run();
            if (configure_sensors() != ESP_OK) {
                return si_power_StopReason_STOP_SENSOR_FAILED;
            }
            last_alert_us = now;
        }

        if (frame_len != 0 && now - frame_t_us >= FLUSH_INTERVAL_US) {
            flush();
        }
    }
}

esp_err_t power_profiler::configure_sensors()
{
    // An idle sensor measures VPP_INPUT; every bus input is wired there.
    const uint8_t vin_index = active == sidp::POWER_RANGE_HIGH ? sidp::POWER_RANGE_MID : sidp::POWER_RANGE_HIGH;

    esp_err_t ret = ESP_OK;
    for (uint8_t r = 0; r < sidp::POWER_RANGE_COUNT && ret == ESP_OK; ++r) {
        if (r == active) {
            continue;
        }
        const auto mode = r == vin_index ? ina226::OP_MODE_BUS_CONTINUOUS : ina226::OP_MODE_POWER_DOWN;
        ret = sensors[r].write_alert_flag(0);
        ret = ret ?: sensors[r].set_config(averaging, conversion, conversion, mode);
    }

    // Writing the configuration restarts conversion and clears the ready flag.
    ret = ret ?: sensors[active].set_config(averaging, conversion, conversion, ina226::OP_MODE_SHUNT_CONTINUOUS);
    ret = ret ?: sensors[active].write_alert_flag(ina226::ALERT_FLAG_CNVR);
    ulTaskNotifyValueClear(nullptr, NOTIFY_ALERT);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "configure_sensors: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool power_profiler::sample(int64_t t_us, si_power_StopReason *reason)
{
    const uint8_t vin_index = active == sidp::POWER_RANGE_HIGH ? sidp::POWER_RANGE_MID : sidp::POWER_RANGE_HIGH;

    // Reading Mask/Enable clears the ready flag and releases /ALERT for the next conversion.
    uint16_t flags = 0;
    int16_t shunt = 0;
    uint16_t bus = 0;
    esp_err_t ret = sensors[active].read_alert_flag(&flags);
    ret = ret ?: sensors[active].read_shunt_raw(&shunt);
    ret = ret ?: sensors[vin_index].read_bus_raw(&bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sample: I2C read failed: %s", esp_err_to_name(ret));
        *reason = si_power_StopReason_STOP_SENSOR_FAILED;
        return false;
    }
    if ((flags & ina226::ALERT_FLAG_CVRF) == 0) {
        return true; // A late edge from before the last reconfiguration
    }

    // A comparator that moved the range during the conversion makes it invalid.
    sidp::power_range_t hw = active;
    read_latch(&hw);
    if (hw != active) {
        if (hw > active) {
            holdoff_until_us = t_us + STEP_DOWN_HOLDOFF_US;
        } else {
            ESP_LOGW(TAG, "sample: range latch dropped on its own, %d -> %d", active, hw);
        }
        active = hw;
        reset_step_average();
        settle_until_us = t_us + RANGES[active].settle_us;
        close_run();
        if (configure_sensors() != ESP_OK) {
            *reason = si_power_StopReason_STOP_SENSOR_FAILED;
            return false;
        }
        return true;
    }

    const float vin = bus * BUS_LSB_V;
    if (vin < VPP_MIN_V || vin > VPP_MAX_V) {
        if (++no_supply >= NO_SUPPLY_SAMPLES) {
            *reason = si_power_StopReason_STOP_NO_SUPPLY;
            return false;
        }
    } else {
        no_supply = 0;
    }

    // A missed conversion breaks the even spacing of a run.
    if (run_open && t_us - last_sample_us > static_cast<int64_t>(period_us) * 3 / 2) {
        close_run();
    }
    append(t_us, shunt, bus, t_us < settle_until_us);
    last_sample_us = t_us;
    sample_count++;

    const float amps = shunt * SHUNT_LSB_V / RANGES[active].shunt_ohm;
    if (should_step_down(t_us, amps, vin) && !step_down(t_us)) {
        *reason = si_power_StopReason_STOP_RANGE_FAULT;
        return false;
    }
    return true;
}

bool power_profiler::should_step_down(int64_t t_us, float amps, float vin)
{
    if (active <= lowest || t_us < holdoff_until_us) {
        reset_step_average();
        return false;
    }

    // Judge the average, not single readings: HIGH's 50 uA steps make a load
    // near the limit read on both sides of it.
    step_sum_amps += amps;
    if (++step_samples < STEP_DOWN_SAMPLES) {
        return false;
    }
    const float average = step_sum_amps / static_cast<float>(step_samples);
    reset_step_average();

    const float limit = trip_min_amps(vin, static_cast<sidp::power_range_t>(active - 1)) * STEP_DOWN_MARGIN;
    return limit > 0.0f && average < limit;
}

bool power_profiler::step_down(int64_t t_us)
{
    const auto expected = static_cast<sidp::power_range_t>(active - 1);
    clock_latch(false);

    sidp::power_range_t hw = expected;
    read_latch(&hw);
    if (hw < expected) {
        ESP_LOGE(TAG, "step_down: latches went to %d instead of %d", hw, expected);
        return false;
    }
    if (hw > expected) {
        // A comparator tripped straight back up: the load is above the new range.
        holdoff_until_us = t_us + STEP_DOWN_HOLDOFF_US;
    }

    active = hw;
    reset_step_average();
    settle_until_us = esp_timer_get_time() + RANGES[active].settle_us;
    close_run();
    return configure_sensors() == ESP_OK;
}

float power_profiler::trip_min_amps(float vin, sidp::power_range_t into) const
{
    const float drop = vin / VTH_DIVIDER - TRIP_BELOW_NOMINAL_V;
    return drop > 0.0f ? drop / RANGES[into].chain_ohm : 0.0f;
}

bool power_profiler::read_latch(sidp::power_range_t *range) const
{
#if CONFIG_SI_POWER_PROFILER_ENABLE
    if (gpio_get_level(PIN_HIGH_READBACK) != 0) {
        *range = sidp::POWER_RANGE_HIGH;
    } else {
        *range = gpio_get_level(PIN_MID_READBACK) != 0 ? sidp::POWER_RANGE_MID : sidp::POWER_RANGE_LOW;
    }
    return true;
#else
    (void)range;
    return false;
#endif
}

void power_profiler::clock_latch(bool data) const
{
#if CONFIG_SI_POWER_PROFILER_ENABLE
    // D feeds the HIGH latch, which feeds the MID latch, on a shared rising clock.
    gpio_set_level(PIN_RANGE_D, data ? 1 : 0);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_RANGE_CLK, 1);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_RANGE_CLK, 0);
    esp_rom_delay_us(1);
#else
    (void)data;
#endif
}

void power_profiler::append(int64_t t_us, int16_t shunt, uint16_t bus, bool settling)
{
    constexpr size_t frame_header_len = sizeof(sidp::msg_header_t) + sizeof(sidp::power_data_t);
    const size_t needed = sizeof(sidp::power_raw_sample_t) + (run_open ? 0 : sizeof(sidp::power_run_t));
    if (frame_len != 0 && frame_len + needed > FRAME_SIZE) {
        flush();
    }

    if (frame_len == 0) {
        frame_len = frame_header_len;
        frame_t_us = t_us;
        run_count = 0;
        run_open = false;
    }

    const auto offset = static_cast<uint32_t>(t_us - frame_t_us);
    sidp::power_run_t run = {};
    if (run_open) {
        memcpy(&run, frame + run_offset, sizeof(run));
    } else {
        run_offset = frame_len;
        run.range = active;
        run.encoding = sidp::POWER_ENCODING_RAW;
        run.t_first_us = offset;
        frame_len += sizeof(run);
        run_count++;
        run_open = true;
    }

    const sidp::power_raw_sample_t raw = {shunt, bus};
    memcpy(frame + frame_len, &raw, sizeof(raw));
    frame_len += sizeof(raw);

    // Settling samples are counted only at the start of a run.
    if (settling && run.settling == run.count) {
        run.settling++;
    }
    run.count++;
    run.t_last_us = offset;
    memcpy(frame + run_offset, &run, sizeof(run));
}

void power_profiler::flush()
{
    if (frame_len == 0) {
        return;
    }

    sidp::msg_header_t header = {};
    header.version = sidp::PROTOCOL_VERSION;
    header.kind = sidp::KIND_LOG_STREAM;
    header.opcode = sidp::OP_POWER_DATA;
    header.request_id = 0;
    memcpy(frame, &header, sizeof(header));

    sidp::power_data_t data = {};
    data.seq = seq++;
    data.t_us = static_cast<uint64_t>(frame_t_us);
    data.run_count = run_count;
    memcpy(frame + sizeof(header), &data, sizeof(data));

    // The log queue drops a frame it has no room for; the sequence gap shows it.
    std::span<uint8_t> message(frame, frame_len);
    if (!sidp::crc32_hasher::set_message_crc(message) || send_data == nullptr || send_data(message) != ESP_OK) {
        dropped_frames++;
    }

    frame_len = 0;
    run_count = 0;
    run_open = false;
}

void power_profiler::send_stopped(si_power_StopReason reason)
{
    uint8_t buf[sizeof(sidp::msg_header_t) + si_power_Stopped_size] = {};
    si_power_Stopped stopped = si_power_Stopped_init_zero;
    stopped.reason = reason;
    stopped.samples = sample_count;
    stopped.dropped_frames = dropped_frames;

    pb_ostream_t out = pb_ostream_from_buffer(buf + sizeof(sidp::msg_header_t), si_power_Stopped_size);
    if (!pb_encode(&out, si_power_Stopped_fields, &stopped)) {
        ESP_LOGE(TAG, "send_stopped: encode failed: %s", PB_GET_ERROR(&out));
        return;
    }

    sidp::msg_header_t header = {};
    header.version = sidp::PROTOCOL_VERSION;
    header.kind = sidp::KIND_EVENT;
    header.opcode = sidp::EVT_POWER_STOPPED;
    header.request_id = 0;
    memcpy(buf, &header, sizeof(header));

    std::span<uint8_t> message(buf, sizeof(sidp::msg_header_t) + out.bytes_written);
    if (!sidp::crc32_hasher::set_message_crc(message) || send_event == nullptr || send_event(message) != ESP_OK) {
        ESP_LOGW(TAG, "send_stopped: event not queued");
    }
}

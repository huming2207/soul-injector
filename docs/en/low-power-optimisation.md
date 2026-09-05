# Future low-power optimisations

[中文](../zh/low-power-optimisation.md)

These engineering notes describe proposed work for Rev 7.1 and later hardware
using **ESP32-S31**, not ESP32-S3, and the Quectel EG800Z modem. They are based on
the firmware and local ESP-IDF v6.1 source reviewed on 2026-09-05. They are not a
claim that the changes have been implemented or validated on hardware.

## Baseline and measurement conditions

Previously reported board measurements were approximately:

| State | Reported current |
| --- | --- |
| ESP light sleep, modem off | 600 µA |
| ESP deep sleep, modem `QSCLK=1` | 6 mA |
| ESP light sleep with UART FIFO wake, modem `QSCLK=1` | 11–12 mA |
| UART wake configuration removed, modem `QSCLK=1` | 5–6 mA |

These are measurements from different tests, not a controlled component power
budget. Use the same supply voltage, measurement point, network conditions and
averaging window when comparing changes. Currents measured on different voltage
rails cannot simply be added; account for converter efficiency when comparing
input power.

The source reviewed on the date above instead requests
`AT+QSCLK=3,5,"10100001","00000001"` and calls
`esp_deep_sleep(1000000000)` immediately after `setup_modem()`. This requests a
one-minute TAU interval and a two-second active timer, then sleeps the ESP for
1,000 seconds. The requested timers are subject to network assignment. This is
not the earlier `QSCLK=1` test. Registration is not awaited before ESP deep sleep;
the modem may still be registering. The ping task waits for PPP connection and
does not send in this path.

The configuration to inspect is `build-rev71/sdkconfig.rev71`, rather than the
root `sdkconfig`. Record the flashed firmware version and configuration with
each capture.

## Prioritised work

| Priority | Proposed change | Purpose / verification |
| --- | --- | --- |
| 1 | Replace HP UART FIFO wake with MAIN_RI on an LP GPIO | Allow UART-related clocks to stop; verify both URC and PPP receive behaviour |
| 2 | Remove the DTR internal pull-down | Eliminate a resistive load while DTR is driven high |
| 3 | Replace the UART task's 100 ms polling with event-driven waiting | Reduce unnecessary CPU wakeups while preserving terminal shutdown and RX recovery |
| 4 | Trial a longer sleep-clock calibration interval | Reduce sleep entry overhead; verify timing and stability |
| 5 | Enable UART retention and peripheral power-down | Reduce the remaining ESP sleep load; confirm the domain actually powers down |
| 6 | Measure modem and interface loads separately | Locate residual current and explain the low VDD_EXT voltage |

### UART wake and MAIN_RI

In the reviewed IDF, HP UART FIFO wake setup keeps XTAL powered and ungates the
UART and IO-MUX clocks. The S31 PMU also changes analog sleep settings when XTAL
remains powered. The observed penalty therefore includes more than the UART
receiver itself. `esp_pm_dump_locks()` does not expose all sleep clock and domain
requirements. When changing wake modes at runtime, undo the old wake configuration
as well as changing the wake source; removing source enablement alone does not
undo `uart_wakeup_setup()`.

Rev 7.1 already connects MAIN_RI to U9/TXS0108E A4. B4, pin 16, is unconnected.
Route this translated signal to an available S31 LP GPIO in the next revision,
or use a temporary wire for a PoC. GPIO2 is a candidate provided its breakout
function is no longer needed. Keep a defined inactive high level on the ESP side
when the modem or translator is off; a weak external pull-up is a candidate to
validate with the translator. Do not pull the modem's 1.8 V pin directly to 3.3 V.

| Sleep configuration | Wake pin selection |
| --- | --- |
| Ordinary light sleep, peripheral domain powered | Any available valid GPIO |
| Light sleep with peripheral power-down, or deep sleep | S31 LP/RTC GPIO0–GPIO7 |

For peripheral power-down, configure the pin as an input and use:

```cpp
ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
    BIT64(CELL_MAIN_RI_PIN), ESP_GPIO_WAKEUP_GPIO_LOW));
```

Ordinary `gpio_wakeup_enable()` plus `esp_sleep_enable_gpio_wakeup()` is not the
appropriate wake path when the digital peripheral domain is off. Follow the
selected API's pull-resistor guidance, including
`CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS` when using external pulls.

Quectel documents MAIN_RI as idle high, with at least 120 ms low before UART
URC/SMS output. `AT+QCFG="risignaltype","physical"` configures physical URC
indication; also check the URC output port and RI configuration. **This does not
establish that every PPP data burst gets the same advance warning.** Capture RI,
RX, TX, DTR and RTS during incoming PPP traffic before making RI-only wake a
product dependency. Verify flow control can prevent data loss until the ESP UART
is ready. Keep the ESP awake through the RI-to-data gap and receive transaction;
avoid immediate re-sleep or repeated low-level wake while RI remains asserted.

### GPIO levels and translator loads

The reviewed `modem_manager::init()` sets DTR to `GPIO_PULLDOWN_ONLY`, but
`QuectelDTE::allow_sleep()` drives it high. Disable that opposing pull-down and
ensure DTR stays high during sleep. Preserve released PWRKEY/reset levels and a
deliberate RTS state; do not drive modem output signals from the ESP.

S31-specific correction: the reviewed PMU configuration automatically holds HP
and LP pads when TOP powers down during light sleep. Do not add manual holds to
every output without checking that mechanism. Establish correct levels and pulls
before sleep, then verify them on the board. Ordinary sleep isolation settings
and TOP-powered-down sleep are different paths.

The reported VDD_EXT measurement of 1.6 V remains unexplained. Quectel specifies
1.71–1.89 V and a maximum output current of 4 mA, describing this output for
external pull-up circuitry. Verify its suitability and load when supplying U9.
Measure at the modem pin and translator supply during idle and activity.
TXS0108E's internal pull-ups can draw current when signals are low even without
output contention. Removing the UART wake penalty does not prove this separate
electrical issue is resolved. Do not disable U9 during sleep if MAIN_RI still
depends on it to reach the ESP.

### Periodic wakeups and clock calibration

The bundled esp-modem UART task calls `get_event(event, 100)` and checks buffered
RX data on timeout. This creates roughly ten task wakeups per second while idle.
Its stop handshake also depends on leaving that wait. An event-driven replacement
must wake explicitly for shutdown and preserve any required buffered-data
recovery; changing the timeout to infinite without that work can break teardown.

The active configuration has
`CONFIG_PM_LIGHTSLEEP_RTC_OSC_CAL_INTERVAL=1`. Trial 16 after obtaining a baseline,
then compare current and timing over repeated sleep cycles. Longer intervals
reduce calibration overhead but track oscillator drift less often. The fitted
32.768 kHz crystal is another candidate to evaluate: the current configuration
selects internal RC. Do not assume choosing the crystal removes all calibration,
because the sleep path also calibrates RC_FAST.

### Peripheral retention and memory

The active configuration already enables CPU power-down, flash deep-power-down
commands and PSRAM half-sleep. Digital peripheral power-down is disabled.

Before enabling `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP`, arrange UART
retention using `uart_config.flags.allow_pd`. The bundled esp-modem leaves this
flag zero and calls `uart_param_config()` before `uart_driver_install()`. Check
initialisation order against IDF's retention allocation requirements; setting
the flag alone is insufficient if the retention module is not yet initialised.
Check allocation warnings and actual power-domain behaviour. Other initialised
drivers can also prevent power-down or need retention support.

This option does not power off the whole board. LP wake logic and retained memory
remain powered; PSRAM must retain live application data. LCD, LEDs, translators
and regulators need their own appropriate power states. Avoid initialising unused
peripherals, but do not assume that skipping their drivers removes external loads.
For measurements, compare with USB disconnected and unnecessary console output
disabled. Restore diagnostics outside the measurement window.

### Modem sleep policy

Use `QSCLK=1` for the normal-sleep baseline. For PSM experiments, choose the TAU
interval deliberately, allow re-registration and confirm actual PSM entry.
Quectel states that `QSCLK?` reports requested/reference timer values; its note
directs users to QLog for network-assigned values. Do not repeatedly poll AT
commands while trying to measure an undisturbed sleep interval.

The reviewed setup does not issue `AT+ECSIMCFG="SimPowerSave",1`; check the modem's
actual setting rather than assuming a previous test still applies. For PSM with
DTR wake, evaluate disabling modem-side UART wake through `AT+QPSMUART=0` after
confirming firmware support and wake behaviour. This is distinct from ESP UART
wake. Keep modem USB conditions consistent with the Quectel low-power guidance.

After re-enabling PPP and periodic pings, measure a complete send-to-sleep cycle.
Radio activity and the time before the network releases the connection can dominate
average consumption. Batch traffic or lengthen reporting intervals where product
latency permits. A temporary `CFUN=0` measurement helps separate radio/network
activity from other loads, but removes normal cellular connectivity.

## LP UART: an option requiring a separate PoC

The reviewed S31 implementation has a 16-byte LP UART FIFO and FIFO-threshold,
edge-threshold, start-bit and character-sequence wake detectors. Signals can use
the LP GPIO matrix on GPIO0–GPIO7. Native TX/RX are GPIO6/7 and RTS/CTS are GPIO4/5;
the current modem GPIO42/43/46/47 cannot be reused as LP UART pins.

The local S31 capability definitions do not enable the direct LP UART branch of
`esp_sleep_enable_uart_wakeup()`. The supported path examined is LP UART waking
the LP core, which calls `ulp_lp_core_wakeup_main_processor()` with ULP wake enabled
on the main CPU. Standard LP startup clears the UART buffers when handling the
S31's persistent wake condition, so preserving the triggering modem bytes needs
additional work. A 16-byte FIFO and 921600-baud traffic also require careful flow
control and latency testing.

LP UART FIFO wake keeps a receiver clock running. Measure its actual S31 power
cost; no measured LP-UART-versus-RI result is available here. Do not treat it as a
drop-in, lossless replacement for HP UART wake.

## Validation sequence

1. Record firmware/configuration, supply point, USB connections and modem mode.
2. Capture the modem-off ESP baseline, then repeat with the modem in settled
   `QSCLK=1` sleep. Record quiet current and integrated average separately.
3. Remove the DTR pull-down; verify DTR and VDD_EXT electrically.
4. Compare ordinary light sleep with timer-woken peripheral-powered-down light
   sleep. Timer wake allows this test before MAIN_RI wiring is available, but does
   not provide asynchronous modem receive wake.
5. Compare UART polling and clock-calibration changes individually.
6. Test MAIN_RI wake repeatedly, including URCs, incoming PPP data, host TX,
   reconnects and modem power cycles. Check for missing bytes and wake loops.
7. Compare deep sleep separately, accounting for changed GPIO states, reboot and
   reinitialisation energy. Do not infer a component's current solely by subtracting
   two different sleep modes.
8. Reintroduce production traffic and peripherals incrementally and capture
   several full reporting cycles.

## Evidence and references

- Project: [modem manager](../../main/comm/modem_manager.cpp),
  [Quectel DTE](../../main/comm/quectel_dte.cpp),
  [bootstrap](../../main/bootstrap_fsm.cpp),
  [ping test](../../main/comm/ping_test.cpp).
- Local IDF root used for review: `/home/hu/esp/esp-idf`.
  Relevant files: `components/esp_driver_uart/src/uart_wakeup.c`,
  `components/esp_driver_uart/src/uart.c`,
  `components/esp_hw_support/port/esp32s31/pmu_sleep.c`,
  `components/esp_hw_support/port/esp32s31/private_include/pmu_param.h`,
  `components/esp_pm/Kconfig`, and
  `components/ulp/lp_core/lp_core/lp_core_utils.c`.
- [Espressif S31 hardware guidelines](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s31/schematic-checklist.html#gpio).
- [TI TXS0108E documentation](https://www.ti.com/product/TXS0108E).
- Quectel local documents in `/home/hu/Downloads/quectel`:
  `Quectel_EG800Z_Series_Low_Power_Mode_Application_Note_V1.0.0.pdf`
  (§§2.1.3–2.1.5, 3.2–3.3) and
  `Quectel_EG800Z_Series_Hardware_Design_V1.2.pdf`
  (VDD_EXT pin specification and §4.9.3 MAIN_RI).

Recheck S31 source and Espressif guidance when upgrading IDF. Capability flags,
retention implementation and LP UART startup behaviour may change.

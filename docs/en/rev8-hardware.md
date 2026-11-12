# Rev 8 hardware notes

Rev 8 splits the design into a main board and an adaptor board, joined by a
40-pin connector. The ESP32-S31 GPIO assignments match Rev 7.1, except that
GPIO2 is now the modem's `MAIN_RI`. The SWD/UART front end, LCD and power
profiler are on the adaptor.

Measurements below were taken on 2026-11-11 with the `esp_swd_demo` hardware
test (branch `feature/si-rev8`) and an STM32WLE5 target.

## Firmware status

Build with `-D SI_HW_REV=rev8`; see [getting started](getting-started.md). The
firmware does not support Rev 8 fully yet:

| Signal | GPIO | Status |
| --- | --- | --- |
| `DBG_EN` | 57 | Driven high during a programming run or debug session, low otherwise. |
| LCD power | 39 | Not driven. `SI_DISP_PANEL_PWR` is set but the firmware does not use it. |
| Power profiler | See below | Measured over SIDP's power service; see below for what is verified. |

The SWD and UART GPIOs are unchanged from Rev 7.1. The firmware still uses the
option named `ESP_SWD_PHY_AXC2T245`: Rev 8 fits SN74AVC2T245 translators, whose
pinout and OE/DIR logic match.

Rev 8 runs SWD at 13 MHz over the direct SPI2 backend, so the LCD uses SPI3.
Rev 7.1 keeps the LCD on SPI2 and SWD at 500 kHz.

A debug session reads the target's flash range and RAM regions from the active
job, so a Cortex-M job must be pushed before debugging. A debug session and a
programming run cannot happen at the same time: whichever starts second is
refused. See [Debugging with GDB](debugging.md).

## Programming connector wiring

Pins 9 and 11 were a single rail on Rev 7.1 and are separate on Rev 8:

| Pin | Net | Use |
| --- | --- | --- |
| 7 | `+3V3` | Adaptor 3.3 V output |
| 9 | `VPP_OUTPUT` | Measured supply to the target |
| 11 | `VPP_INPUT` | 1.8 to 3.6 V supply for the translators and profiler |

Connect pin 11 to pin 7 or to a bench supply, and power the target from pin 9.
Whatever drives `VPP_INPUT` powers the target as soon as it is applied, because
there is no target power switch. A Rev 7.1 harness that joins both pins to the
target's supply bypasses the profiler.

The translators need at least 1.2 V on `VPP_INPUT`, up from 0.65 V on Rev 7.1.

For an ESP32 target, set `reset_assert_level: high` in
[target.yaml](target-yaml.md), because a high GPIO drives the reset MOSFET on.

## Debug-line isolation

`DBG_EN` (GPIO57) connects SWD, reset, BOOT and UART to the programming
connector. It is active high and pulled low, so the lines are open until
firmware drives it. This allows current to be measured with the debug cable
still attached. Target detect is not switched.

## Power profiler

Three INA226 sensors sit on a series shunt chain between `VPP_INPUT` and
`VPP_OUTPUT`:

| Range | Current, nominal at 3.3 V | Shunt | I2C address | LSB |
| --- | --- | --- | --- | --- |
| HIGH | 8.8 mA to 1.64 A | 50 mΩ | 0x40 | 50 µA |
| MID | 216 µA to 8.8 mA | 4.02 Ω | 0x41 | 622 nA |
| LOW | Up to 216 µA | 162 Ω | 0x44 | 15.4 nA |

Two latches hold the range bypasses as a 2-bit shift register:

| Signal | GPIO |
| --- | --- |
| Shift data (D) | 19 |
| Shift clock | 37 |
| HIGH latch readback | 38 |
| MID latch readback | 5 |
| I2C SDA / SCL | 17 / 18 |
| Shared `/ALERT` | 4 |

- Comparators move the range up in hardware.
- Firmware moves it down. Set D low and clock once to step down one range. Set
  D high and clock twice to force both bypasses on.
- Trip currents scale with `VPP_INPUT`, so derive the down-range limits from the
  measured bus voltage.
- After a range change, wait for settling and discard the conversion that spans
  the switch.

The firmware (`main/power/power_profiler.cpp`) streams raw readings over SIDP
(`components/sidp_client/docs/sidp-power.md`); `sidp-agent power record`
writes them to CSV. While measuring, the active range's sensor converts shunt
voltage only, so no time is skipped, and an idle sensor converts `VPP_INPUT`.
All three bus inputs are on `VPP_INPUT`, so `VPP_OUTPUT` is computed on the
host from the range's series resistance. Between measurements both bypasses
stay on.

The sensors convert continuously, not triggered, so every conversion follows
the previous one and the current is integrated without dead time.

The I2C bus runs at 1 MHz with the ESP-IDF driver and no HS master code. That
is above the INA226's 400 kHz fast mode and the ESP32-S31's rated 800 kHz. The
sensors' ID and measurement readings matched those at 400 kHz on the board
tested. Measured on 2026-11-12 with the STM32WLE5 attached, over USB with
`sidp-agent power record`, 3 s each:

| Conversion | Samples per second | Missed at 400 kHz | Missed at 1 MHz |
| --- | --- | --- | --- |
| 1.1 ms | 892 (909 nominal) | None | None |
| 588 µs, the shortest allowed | 1668 (1700 nominal) | 6, each marked as a gap | None |
| 332 µs, not allowed | | | 1485: the sampler falls behind |

The firmware steps down when the average of 8 samples is below 85% of the next
range's lowest possible trip current. The running target (about 5.1 mA at
3.31 V, which HIGH reads anywhere from 4.15 to 6.0 mA) reached MID after its
first 8 samples in each of three runs and stayed there. With the earlier rule,
3 samples in a row below 70%, it stayed in HIGH for about 9 s.
`--lowest-range high` held HIGH.

## Measured results

SWD over the direct SPI2 backend, 100 of 100 iterations verified:

| Clock | Write | Read |
| --- | --- | --- |
| 500 kHz | 32.8 KB/s | 33.0 KB/s |
| 13.33 MHz | 251 KB/s | 262 KB/s |

Target current at 3.31 V:

| State | Average |
| --- | --- |
| Running its own firmware | About 1.4 mA |
| Halted, SWD idle | 610 µA |
| Halted, SWD writing | 694 µA |
| Halted, SWD reading | 767 µA |
| Stop 2, debug kept on | 556 µA |
| Standby, debug and RTC off | 6.73 µA, almost all measurement error |

The SWD and halted figures are at the reset clock (MSI, 4 MHz). Debug in Stop 2
keeps clocks running, so that figure is not a true Stop 2 current. Entry into
both Stop 2 and Standby was confirmed from the STM32's power status flags.

## Known limitations

**INA226 input bias.** With the output open, each INA226 shunt pin on these
boards draws about 6.6 µA, against 0.1 nA in the datasheet. All three report die
ID `0x2260`, the value that datasheet lists, so the die ID does not identify
low-bias parts. The LOW shunt carries one pin and reads about 6.6 µA. The MID
shunt carries three and reads about 19.6 µA. Powering down the LOW sensor
dropped the MID reading by 13.2 µA, against 13.3 µA predicted.

LOW range readings therefore include about 6.6 µA that is not target current, so
the STM32's real Standby current has not been measured. `sidp-agent power zero`
measures a zero per range with the output open and `power record --zero`
subtracts it, scaled with `VPP_INPUT`. Expect a few hundred nA at best with
these parts, because repeat readings varied from 6.49 to 6.93 µA.

**USB frames damaged at high rates.** Raw captures of the power stream showed
1 bad POWER_DATA frame in 1431 over 30 s at 588 µs, none at 1.1 ms, and a few
per second at 140 µs. Some lose about 512 bytes at the front (the TinyUSB CDC
TX buffer size); one kept its header but failed its CRC. sidp-agent drops such
frames, as SIDP requires, so the recording goes on with a gap, and `power
record` counts them: a 60 s recording at 588 µs had 2. The cause on the device
is not found.

**Open items for the power profiler** (as of 2026-11-12):

- Find why POWER_DATA frames are occasionally damaged on the USB send path
  (above). First experiment: move the transport's log ring buffer from PSRAM to
  internal RAM, then repeat a 30 s raw capture at 588 µs.
- Verify the step to LOW and `sidp-agent power zero` with `VPP_OUTPUT` open.
- Sample periods below 588 µs: the sampler task falls behind (three register
  reads per sample through the ESP-IDF I2C driver). Fewer or cheaper reads
  would be needed.
- HIGH range `vout` uses 50 mΩ and ignores the bypass FET's resistance, about
  1 mV of error at 20 mA.
- The desktop app measures power during debugging only while a USB connection
  is open: with *keep serving*, not between GDB sessions.

**Not yet verified:**

- The firmware's step to LOW, and `power zero`, which needs `VPP_OUTPUT` open.

- The HIGH comparator has not been tested with a real load above about 13 mA.
- With a target attached, a step to LOW sometimes sets both latches when only
  MID should trip. This happened in 4 of 6 runs at about 1.4 mA. The cause is
  not confirmed.
- LOW range noise with the output open was about ±3 µA before SWD init and about
  ±11 µA after. The source is not confirmed.

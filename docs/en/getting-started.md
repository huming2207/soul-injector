# Getting started

SoulInjector is an ESP32-S31 based offline programmer. It supports:

- SWD programming of ARM Cortex-M targets using a flash algorithm.
- UART programming of Espressif targets using the ROM bootloader and flasher
  stub.

It loads firmware and target configuration from the device storage partition
and runs the programming flow without needing a host PC.

## Hardware revision builds

Select the board revision when configuring or building the project:

```sh
idf.py --preview -B build-rev71 -D SI_HW_REV=rev71 build
```

The only supported value is `rev71`, which is also the default. It requires
ESP-IDF v6.1 or later; ESP32-S31 is still a preview target there. Each revision
uses its own build-directory `sdkconfig` so a future revision does not reuse
pin assignments from another board.

The Rev 7.1 configuration enables the split, SN74AXC2T245-translated SWD
interface and uses the GPIO assignments from the Rev 7.1 KiCad schematic. Its
NT279VJ-C10-01-V1 LCD uses the NV3007 panel driver and is enabled by default.

The ESP32-S3 boards (Rev 3, Rev 5 and Rev 6) are no longer built from this
branch. Their last firmware is the `legacy-s3-yaml` tag.

## Terminology

Unless otherwise specified:

- The **target** means the microcontroller being programmed: an ARM Cortex-M
  device over SWD, or an Espressif chip over UART.
- The **host** means the Soul Injector device itself.

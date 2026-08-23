# Getting started

SoulInjector is an ESP32-S3 based offline programmer. It supports:

- SWD programming of ARM Cortex-M targets using a flash algorithm.
- UART programming of Espressif targets using the ROM bootloader and flasher
  stub.

It loads firmware and target configuration from the device storage partition
and runs the programming flow without needing a host PC.

## Hardware revision builds

Select the board revision when configuring or building the project:

```sh
idf.py -B build-rev6 -D SI_HW_REV=rev6 build
```

Supported values are `rev3`, `rev5`, and `rev6`; `rev5` remains the default.
Each revision uses its own build-directory `sdkconfig` so changing revisions
does not reuse pin assignments from another board.

The Rev 6 configuration enables the split, SN74AXC2T245-translated SWD
interface and uses the GPIO assignments from the Rev 6 KiCad schematic. Its
NT279VJ-C10-01-V1 LCD uses the NV3007 panel driver and is enabled by default.

## Terminology

Unless otherwise specified:

- The **target** means the microcontroller being programmed: an ARM Cortex-M
  device over SWD, or an Espressif chip over UART.
- The **host** means the Soul Injector device itself.

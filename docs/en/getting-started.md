# Getting started

SoulInjector is an ESP32-S31 based offline programmer. It supports:

- SWD programming of ARM Cortex-M targets using a flash algorithm.
- UART programming of Espressif targets using the ROM bootloader and flasher
  stub.

A PC with `sidp-agent` pushes the programming job and its firmware images over
USB; after that the device programs targets on its own, on request or whenever
a target is plugged in. Every run is recorded in a production log that the PC
collects later.

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

ESP-IDF v6.1 ships FatFs with `f_expand()` disabled, which the production log
needs. Until Espressif releases upstream commit `74a7a4b`, set
`#define FF_USE_EXPAND 1` in `components/fatfs/src/ffconf.h` of your ESP-IDF
tree, or the build fails to link with `undefined reference to 'f_expand'`.

## Flash layout

`partitions.csv` assigns the 16 MB flash as follows:

- `factory`: 2 MB application.
- `nvs`: 256 KB settings (the job's trigger).
- `data`: 5 MB FAT, mounted at `/data`, holding the job and its images.
- `log`: 2 MB FAT, mounted at `/log`, holding the production log.

New partitions are added after the existing ones, so flashing a newer firmware
with `idf.py flash` keeps the job and images on `data`.

## Other boards

The ESP32-S3 boards (Rev 3, Rev 5 and Rev 6) are no longer built from this
branch. Their last firmware is the `legacy-s3-yaml` tag.

## Terminology

Unless otherwise specified:

- The **target** means the microcontroller being programmed: an ARM Cortex-M
  device over SWD, or an Espressif chip over UART.
- The **host** means the Soul Injector device itself.
- The **PC** means the computer running `sidp-agent`, connected to the Soul
  Injector over USB.

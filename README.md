# Soul Injector

Offline firmware downloader for ARM Cortex-M, on an ESP32-S3.

## To-do list - Major building blocks

- [ ] LittleVGL + LCD?
- [ ] SWD on SPI Master, instead of bit-banging
- [ ] "Unlimited" buffering (write firmware chunk to RAM cache while the target is busy committing cache to flash)
- [-] Better flash algorithm support (doing - plan to support [probe-rs's Rust-based flash algorithm](https://github.com/probe-rs/flash-algorithm-template))
- [x] Chip erase
- [x] Run flash algorithm (done in v0.0.1)
- [x] Offline firmware flashing (done in v0.0.1)
- [x] More (manual) tests (done in v1.0.0)
- [x] Read verification (done in v1.0.0)
- [x] Desktop app: see [huming2207/soul-composer-app](https://github.com/huming2207/soul-composer-app) (done in v1.0)
- [x] USB CDC-ACM communication (done in v1.0.0)

## To-do list - other platforms except Cortex-M & SWD

- [ ] ESP32 Bootloader UART firmware downloading 
- [ ] GD32VF103 RISC-V, over JTAG??
- [ ] CH32VF103 RISC-V, over SWD/cJTAG?

## Post-MVP feature list

- [ ] Basic BLE functionality
- [ ] Compression on BLE transmission
- [ ] OCD server over BLE?
- [ ] USB DAP-Link probe? (but why not buy one from Taobao instead?)
- [ ] Abandon CMSIS-Pack's flash algorithm, move to my own impl of flash algorithm (to get it even faster)

## License

This project (except 3rd-party libraries in `components` directory) has two types of licenses:
  - AGPL-3.0 for non-commercial purposes
  - Commercial licenses for any commercial purposes, speak to me (the author, Jackson Ming Hu, huming2207 at gmail.com) for more details.
  - "Commercial purposes" here means any of these scenarios below:
    - 1. Rebrand and/or resell this project's hardware, firmware and related software, with or without modification
    - 2. Make use of this project for any commercial mass production (i.e. using this tool to download/flash firmware for manufacturing products in a production line)
    - 3. Using this project as a repair/rework/experimental equipment in a repair shop as a service providing to others, or a lab in a company or university for R&D

# SoulInjector firmware programmer

SoulInjector is an ESP32-S3 based offline SWD programmer for ARM Cortex-M microcontroller. It
loads the firmware and target configuration from the device storage partition
and then runs the programming flow without needing a host PC.

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

In the documentation below, unless otherwise specified:
- The **target** means an ARM Cortex-M microcontroller like STM32, that is to be programmed. 
- The **host** means the Soul Injector device itself.

## Programming flow

As of v5.3, the host runs these states:

1. Load assets from `/data/target.yaml` and `/data/firmware.bin`.
2. Run `pre_prog.yaml` if available.
3. Detect the target over SWD.
4. Erase the target flash.
5. Program `/data/firmware.bin`.
6. Verify the programmed firmware through SWD.
7. Run self tests from `target.yaml`.
8. Run `post_prog.yaml` if available.
9. Finish, or run the optional production-rig current test when enabled.

## Maintenance

The Soul Injector has a USB port. If you connect it to a computer, it will show up as a USB Mass Storage Class (MSC) device.

You may alter the files as described below.

Please note that, due to the limitations on ESP32-S3, the USB MSC is very slow. If any of these files mentioned below was altered in any way, **ALWAYS MAKE SURE** that you unmount/eject the soul injector before you unplug from your computer!!!  

### Files on device storage

The Soul Injector uses these files **at the root directory of the USB MSC partition**:

- `firmware.bin`: firmware image to write to the STM32.
- `target.yaml`: required target description and flash algorithm.
- `firmware.bin.sha256`: optional `sha256sum` output for `firmware.bin`.
- `target.yaml.sha256`: optional `sha256sum` output for `target.yaml`.
- `pre_prog.yaml`: optional SWD procedure to run before programming.
- `post_prog.yaml`: optional SWD procedure to run after self tests.

These files will be mounted to a mountpoint called `/data` internally. You may see the logs mentioning path started with `/data` (e.g. `/data/firmware.bin`).

If a `.sha256` file exists, the matching file is checked once when assets are
loaded. If the `.sha256` file is absent, that check is skipped.

It is recommended to have a `.sha256` file in place to avoid flash corruption. 

### Target YAML

`target.yaml` describes what target is being
programmed and how the flash algorithm should be called.

The file must contain:

- `variants`: target variants. If there is only one variant it is selected
  automatically. If there are multiple variants, the caller must provide a
  variant name.
- `flash_algorithms`: flash algorithm definitions. The selected variant names
  the algorithm to use.

The selected flash algorithm provides:

- `instructions`: base64 encoded flash algorithm binary.
- `load_address`: where the algorithm is loaded in target RAM.
- `pc_init`, `pc_uninit`, `pc_program_page`, `pc_erase_sector`,
  `pc_erase_all`, `pc_verify`: function offsets. The firmware adds these
  offsets to `load_address`.
- `data_section_offset`: raw data section offset.
- `flash_properties`: flash address range, page size, erased byte value, and
  operation timeouts.

The selected variant can also contain a `memory_map`. RAM entries tagged as
`!Ram` are used to calculate the target RAM range available for the flash
algorithm.

An optional top-level `self_tests` list can define functions to run after
programming and firmware verify. Each item can contain `type`, `addr`, and
`name`. Currently simple internal tests are executed; unsupported test types are
logged and skipped.

### Pre and post programming step files

`pre_prog.yaml` and `post_prog.yaml` are loaded by `procedure_executor`.

Each file has this shape:

```yaml
steps:
  - type: WRITE_32
    addr: 0x40000000
    data: 0x00000001
```

Supported step types:

- `READ_32`: read one 32-bit word from `addr`.
- `WRITE_32`: write `data` to `addr`.
- `READ_MOD_WRITE_32`: read `addr`, apply `mask`, then OR in `data`.
- `POLL_32`: poll `addr` until `(value & mask) == expected` or `timeout_ms`
  expires.
- `DELAY_MS`: delay for `delay_ms`.
- `SWD_REINIT`: reinitialize SWD debug access.
- `SWD_RESET_TARGET`: toggle target reset.
- `SWD_HALT_TARGET`: halt the target CPU.
- `SWD_WAIT_HALT`: wait until the target is halted.

Each step can set `ignore_error: true`. If that step fails, execution continues.
Otherwise, the first failed step stops the procedure.

`pre_prog.yaml` and `post_prog.yaml` are optional. If it is missing or invalid, programming continues
with target detection. If it exists and execution fails, programming stops.

## License

This project is source-available for non-commercial use only under the terms in
[`LICENSE.md`](LICENSE.md).

Commercial use requires a separate written commercial license from the copyright
holder. Commercial use includes, but is not limited to, manufacturing or selling
devices, contract manufacturing, paid programming services, resale,
white-labelling, or use in a commercial production line.

The SoulInjector name, logos, product names, and branding are not licensed for
use. You may not use them to market, sell, or imply endorsement of derived
hardware, firmware, software, or services.

### Separate permission for Smart Guide Pty Ltd

The copyright holder grants Smart Guide Pty Ltd a perpetual, worldwide,
royalty-free permission to use, copy, modify, build, run, deploy, and internally
distribute SoulInjector software and firmware for Smart Guide Pty Ltd's own
internal business purposes. This permission applies to past, current, and future
editions or releases made available by the copyright holder.

This permission is granted only to Smart Guide Pty Ltd as a legal entity. It
does not extend to directors, employees, contractors, colleagues, sole traders,
related entities, personal businesses, customers, suppliers, or other third
parties except when they are acting solely on behalf of Smart Guide Pty Ltd and
only for Smart Guide Pty Ltd's internal business purposes.

This permission does not allow sublicensing, resale, white-labelling, public
redistribution, manufacturing or sales for third parties, or use by any separate
business without separate written permission from the copyright holder.

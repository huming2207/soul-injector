# target.yaml Reference

`target.yaml` is the target description file. The firmware reads it from
`/data/target.yaml` every time a target is connected. If
`/data/target.yaml.sha256` exists, `target.yaml` is verified against it before
parsing.

The file describes one of two programming families:

- `cortex-m` — ARM Cortex-M targets programmed over SWD with a flash algorithm.
- `esp32` — Espressif targets programmed over UART with the ROM bootloader and
  flasher stub.

## Top-level keys

| Key              | Required | Description |
| ---------------- | -------- | ----------- |
| `family`         | no       | `cortex-m` (default), `swd`, or `arm` select the SWD backend. `esp32`, `esp32-serial`, or `espressif` select the ESP32 UART backend. |
| `variants`       | yes      | Non-empty sequence of target variants. |
| `flash_algorithms` | only for `cortex-m` | Sequence of flash algorithm definitions. |
| `self_tests`     | no       | Sequence of self-test items run after verify. |

## Variants

`variants` is a sequence of maps. Each variant may have a `name`.

- If exactly one variant exists, it is selected automatically.
- If more than one variant exists, the caller must provide the variant name;
  otherwise parsing fails.

Variant-specific fields depend on the selected `family`.

## Family: cortex-m (SWD)

The selected variant must contain:

| Key               | Required | Description |
| ----------------- | -------- | ----------- |
| `flash_algorithms` | yes     | Sequence of algorithm names. Only the first entry is used. |

The selected variant may contain:

| Key          | Description |
| ------------ | ----------- |
| `name`       | Variant name (copied into the parsed config for logging). |
| `memory_map` | Target memory map. Only entries tagged `!Ram` are used; they define the RAM regions available for the flash algorithm. Up to 4 `!Ram` regions are supported. |

### Root flash_algorithms

The root `flash_algorithms` sequence contains the algorithm definitions. The
algorithm named by the variant's first `flash_algorithms` entry is used.

Each algorithm entry supports:

| Key                   | Required | Description |
| --------------------- | -------- | ----------- |
| `name`                | yes      | Algorithm name, matched against the variant's `flash_algorithms` entry. |
| `instructions`        | yes      | Base64-encoded flash algorithm binary. |
| `load_address`        | yes      | Address in target RAM where the algorithm is loaded. |
| `pc_init`             | no       | Offset from `load_address` of the init entry point. |
| `pc_uninit`           | no       | Offset from `load_address` of the uninit entry point. |
| `pc_program_page`     | no       | Offset from `load_address` of the program-page entry point. |
| `pc_erase_sector`     | no       | Offset from `load_address` of the erase-sector entry point. |
| `pc_erase_all`        | no       | Offset from `load_address` of the erase-all entry point. |
| `pc_verify`           | no       | Offset from `load_address` of the verify entry point. |
| `data_section_offset` | no       | Static base address used by the algorithm syscall. |
| `flash_properties`    | no       | Flash geometry and timing (see below). |

The `pc_*` values are offsets. The firmware adds them to `load_address` to
compute the absolute entry-point addresses.

### flash_properties

| Key                      | Required | Description |
| ------------------------ | -------- | ----------- |
| `address_range`          | no       | Map with `start` and `end`. Defines the flash address range. Erase and program validation use this when present. |
| `page_size`              | yes      | Flash page size in bytes. Must be non-zero. |
| `erased_byte_value`      | no       | Value of an erased byte, normally `0xff`. |
| `program_page_timeout`   | no       | Program-page timeout in milliseconds. |
| `erase_sector_timeout`   | no       | Erase-sector timeout in milliseconds. |

Additional `flash_properties` keys, such as `sectors`, are currently not parsed
by the firmware.

### memory_map and RAM regions

`memory_map` is a sequence of tagged maps, for example:

```yaml
memory_map:
  - !Nvm
    name: BANK_1
    range: { start: 0x8000000, end: 0x8040000 }
  - !Ram
    name: SRAM1
    range: { start: 0x20000000, end: 0x20008000 }
```

Only entries tagged `!Ram` are used. Each `!Ram` entry must contain a `range`
with `start` and `end`, and `end` must be greater than `start`. The largest RAM
region and the region containing the flash algorithm are computed from these
entries.

## Family: esp32 (UART)

When `family: esp32` is set, the selected variant describes the Espressif chip
and the images to flash.

| Key             | Required | Description |
| --------------- | -------- | ----------- |
| `chip`          | yes      | Target chip name. Must match the chip reported by the target at connect time. Supported names: `esp8266`, `esp32`, `esp32s2`, `esp32s3`, `esp32c2`, `esp32c3`, `esp32c5`, `esp32c6`, `esp32h2`, `esp32p4`, `esp32c61`, `esp32s31`. |
| `images`        | yes      | Non-empty sequence of images to flash. Maximum 8 entries. |
| `flash_size_kb` | no       | Flash size in KiB (1–262144). When omitted, the host detects the flash size from the target. When present, the smaller of the configured and detected sizes is used; if detection fails, the configured size is used. |
| `baud`          | no       | Programming baud rate. Defaults to `115200`. Values above `115200` are applied after the flasher stub is running. |

The optional top-level `control_pins` map describes the electrical levels of
the programming-header control signals. It is board-level wiring configuration:

| Key                   | Required | Values        | Default | Description |
| --------------------- | -------- | ------------- | ------- | ----------- |
| `reset_assert_level`  | no       | `low`, `high` | `low`   | Host GPIO level that asserts target reset. Use `high` for Rev 6, where a high GPIO drives the reset MOSFET on. |
| `boot_assert_level`   | no       | `low`, `high` | `low`   | Host GPIO level that puts the target into ROM download mode. Espressif chips normally require `low`. |

Each `images` entry:

| Key      | Required | Description |
| -------- | -------- | ----------- |
| `path`   | yes      | File path, for example `/data/bootloader.bin`. Maximum 63 characters. |
| `offset` | yes      | Flash offset. Must be 4-byte aligned. Images must not overlap in the same erase sector. |

Example (Rev 6 reset polarity):

```yaml
family: esp32
control_pins:
  reset_assert_level: high   # Rev 6: high GPIO asserts target reset
  boot_assert_level: low     # default, can be omitted
variants:
  - name: esp32s31
    chip: esp32s31
    flash_size_kb: 4096
    baud: 921600
    images:
      - { path: /data/bootloader.bin, offset: 0x0 }
      - { path: /data/partitions.bin, offset: 0x8000 }
      - { path: /data/ota_data.bin, offset: 0xe000 }
      - { path: /data/firmware.bin, offset: 0x10000 }
```

Image handling notes:

- Each image is padded to a 4-byte boundary with `0xFF` before flashing.
- The flasher stub MD5-verifies each image on the target as part of writing.
- After writing, every image is read back and compared byte-for-byte.
- Pre/post procedure register steps (`READ_32`, `WRITE_32`,
  `READ_MOD_WRITE_32`, `POLL_32`) are supported by the ESP32 backend after
  target detection. `SWD_HALT_TARGET`, `SWD_WAIT_HALT`, and `SWD_REINIT` are
  not supported and will fail the procedure unless `ignore_error: true` is
  set.

## self_tests

Optional top-level sequence, for example:

```yaml
self_tests:
  - name: LoRa send
    type: simple
    addr: 0xfffff006
```

| Key    | Required | Description |
| ------ | -------- | ----------- |
| `addr` | yes      | Test entry address. |
| `name` | no       | Human-readable test name (maximum 31 characters). |
| `type` | no       | `simple` (default), `extend`, or `power`. Unknown values default to `simple`. Only `simple` is currently executed; `extend` and `power` are skipped. |

Up to 16 self-test items are supported. The ESP32 backend reports self tests as
unsupported; the FSM then skips the remaining tests and continues with the
post-program procedure.

## Asset verification

For every file referenced by the parsed config (`target.yaml`, cortex-m
`firmware.bin`, or each ESP32 image), the firmware looks for a sidecar file
named `<file>.sha256`. If the sidecar exists, the file content must match the
SHA-256 digest in the sidecar. If the sidecar is absent, the check is skipped.

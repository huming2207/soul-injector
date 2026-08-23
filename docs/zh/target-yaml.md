# target.yaml 参考

`target.yaml` 是目标描述文件。固件在每次目标接入时从 `/data/target.yaml` 读取。
如果存在 `/data/target.yaml.sha256`，解析前会先校验 `target.yaml` 的 SHA-256。

该文件描述两种编程家族之一：

- `cortex-m` — 通过 SWD 和 flash algorithm 编程 ARM Cortex-M 目标。
- `esp32` — 通过 UART、ROM bootloader 和 flasher stub 编程 Espressif 目标。

## 顶层键

| 键               | 是否必需 | 说明 |
| ---------------- | -------- | ---- |
| `family`         | 否       | `cortex-m`（默认）、`swd` 或 `arm` 选择 SWD 后端。`esp32`、`esp32-serial` 或 `espressif` 选择 ESP32 UART 后端。 |
| `variants`       | 是       | 非空的目标 variant 序列。 |
| `flash_algorithms` | 仅 `cortex-m` 需要 | flash algorithm 定义序列。 |
| `self_tests`     | 否       | 编程验证后执行的自检项序列。 |

## Variants

`variants` 是一个 map 序列。每个 variant 可以有一个 `name`。

- 只有一个 variant 时，自动选择该 variant。
- 有多个 variant 时，调用方必须提供 variant 名称，否则解析失败。

variant 的具体字段取决于所选 `family`。

## 家族：cortex-m（SWD）

所选 variant 必须包含：

| 键               | 是否必需 | 说明 |
| ---------------- | -------- | ---- |
| `flash_algorithms` | 是     | 算法名称序列。只使用第一项。 |

所选 variant 可以包含：

| 键          | 说明 |
| ------------ | ---- |
| `name`       | variant 名称（写入解析后的配置，用于日志）。 |
| `memory_map` | 目标内存映射。只使用带 `!Ram` 标签的条目；它们定义了 flash algorithm 可用的 RAM 区域。最多支持 4 个 `!Ram` 区域。 |

### 根 flash_algorithms

根 `flash_algorithms` 序列包含算法定义。固件使用 variant 的第一个
`flash_algorithms` 条目所指名称的算法。

每个算法条目支持：

| 键                   | 是否必需 | 说明 |
| --------------------- | -------- | ---- |
| `name`                | 是       | 算法名称，与 variant 的 `flash_algorithms` 条目匹配。 |
| `instructions`        | 是       | Base64 编码的 flash algorithm 二进制。 |
| `load_address`        | 是       | 算法加载到目标 RAM 的地址。 |
| `pc_init`             | 否       | init 入口相对 `load_address` 的偏移。 |
| `pc_uninit`           | 否       | uninit 入口相对 `load_address` 的偏移。 |
| `pc_program_page`     | 否       | program-page 入口相对 `load_address` 的偏移。 |
| `pc_erase_sector`     | 否       | erase-sector 入口相对 `load_address` 的偏移。 |
| `pc_erase_all`        | 否       | erase-all 入口相对 `load_address` 的偏移。 |
| `pc_verify`           | 否       | verify 入口相对 `load_address` 的偏移。 |
| `data_section_offset` | 否       | 算法 syscall 使用的静态基地址。 |
| `flash_properties`    | 否       | Flash 几何与时序（见下）。 |

`pc_*` 值都是偏移量。固件会把它们加到 `load_address` 上得到绝对入口地址。

### flash_properties

| 键                      | 是否必需 | 说明 |
| ------------------------ | -------- | ---- |
| `address_range`          | 否       | 包含 `start` 和 `end` 的 map，定义 flash 地址范围。擦除和编程校验在存在时使用它。 |
| `page_size`              | 是       | Flash 页大小，单位字节，必须非零。 |
| `erased_byte_value`      | 否       | 擦除后的字节值，通常为 `0xff`。 |
| `program_page_timeout`   | 否       | 编程一页超时时间，单位毫秒。 |
| `erase_sector_timeout`   | 否       | 擦除一个扇区超时时间，单位毫秒。 |

`flash_properties` 中的其他键（如 `sectors`）当前固件不解析。

### memory_map 与 RAM 区域

`memory_map` 是带标签的 map 序列，例如：

```yaml
memory_map:
  - !Nvm
    name: BANK_1
    range: { start: 0x8000000, end: 0x8040000 }
  - !Ram
    name: SRAM1
    range: { start: 0x20000000, end: 0x20008000 }
```

只使用 `!Ram` 条目。每个 `!Ram` 条目必须包含带 `start` 和 `end` 的 `range`，
且 `end` 必须大于 `start`。固件会从这些条目中计算最大 RAM 区域和承载
flash algorithm 的区域。

## 家族：esp32（UART）

当 `family: esp32` 时，所选 variant 描述 Espressif 芯片和要烧录的镜像。

| 键             | 是否必需 | 说明 |
| --------------- | -------- | ---- |
| `chip`          | 是       | 目标芯片名。必须与连接时目标上报的芯片一致。支持：`esp8266`、`esp32`、`esp32s2`、`esp32s3`、`esp32c2`、`esp32c3`、`esp32c5`、`esp32c6`、`esp32h2`、`esp32p4`、`esp32c61`、`esp32s31`。 |
| `images`        | 是       | 非空的镜像序列，最多 8 项。 |
| `flash_size_kb` | 否       | Flash 大小，单位 KiB（1–262144）。省略时主机从目标检测 flash 大小。提供时取配置值与检测值中较小者；检测失败时使用配置值。 |
| `baud`          | 否       | 编程波特率，默认 `115200`。高于 `115200` 时在 flasher stub 运行后切换。 |

可选的顶层 `control_pins` map 描述编程排针控制信号的电平约定，属于板级接线配置：

| 键                   | 是否必需 | 可选值        | 默认 | 说明 |
| --------------------- | -------- | ------------- | ---- | ---- |
| `reset_assert_level`  | 否       | `low`、`high` | `low` | 主机 GPIO 输出什么电平时目标复位有效。Rev 6 上高电平驱动复位 MOSFET 导通，因此用 `high`。 |
| `boot_assert_level`   | 否       | `low`、`high` | `low` | 主机 GPIO 输出什么电平时目标进入 ROM 下载模式。Espressif 芯片通常需要 `low`。 |

每个 `images` 条目：

| 键      | 是否必需 | 说明 |
| -------- | -------- | ---- |
| `path`   | 是       | 文件路径，例如 `/data/bootloader.bin`。最长 63 字符。 |
| `offset` | 是       | Flash 偏移地址。必须 4 字节对齐。多个镜像不能落在同一个擦除扇区内。 |

示例（Rev 6 复位极性）：

```yaml
family: esp32
control_pins:
  reset_assert_level: high   # Rev 6：高电平复位有效
  boot_assert_level: low     # 默认值，可省略
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

镜像处理说明：

- 每个镜像在烧录前会补齐到 4 字节边界，填充值为 `0xFF`。
- 写入过程中，flasher stub 会在目标端对每个镜像做 MD5 校验。
- 写入完成后，每个镜像都会被读回并逐字节比较。
- ESP32 后端在目标连接建立后支持 `READ_32`、`WRITE_32`、`READ_MOD_WRITE_32`、
  `POLL_32` 等寄存器步骤。`SWD_HALT_TARGET`、`SWD_WAIT_HALT`、`SWD_REINIT`
  仍不支持，除非步骤设置了 `ignore_error: true`，否则流程会失败。

## self_tests

可选的顶层序列，例如：

```yaml
self_tests:
  - name: LoRa send
    type: simple
    addr: 0xfffff006
```

| 键    | 是否必需 | 说明 |
| ------ | -------- | ---- |
| `addr` | 是       | 测试入口地址。 |
| `name` | 否       | 可读的测试名称（最长 31 字符）。 |
| `type` | 否       | `simple`（默认）、`extend` 或 `power`。未知值默认按 `simple` 处理。当前只执行 `simple`；`extend` 和 `power` 会被跳过。 |

最多支持 16 个自检项。ESP32 后端会报告自检不支持；FSM 会跳过剩余自检并继续
执行 post-program 流程。

## 资产校验

对于解析后配置引用的每个文件（`target.yaml`、cortex-m 的 `firmware.bin`，或
每个 ESP32 镜像），固件会寻找名为 `<file>.sha256` 的伴生文件。伴生文件存在
时，文件内容必须与伴生文件中的 SHA-256 摘要一致；伴生文件不存在时跳过校验。

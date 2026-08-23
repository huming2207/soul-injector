# pre_prog.yaml / post_prog.yaml 步骤文件参考

本文档描述 Soul Injector 编程前步骤文件 `/data/pre_prog.yaml` 与编程后步骤文件 `/data/post_prog.yaml` 的 YAML 格式与执行语义。

## 文件位置与加载行为

| 项目 | 说明 |
| ---- | ---- |
| 文件路径 | `/data/pre_prog.yaml`、`/data/post_prog.yaml` |
| 是否必需 | 两者均为可选文件 |
| 文件缺失 | 编程流程继续，直接进入目标检测（`pre_prog.yaml`）或完成收尾（`post_prog.yaml`） |
| YAML 非法或解析失败 | 编程流程继续，日志会记录跳过原因 |
| 文件存在且执行失败 | 编程流程停止，进入错误状态 |

> 注意：这里的“解析失败”指 YAML 无法解析或步骤定义非法；而文件解析成功后，若某个步骤执行失败，则按下面的 `ignore_error` 规则处理。

## 顶层结构

每个步骤文件必须包含 `steps` 键，且 `steps` 必须是非空序列：

```yaml
steps:
  - type: DELAY_MS
    delay_ms: 100
```

约束：

- `steps` 键必须存在，否则文件解析失败。
- `steps` 必须是 YAML 序列。
- `steps` 不能为空列表。空列表会在执行阶段失败，请不要使用空列表。
- 当前实现最多支持 **96 个步骤**；超过该数量时文件解析失败。

## 步骤通用字段

每个步骤都支持以下通用字段：

| 字段 | 是否必需 | 类型 | 说明 |
| ---- | -------- | ---- | ---- |
| `type` | 必需 | 字符串 | 步骤类型，取值见下文“步骤类型与字段” |
| `ignore_error` | 可选 | 布尔 | 默认 `false`。设为 `true` 时，该步骤失败后记录警告并继续执行后续步骤；为 `false` 时，该步骤失败会立即中止整个步骤文件 |

布尔值支持 `true` / `false` / `1` / `0` / `yes` / `no`。

## 步骤类型与字段

### READ_32

读取目标 32 位内存字。

| 字段 | 必需 | 说明 |
| ---- | ---- | ---- |
| `addr` | 是 | 要读取的目标地址 |

```yaml
- type: READ_32
  addr: 0x40000000
```

### WRITE_32

向目标地址写入 32 位数据。

| 字段 | 必需 | 说明 |
| ---- | ---- | ---- |
| `addr` | 是 | 目标地址 |
| `data` | 是 | 要写入的 32 位数据 |

```yaml
- type: WRITE_32
  addr: 0x58004008
  data: 0x45670123
```

### READ_MOD_WRITE_32

读-改-写目标 32 位内存字。语义为：

```text
写入值 = (读取值 & mask) | data
```

| 字段 | 必需 | 说明 |
| ---- | ---- | ---- |
| `addr` | 是 | 目标地址 |
| `mask` | 是 | 按位与掩码 |
| `data` | 是 | 按位或数据 |

```yaml
- type: READ_MOD_WRITE_32
  addr: 0x58004014
  mask: 0xFFFDFFFF
  data: 0x00020000
```

### POLL_32

轮询目标 32 位内存字，直到满足 `(value & mask) == expected`，或超过 `timeout_ms`。

| 字段 | 必需 | 说明 |
| ---- | ---- | ---- |
| `addr` | 是 | 要轮询的目标地址 |
| `mask` | 是 | 按位与掩码 |
| `expected` | 是 | 期望的掩码结果 |
| `timeout_ms` | 是 | 超时时间，单位毫秒 |

超时后返回 `ESP_ERR_TIMEOUT`，步骤按失败处理。

```yaml
- type: POLL_32
  addr: 0x58004010
  mask: 0x00010000
  expected: 0x00000000
  timeout_ms: 1000
```

### DELAY_MS

延时指定毫秒数。

| 字段 | 必需 | 说明 |
| ---- | ---- | ---- |
| `delay_ms` | 是 | 延时时间，单位毫秒 |

```yaml
- type: DELAY_MS
  delay_ms: 500
```

### SWD_REINIT

重新初始化 SWD 调试连接。无额外字段。

```yaml
- type: SWD_REINIT
```

### SWD_RESET_TARGET

翻转目标复位线。无额外字段。

```yaml
- type: SWD_RESET_TARGET
```

### SWD_HALT_TARGET

暂停目标 CPU。无额外字段。

```yaml
- type: SWD_HALT_TARGET
```

### SWD_WAIT_HALT

等待目标 CPU 进入暂停状态。无额外字段。

```yaml
- type: SWD_WAIT_HALT
```

## 历史兼容步骤

以下步骤类型为历史遗留类型，仍可解析，但会被转换为无操作步骤并跳过，不会执行任何读写：

| 类型 | 当前行为 |
| ---- | -------- |
| `READ_BLOB` | 解析为无操作步骤，跳过 |
| `WRITE_BLOB` | 解析为无操作步骤，跳过 |

```yaml
# 这两个步骤解析成功，但执行时不会进行任何操作
- type: READ_BLOB
- type: WRITE_BLOB
```

## 目标家族兼容性

步骤是否可执行取决于 `target.yaml` 中选择的编程后端。

### ESP32（UART 串口）后端

ESP32 后端支持：

- `DELAY_MS`
- `SWD_RESET_TARGET`
- `READ_32`
- `WRITE_32`
- `READ_MOD_WRITE_32`
- `POLL_32`

其中 `READ_32`、`WRITE_32`、`READ_MOD_WRITE_32`、`POLL_32` 通过 loader 寄存器
协议实现，因此只有在目标连接建立（`detect`）之后才可用。若在 `pre_prog.yaml`
中使用，因为该阶段还未连接目标，会返回 `ESP_ERR_INVALID_STATE` 并失败。

ESP32 后端不支持以下步骤类型；执行时会返回 `ESP_ERR_NOT_SUPPORTED`，除非该步骤设置了 `ignore_error: true`，否则会导致整个步骤文件失败：

- `SWD_REINIT`
- `SWD_HALT_TARGET`
- `SWD_WAIT_HALT`

```yaml
# ESP32 目标示例：复位步骤可用，寄存器步骤必须忽略错误或直接删除
steps:
  - type: SWD_RESET_TARGET
  - type: DELAY_MS
    delay_ms: 200
  - type: WRITE_32
    ignore_error: true   # ESP32 后端会跳过该错误并继续
    addr: 0x58004008
    data: 0x45670123
```

### SWD（Cortex-M）后端

SWD 后端支持本文列出的全部步骤类型，包括历史兼容的无操作步骤。

## 完整示例

下面是一个完整的 `pre_prog.yaml` 示例：

```yaml
# /data/pre_prog.yaml
steps:
  # 暂停目标 CPU
  - type: SWD_HALT_TARGET

  # 解锁 Flash 内存接口（STM32 FLASH_KEYR）
  - type: WRITE_32
    addr: 0x58004008
    data: 0x45670123

  - type: WRITE_32
    addr: 0x58004008
    data: 0xCDEF89AB

  # 等待 Flash 解锁完成（FLASH_CR LOCK 位清零）
  - type: POLL_32
    addr: 0x58004014
    mask: 0x80000000
    expected: 0x00000000
    timeout_ms: 100

  # 触发 Option Byte 装载（会复位目标）
  - type: READ_MOD_WRITE_32
    ignore_error: true
    addr: 0x58004014
    mask: 0xF7FFFFFF
    data: 0x08000000

  # 等待复位完成
  - type: DELAY_MS
    delay_ms: 500

  # 重新初始化 SWD 连接
  - type: SWD_REINIT
```

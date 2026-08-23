# Pre/Post Programming Procedure YAML

`pre_prog.yaml` and `post_prog.yaml` describe optional step sequences that the
Soul Injector executes around the main programming flow.

| File               | Mounted path           | When it runs |
| ------------------ | ---------------------- | ------------ |
| `pre_prog.yaml`    | `/data/pre_prog.yaml`  | After assets are loaded, before target detection. |
| `post_prog.yaml`   | `/data/post_prog.yaml` | After self tests, before the flow finishes. |

Both files are optional:

- If the file is missing or cannot be parsed, the procedure is skipped. For
  `pre_prog.yaml`, programming continues to target detection. For
  `post_prog.yaml`, the flow continues to completion.
- If the file is present and a step fails, programming stops unless the step
  has `ignore_error: true`.

## Top-level structure

A procedure file must contain a top-level `steps` key whose value is a
sequence of step maps:

```yaml
steps:
  - type: DELAY_MS
    delay_ms: 100
```

- `steps` is required.
- `steps` must be a non-empty sequence.
- The current implementation supports up to 96 steps.
- Do not use an empty `steps` list: it parses successfully but currently
  fails when execution starts.

## Common step fields

Every step supports these fields:

| Field          | Required | Description |
| -------------- | -------- | ----------- |
| `type`         | yes      | Step type, for example `WRITE_32`. |
| `ignore_error` | no       | Boolean, default `false`. Accepted values: `true`, `false`, `1`, `0`, `yes`, `no`. |

When `ignore_error: true`, a failing step is logged and skipped, and execution
continues with the next step. When `ignore_error` is false or absent, the
first failed step aborts the whole procedure.

## Step types

### READ_32

Reads one 32-bit word from the target.

```yaml
- type: READ_32
  addr: 0x58004010
```

| Field  | Required | Description |
| ------ | -------- | ----------- |
| `addr` | yes      | Address to read. |

### WRITE_32

Writes one 32-bit word to the target.

```yaml
- type: WRITE_32
  addr: 0x58004008
  data: 0x45670123
```

| Field  | Required | Description |
| ------ | -------- | ----------- |
| `addr` | yes      | Address to write. |
| `data` | yes      | Value to write. |

### READ_MOD_WRITE_32

Reads `addr`, applies the AND mask, then ORs in the data value:

```text
value = (read_value & mask) | data
```

```yaml
- type: READ_MOD_WRITE_32
  addr: 0x58004014
  mask: 0xFFFDFFFF
  data: 0x00020000
```

| Field  | Required | Description |
| ------ | -------- | ----------- |
| `addr` | yes      | Address to modify. |
| `mask` | yes      | AND mask applied to the read value. |
| `data` | yes      | Value ORed into the masked result. |

### POLL_32

Polls `addr` until `(value & mask) == expected`, or until `timeout_ms`
expires.

```yaml
- type: POLL_32
  addr: 0x58004010
  mask: 0x00010000
  expected: 0x00000000
  timeout_ms: 1000
```

| Field         | Required | Description |
| ------------- | -------- | ----------- |
| `addr`        | yes      | Address to poll. |
| `mask`        | yes      | AND mask applied to each read value. |
| `expected`    | yes      | Expected masked value. |
| `timeout_ms`  | yes      | Poll timeout in milliseconds. Expiry returns `ESP_ERR_TIMEOUT` and fails the step. |

### DELAY_MS

Waits for the given number of milliseconds.

```yaml
- type: DELAY_MS
  delay_ms: 500
```

| Field       | Required | Description |
| ----------- | -------- | ----------- |
| `delay_ms`  | yes      | Delay in milliseconds. `0` is valid and returns immediately. |

### SWD_REINIT

Reinitializes the debug connection. No extra fields.

```yaml
- type: SWD_REINIT
```

### SWD_RESET_TARGET

Toggles the target reset line. No extra fields.

```yaml
- type: SWD_RESET_TARGET
```

### SWD_HALT_TARGET

Halts the target CPU. No extra fields.

```yaml
- type: SWD_HALT_TARGET
```

### SWD_WAIT_HALT

Waits until the target CPU is halted. No extra fields.

```yaml
- type: SWD_WAIT_HALT
```

## Legacy compatibility

`READ_BLOB` and `WRITE_BLOB` are recognized for compatibility with older
procedure files. They are parsed as no-op steps and skipped at run time.

## Target family compatibility

Step support depends on the selected programming family.

The ESP32 UART backend supports:

- `DELAY_MS`
- `SWD_RESET_TARGET`
- `READ_32`
- `WRITE_32`
- `READ_MOD_WRITE_32`
- `POLL_32`

`READ_32`, `WRITE_32`, `READ_MOD_WRITE_32`, and `POLL_32` are implemented
with the loader register protocol, so they are only available after the
target has been connected (`detect`). They fail with `ESP_ERR_INVALID_STATE`
if used in `pre_prog.yaml`, which runs before target detection.

The ESP32 backend does **not** support:

- `SWD_REINIT`
- `SWD_HALT_TARGET`
- `SWD_WAIT_HALT`

Unsupported steps return `ESP_ERR_NOT_SUPPORTED`. They fail the procedure
unless `ignore_error: true` is set on the step.

## Complete example

```yaml
steps:
  - type: SWD_RESET_TARGET

  - type: DELAY_MS
    delay_ms: 250

  - type: SWD_HALT_TARGET
    ignore_error: true

  - type: READ_32
    addr: 0x40000000
    ignore_error: true

  - type: WRITE_32
    addr: 0x40000010
    data: 0x00000001

  - type: READ_MOD_WRITE_32
    addr: 0x40000020
    mask: 0xFFFFFF00
    data: 0x000000AA

  - type: POLL_32
    addr: 0x40000030
    mask: 0x80000000
    expected: 0x00000000
    timeout_ms: 500

  - type: DELAY_MS
    delay_ms: 100

  - type: SWD_RESET_TARGET
```

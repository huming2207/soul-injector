# Job scripts

A job can carry a JavaScript script instead of the pre/post procedure YAML.
Its `preProgram` and `postProgram` hooks run where the procedures would, with
the same target operations plus waits, error handling and logic. A `test`
hook can replace the job's self tests, and `onFail` runs when the run fails. Scripts need
a Rev 8 build (`CONFIG_SI_JS_ENABLE`); other builds refuse jobs that have one.

```js
import * as t from 'si:target';

export async function preProgram(ctx) {
    t.halt();
    t.write32(0x58004008, 0x45670123);
    await t.poll32(0x58004014, 0x80000000, 0, { timeoutMs: 100 });
}

export async function postProgram(ctx) {
    console.log(`${ctx.job.name}: done`);
}
```

[`scripts/stm32wl-rdp.js`](../../scripts/stm32wl-rdp.js) is `pre_prog.yaml`
and `post_prog.yaml` as a script.

## In the job

The job names the script as an asset and may pin its SHA-256
(`job.proto`, `Script`):

| Field             | Meaning |
| ----------------- | ------- |
| `asset`           | Asset name; the device reads `/data/<asset>`. |
| `sha256`          | Optional. When set, the job only activates and runs with exactly this file, like the firmware images. |
| `hook_timeout_ms` | Per hook. 0 uses `CONFIG_SI_JS_HOOK_TIMEOUT_MS` (30 s). |
| `network`         | The script may use `esp:ws`. Needs firmware built with `CONFIG_SI_JS_NETWORK`; others refuse the job's script. |

A job has either a script or pre/post procedures, not both.

Every run record in the production log carries the SHA-256 of the script the
run read (`RunRecord.script_sha256`), whether the job pinned it
(`script_pinned`), and what the script logged (`results_json`, see
[si:log](#silog)).

## Compiling

The device runs QuickJS bytecode, compiled on the PC by the same QuickJS
version the firmware has. `sidp-agent` compiles the script, uploads it and
sets `Script` in the job:

```sh
sidp-agent job push --port /dev/ttyACM0 --target target.yaml \
    --script my-job.js --image firmware.bin=build/app.bin
```

It refuses a device whose QuickJS-NG version (`js_version` in `DEVICE_INFO`,
shown by `sidp-agent device info`) differs from the one it compiles for.
`components/esp-quickjs-ng/tools/qjs_esp_bc.py compile my-job.js -o my-job.qbc`
makes the same bytes; `--script` accepts its `.qbc` too.

The script must be one ES module. Bundle local imports first (esbuild,
`bun build`); imports like `si:target` are left to the device. The device
only loads job scripts as bytecode, and scripts get no `eval` or
`new Function`.

## Hooks

All hooks are optional exports; each gets `ctx`:

```js
{ hook: 'preProgram', job: { name, variant, family } }
```

| Hook | Runs |
| ---- | ---- |
| `preProgram(ctx)` | After the backend session starts, before target detection. |
| `test(ctx)` | After verify, instead of the job's `self_tests`. The CPU is reset after it passes (not on ESP32). |
| `postProgram(ctx)` | After the self tests. |
| `onFail(ctx, stage, err)` | When the run fails, before the device lets go of the target. |

- `onFail` gets the stage that failed (`preProgram`, `detect`, `erase`,
  `program`, `verify`, `test`, `postProgram`) and an `Error`. Its message is
  the script's own error when a hook failed, or `'failed'`. Use it to put the
  target in a safe state or `log.record()` what went wrong. If it throws, the
  device logs that, and the run record still shows the first failure.
- `onFail` doesn't run on a cancel, or after the script itself ran out of time
  or memory or was stopped: by then its runtime is gone.
- A hook passes when it returns or its promise resolves. A throw or a
  rejection fails the run at that stage, and the device shows the message.
- Both hooks of a run share one runtime, so `preProgram` can leave values for
  `postProgram`. Each run starts from a fresh runtime.
- The module's top level runs once when the run starts. It can't touch the
  target.

## si:target

| Call | Kind | Does |
| ---- | ---- | ---- |
| `read32(addr, opts?)` | sync | Returns the word at `addr`. |
| `write32(addr, value, opts?)` | sync | |
| `rmw32(addr, andMask, orBits, opts?)` | sync | Writes and returns `(old & andMask) \| orBits`, like `READ_MOD_WRITE_32`. |
| `halt(opts?)` | sync | Halts the CPU. |
| `resume(opts?)` | sync | Lets a halted CPU run. Debug stays on, so `halt()` works after it. |
| `readReg(reg, opts?)` | sync | Returns a core register. The CPU must be halted. |
| `writeReg(reg, value, opts?)` | sync | The CPU must be halted. |
| `readMem(addr, len, opts?)` | sync | Returns a `Uint8Array`, at most 4096 bytes. |
| `writeMem(addr, bytes, opts?)` | sync | At most 4096 bytes. |
| `poll32(addr, mask, expect, { timeoutMs = 1000, intervalMs = 1 })` | promise | Resolves with the value once `(value & mask) == expect`. |
| `call(addr, [r0, r1, r2, r3], { stackTop, timeoutMs = 1000 })` | promise | Runs the function at `addr` in target RAM and resolves with its `r0`. |
| `waitHalt()` | promise | Waits for the CPU to halt. |
| `reset()` | promise | Toggles the target reset line. |
| `reinit()` | promise | Re-initialises the debug connection after a reset. |
| `delay(ms)` | promise | |

`reg` is a name (`r0`-`r15`, `sp`, `lr`, `pc`, `xpsr`, `msp`, `psp`,
`fpscr`, `s0`-`s31`, any case) or the raw `DCRSR` register number (0-127).
`cfbp` (20) is CONTROL, FAULTMASK, BASEPRI and PRIMASK packed into one word.

Sync calls take `{ timeoutMs = 500, retries = 0 }`. They only work inside a
hook, and not while a promise call is still pending. Failures throw (or
reject with) a `TargetError` whose `code` is:

- `NOT_OWNED`: called outside a hook.
- `BUSY`: a sync call while a promise call is pending, more than four
  promise calls at once, or a second buffer call (`readMem`, `writeMem`,
  `flash.*`) before the first one finished.
- `TIMEOUT`: the call, or the poll, ran out of time.
- `STOPPED`: the run was cancelled.
- `NOT_HALTED`: `readReg`/`writeReg` while the CPU runs.
- `UNSUPPORTED`: this target can't do that (see [Targets](#targets)).
- `FAULT`: the target operation failed; `err` holds the `esp_err_t`.

`call()` puts two breakpoint instructions at `stackTop - 8` to return to, and
the function's stack below them; `stackTop` must be 8-byte aligned inside a
RAM region of the job. The function runs with interrupts masked. On a
timeout, a cancel, or a halt anywhere but the return breakpoint (a fault),
the CPU is left halted where it stopped.

`console.log/info/warn/error` go to the device log.

## si:flash

| Call | Does |
| ---- | ---- |
| `erase(addr, len)` | Erases whole sectors. |
| `write(addr, bytes)` | Programs at most 4096 bytes. |
| `read(addr, len)` | Resolves with a `Uint8Array`, at most 4096 bytes. |

All three return promises. Addresses are what the target uses: absolute on
Cortex-M (`0x0803f800`), the flash offset on ESP32 (`0x3f0000`). They're
meant for per-board data next to the firmware: serial numbers, keys,
calibration.

- **Cortex-M**: through the job's flash algorithm, inside its flash range.
  Sectors are `page_size` long. `write` needs erased flash and a length the
  flash can program (a multiple of 4, of 8 on STM32WL). Erase and write
  reload the flash algorithm first, which resets the target.
- **ESP32**: through the flasher stub, only after detection. `erase` takes
  whole 4 KB sectors; `write` erases the 4 KB sectors it covers itself, so
  anything else in them is lost, and is MD5-checked like an image.

## si:crypto, si:log, si:job

```js
import { randomBytes, sha256 } from 'si:crypto';
import * as log from 'si:log';
import { onProgress } from 'si:job';

const key = randomBytes(16);                  // hardware RNG, at most 4096
const digest = sha256(key);                   // bytes or a string (UTF-8)

log.record({ serial: 'SN0001' });             // merged into the run's results
log.check('vdd', 3.31, { lo: 3.0, hi: 3.6, unit: 'V' });  // throws when out of limits

onProgress(({ stage, pct }) => console.log(stage, pct));
```

- `log.check()` records `{ value, lo, hi, unit, pass }` under its name, then
  throws a `RangeError` when the value is out of limits, which fails the hook.
- The results are one JSON object of at most 359 bytes in the run record. A
  `record()` or `check()` that would go over throws and changes nothing.
- `onProgress` gets `{ stage, pct }` while the device erases (`erase`),
  programs (`program`) and verifies (`verify`), between the hooks. Updates
  are merged: at most one every 100 ms, plus every stage change and every
  100 %.

## Network

With `Script.network` set (and a firmware built with `CONFIG_SI_JS_NETWORK`),
the script can `import * as ws from 'esp:ws'` and talk to a server over
WebSocket, e.g. to report a run or fetch per-board data. Without it the
import fails.

## Targets

| | Cortex-M (SWD) | ESP32 (UART) |
| --- | --- | --- |
| `read32`, `write32`, `rmw32` | yes | after detection only (registers through the stub) |
| `readMem`, `writeMem` | yes | after detection only, word aligned |
| `halt`, `resume`, `waitHalt`, `reinit` | yes | `UNSUPPORTED` |
| `readReg`, `writeReg` | yes | `UNSUPPORTED` |
| `reset` | yes | yes |
| `call` | yes | `UNSUPPORTED` |
| `flash.*` | yes | after detection only |

On ESP32, "after detection" means `test`, `postProgram`, and `onFail` when
the run failed after detection.

## Limits

A script can't take the device down with it:

- **Memory**: everything it allocates comes from a private 2 MB PSRAM arena
  (`CONFIG_SI_JS_ARENA_KB`); a bare runtime takes about 160 KB of it. Running
  out ends the script: the hook fails at once with "InternalError: out of
  memory", and nothing else is affected. `try`/`catch` can't catch it
  (QuickJS-NG makes it uncatchable), so a script that might use a lot should
  keep its own count.
- **Time**: a hook that runs past its timeout fails. One turn of JS (a
  callback, or a batch of promise jobs) may run at most 200 ms
  (`CONFIG_SI_JS_SLICE_MS`); waiting on the target doesn't count. A busy
  script yields every 20 ms so other tasks keep running.
- **Hangs**: if the JS task doesn't come back after a timeout, the run fails
  and scripts stay off until the device restarts; programming without scripts
  keeps working.
- **Crashes**: if the device resets from a panic or watchdog during a run, the
  next boot refuses that script until the following restart, instead of
  crashing on every target plugged in.
- **Access**: no files or environment; the `si:*` modules and `console`,
  plus `esp:ws` only for jobs that allow it.

The arena, the guards and their limits are explained in esp-quickjs-ng's
[docs/untrusted-code.md](../../components/esp-quickjs-ng/docs/untrusted-code.md).

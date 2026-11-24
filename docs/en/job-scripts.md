# Job scripts

A job can carry a JavaScript script instead of the pre/post procedure YAML.
Its `preProgram` and `postProgram` hooks run where the procedures would, with
the same target operations plus waits, error handling and logic. Scripts need
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

A job has either a script or pre/post procedures, not both.

Every run record in the production log carries the SHA-256 of the script the
run read (`RunRecord.script_sha256`) and whether the job pinned it
(`script_pinned`), so an unpinned script is still traceable.

## Compiling

The device runs QuickJS bytecode, compiled on the PC by the same QuickJS
version the firmware has:

```sh
python components/esp-quickjs-ng/tools/qjs_esp_bc.py compile my-job.js -o my-job.qbc
```

The script must be one ES module. Bundle local imports first (esbuild,
`bun build`); imports like `si:target` are left to the device. The device
only loads job scripts as bytecode, and scripts get no `eval` or
`new Function`.

`sidp-agent` doesn't compile or upload scripts yet; until it does, upload the
`.qbc` as an asset and set `Script` in the job by hand.

## Hooks

Both hooks are optional exports; each gets `ctx`:

```js
{ hook: 'preProgram', job: { name, variant, family } }
```

- `preProgram` runs after the backend session starts, before target
  detection. `postProgram` runs after the self tests.
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
| `poll32(addr, mask, expect, { timeoutMs = 1000, intervalMs = 1 })` | promise | Resolves with the value once `(value & mask) == expect`. |
| `waitHalt()` | promise | Waits for the CPU to halt. |
| `reset()` | promise | Toggles the target reset line. |
| `reinit()` | promise | Re-initialises the debug connection after a reset. |
| `delay(ms)` | promise | |

Sync calls take `{ timeoutMs = 500, retries = 0 }`. They only work inside a
hook, and not while a promise call is still pending. Failures throw (or
reject with) a `TargetError` whose `code` is:

- `NOT_OWNED`: called outside a hook.
- `BUSY`: a sync call while a promise call is pending, or more than four
  promise calls at once.
- `TIMEOUT`: the call, or the poll, ran out of time.
- `STOPPED`: the run was cancelled, or there's no target to talk to.
- `FAULT`: the target operation failed; `err` holds the `esp_err_t`.

`console.log/info/warn/error` go to the device log.

## Limits

A script can't take the device down with it:

- **Memory**: everything it allocates comes from a private 2 MB PSRAM arena
  (`CONFIG_SI_JS_ARENA_KB`); running out throws in the script, nothing else
  is affected.
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
- **Access**: no files, network or environment; only `si:target` and
  `console`.

The arena, the guards and their limits are explained in esp-quickjs-ng's
README ("Running code you don't trust").

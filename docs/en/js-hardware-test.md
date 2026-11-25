# Hardware test: job scripts, agent sessions and traps

Everything on `feature/js` (soul-injector) and `origin/feature/js`
(sidp_agent) has only run in esp-emu against a mock target and against a
simulated device. This plan takes it onto a Rev 8 board with a real
Cortex-M target. Results go in the tables as each part runs.

## Bench

| Item | Use |
| ---- | --- |
| Soul Injector Rev 8 + adaptor board | `/dev/ttyACM0` (SIDP over USB), `/dev/ttyUSB0` (CH340 debug UART) |
| RAK3172 (STM32WLE5CC, Cortex-M4, no FPU) | Cortex-M target, firmware variants below |
| ESP32 target (optional) | Job script calls over UART, if one is on the bench |
| hu-net, `sidp-agent serve` on 8080 | WiFi runs; device `1C2904D03896`, password `si-test-8080` |

The debug UART runs at 921600 baud (the ROM banner at 115200). Opening
`/dev/ttyUSB0` resets the ESP32 (DTR/RTS), so the UART log is opened once and
left open. That reset pulse is ragged: the first boot after it locks up in
early ESP-IDF startup and the chip restarts itself about 0.5 s later (see
[findings](#findings)). Test runs and their records are kept in
`si-wle5-debug/results/` (`uart.log`, `runs.jsonl`).

## Preparation

1. sidp_agent: check out `feature/js` (the local checkout is `master`,
   without the JS commits), `cargo build --release`, `cargo test`.
2. Firmware: `idf.py --preview -B build-rev8 -D SI_HW_REV=rev8 build`
   (ESP-IDF v6.1.1; run `tools/patch-idf-fatfs.sh` after a fresh IDF clone).
   The generated sdkconfig has `CONFIG_SI_JS_ENABLE=y`. Flashed over
   `/dev/ttyUSB0`; `data` and `log` stay.
3. Target firmware in `~/Projects/si-wle5-debug`, one image per watch kind
   (`make MODE=...`), all keeping `counter` and the `fault` record:

   | MODE | Does | For |
   | ---- | ---- | --- |
   | `crash` | The existing firmware: divide by zero after ~5 s, HardFault handler waits | `fault` |
   | `bkpt` | `bkpt #1` every 2 s, then carries on | `halt` |
   | `lockup` | Divide by zero, then a fault inside HardFault | `lockup` |
   | `resetloop` | `SYSRESETREQ` every 2 s (`resetfast`: every 50 ms) | `reset`, throttling, rate limit |
   | `rare` | Faults at a pseudo-random time, 2 to 20 min apart (`rarequick`: 5 to 30 s); the HardFault handler resets the chip, as production firmware would | trap soak |
   | `sleep` | Counts for 5 s, then Stop 2 for good (only a reset wakes it) | target lost, `--connect-under-reset` |

   Every image keeps a status block at `0x20000000` that survives resets:
   magic `0x53495445`, boots, faults, a state word that changes every tick,
   and the next fault tick (`rare`).

4. Test scripts in `~/Projects/si-wle5-debug/js/`: a job script that uses
   every `si:target`, `si:flash`, `si:crypto`, `si:log` and `si:job` call, a
   failing variant per stage, and the trap modules below.

## Part 1: device and regressions

| # | Test | Pass when | Result |
|---|------|-----------|--------|
| 1.1 | `sidp-agent device info` | Shows Rev 8, `js_version` 0.18.0, a 2048 KB arena, network off | Pass. QuickJS-NG 0.18.0, 2048 KiB, network off. `device info` doesn't show the hardware revision |
| 1.2 | First boot after flashing | No panic; `data` and `log` mount; free internal and PSRAM heap at idle noted from the UART log | Pass, after the lockup-and-restart above. Idle: 139 KB internal RAM free (min 102 KB), 10.7 MB PSRAM |
| 1.3 | Plain job without a script (crash.bin, `pre_prog.yaml`/`post_prog.yaml`) | Programs and verifies as before | Pass, without procedures (the root YAML sets RDP 1): 648 bytes, 3.4 s |
| 1.4 | GDB over USB with `crash.gdb` | Same as Phase 3: lands in `hard_fault_report`, `fault.cfsr` bit 25; `stepi` about 7.5 ms | Pass. Same backtrace (`divide` ← `average` ← `crash` ← `main`, `by=0`); `stepi` 12.1 ms with the flash cache off (17 ms off, 7.5 ms on, on 2026-11-13) |
| 1.5 | GDB over WiFi through `serve` | Same; `stepi` about 12.5 ms | Not run: the device's WiFi settings were wiped since 2026-11-13; needs the WiFi password |

## Part 2: job scripts on the RAK3172

The job programs `crash.bin` (or `bkpt.bin`) with a script.

| # | Test | Pass when | Result |
|---|------|-----------|--------|
| 2.1 | `job push --script hw-test.js` (pinned), manual run | All hooks run in order; console lines on the UART log; the run record has `script_sha256`, `script_pinned`, `results_json` | Pass. 8.5 s; record has `script_sha256`, `script_pinned`, `results` |
| 2.2 | `read32` CPUID `0xE000ED00`, DBGMCU IDCODE | `0x410FC241`, the WL device id | Pass. `0x410fc241`, `0x10016497` |
| 2.3 | `rmw32`, `poll32` on a harmless register (e.g. a GPIO ODR with the clock on) | Values read back; `poll32` resolves; one with a wrong `expect` rejects with `TIMEOUT` near its `timeoutMs` | Pass, on an SRAM word. Mismatch: `TIMEOUT` after 103 ms; `delay(100)` 110 ms |
| 2.4 | `halt`, `readReg('pc')`, `writeReg('r0')`, `resume`, `waitHalt` with `bkpt.bin` | PC inside flash; R0 reads back; `waitHalt` resolves at the next `bkpt` | Pass for `halt`/`readReg`/`writeReg`/`resume`/`NOT_HALTED`; `waitHalt` not tried yet. `readReg('s0')` on this FPU-less core returns a value instead of failing |
| 2.5 | `readMem`/`writeMem` 4096 bytes in SRAM2 | Same bytes back; time per 4 KB noted; 4097 throws | Pass. 4 KB: write 15.9 ms, read 14.9 ms (275 KB/s); 4097 refused. A plain `number[]` is refused (TypeError), see findings |
| 2.6 | `call()`: write `adds r0, r0, r1; bx lr` to SRAM, call with `[2, 3]`, `stackTop` at the end of SRAM2 | Resolves with 5; a function that spins hits its timeout and leaves the core halted | Pass. 5 in 2 ms; four arguments; spin: `TIMEOUT` at 203 ms, core halted in it; another BKPT: `FAULT`; bad `stackTop` refused |
| 2.7 | `flash.erase/write/read` a serial number in the last page (`0x0803F800`), random bytes from `randomBytes`, `sha256` of them | Read back equal; the firmware still runs after the run (the flash algorithm reload resets the target); `write` of 6 bytes is refused (multiple of 8) | Pass. Erase 597 ms, write of 16 bytes 568 ms; 6 bytes, over written words, outside the flash, and a 4-byte erase are all `FAULT` and change nothing |
| 2.8 | `log.record`, `log.check` in limits, then out of limits | Results in the run record; out of limits fails the hook with the message on the LCD/run record | Pass. `vdd = 2.5V is out of limits`; the check is in the record with `pass: false` |
| 2.9 | `onProgress` | `erase`, `program`, `verify` stages with rising `pct`, at most ~10 a second | Pass. 240 KB image: 36 updates in about 6 s (6.1 a second), every stage to 100 |
| 2.10 | `test()` reads `counter` twice 500 ms apart | Counter rises; CPU reset after the hook; job `self_tests` skipped | Pass. `test()` starts with the core halted after verify (see findings); after `reset()` the state word changes |
| 2.11 | A throw in each hook in turn, and `onFail` | Run fails at that stage; `onFail` gets the right `stage` and message; a throw in `onFail` keeps the first failure | Pass. Right stage each time; a throw in `onFail` keeps the first failure. `err.message` repeats the name ("Error: …") |
| 2.12 | `preProgram` that waits past `--hook-timeout-ms 2000` | Fails at about 2 s; next run works | Pass. Stopped 2.01 s after the hook started; next run passes |
| 2.13 | Spin loop in a hook | Stopped after about 200 ms; the device stays responsive over USB | Pass. Stopped after 210 ms ("without yielding"); USB fine |
| 2.14 | Allocating until out of memory | Throws in the script; next run works; heap after the run as before | Failed, fixed (6fe9f35): the hook waited for its 30 s timeout. Now fails in 0.6 s with "InternalError: out of memory", before and after an await; heap unchanged. It can't be caught (QuickJS-NG) |
| 2.15 | Pinned script replaced with `asset push` | Job refuses to run (hash); unpinned job runs the new one | Pass. Pinned: refused at load in 42 ms ("not the image this job pins"); unpinned: runs the new script, record shows its hash |
| 2.16 | 50 runs in a row (auto on detect, or a loop of manual runs) | All pass; heap at the end equal to the start | Pass. 50 of 50 in 7 min 50 s; every run: arena peak 200 KB, free internal 139055 (min 99844), PSRAM 10965508 (min 10950740) |
| 2.17 | `stm32wl-rdp.js` (RDP 0 → program → RDP 1 → next run RDP 0 with mass erase) | Same outcome as the YAML procedures. Run last in this part: the target ends in RDP 1 and the next run erases it | |
| 2.18 | ESP32 target, if present: `read32` of an eFuse/MAC register after detect, `flash.write` per-board data at a free offset, `halt` | Values correct, MD5 checked; `halt` gives `UNSUPPORTED` | Skipped: no ESP32 target on the bench |

## Part 3: agent sessions (CLI, then MCP)

With `agent eval` / `agent run` first, then the MCP server from Claude Code
(`claude mcp add soul-injector -- sidp-agent mcp --port /dev/ttyACM0 --elf
crash.elf ...`, then a restart of Claude Code so it loads).

| # | Test | Pass when | Result |
|---|------|-----------|--------|
| 3.1 | `agent eval "target.read32(0xE000ED00).toString(16)"` | `410fc241` | Pass. `"410fc241"`, 0.4 s for the whole session |
| 3.2 | `halt`, `registers`, `resume` | Registers sane, names from the ELF | |
| 3.3 | `write32` without `--allow-write` | `NOT_ALLOWED`; works with it. Same for flash and reset | Pass. Without flags `write32`, `writeReg`, `call`, `flash.erase`, `reset` give `NOT_ALLOWED`; reads, halt and resume work; with the flags all are allowed |
| 3.4 | `fault` watch, reflex `halt`, on `crash.bin` | Fires once; capture: `stacked.pc` in `divide`, CFSR DIVBYZERO, HFSR FORCED, core held at `HardFault_Handler`; matches what GDB showed in 1.4 | Failed, fixed (7bbe2a9, swd_esp 9c4282c): the stack capture ran past the end of RAM and broke the session. Now: stacked PC `0x080000ce` (`divide`), LR `0x080000ef` (`average`), CFSR `0x02000000`, HFSR `0x40000000`, held at `HardFault_Handler` |
| 3.5 | Same, reflex `none` / handler returns `'resume'` | Firmware's handler runs; `fault` record in RAM filled | Pass, both. The firmware's handler then fills `fault` (same PC and LR); with `none` the core still halts briefly for the capture |
| 3.6 | `halt` watch with `bkpt.bin`; FPB breakpoint on `crash` (guide recipe) | Fires on each stop; `target.halt()` from the script doesn't fire it | Failed, fixed (99461fe): the handler's `readMem` threw `BUSY` while the module awaited `delay()`, and the core stayed at the BKPT. Now ten BKPTs stepped over in a row; the script's own `halt()` doesn't fire the watch. FPB recipe not tried yet |
| 3.7 | `lockup` watch with `lockup.bin` | Fires; capture taken | Pass. At `hard_fault_report+0x70` (the `udf`), CFSR `0x02010000`, HFSR `0x40000000` |
| 3.8 | `reset` watch with `resetloop.bin`, and the RAK3172 reset button | Fires on both; reflex `reset` works | Pass for `resetloop.bin`: one firing per reset (every 1.63 s), the handler reads the boot count. Reset button: needs a hand |
| 3.9 | `resetloop FAST=1` | Throttled past 10 a second: core held, handler not called | Pass. 10 handled, the 11th in the same second held without the handler |
| 3.10 | Target power off and on during a session | `target` events: stopped answering, answering again | |
| 3.11 | `sleep.bin` with and without `--connect-under-reset` | Session opens under reset; without it, a clear error | Pass: "not responding" when it sleeps; under reset the session opens halted at `Reset_Handler`. But a plain attach also works, by resetting the target: see findings |
| 3.12 | Sampling recipe, 100 ms, 10 min; `console`/`emit` flood | Samples steady; events capped at 20 a second with `missed` counted | Pass. 10 min on `rare.bin`: 4961 samples (8.4 a second: `delay(100)` takes 110 ms at the 100 Hz tick), no errors, longest gap 130 ms. Flood: about 20 events a second, `emit` false when limited; `agent run` lost the first events, fixed (sidp_agent bce50df) |
| 3.13 | GDB attach during a session, session open during GDB, job run during a session | Each refused with "busy" | Blocked: over USB the second host process can't open the port (EBUSY), so the device's refusal isn't reached. Needs a session over WiFi, or a device-side trigger (auto run on detect) |
| 3.14 | `--session-end asis`, `resume`, `reset` | Target left as chosen | Pass. `asis` leaves it halted, `resume` running, `reset` resets it (boot count +1) |
| 3.15 | Session over WiFi through `serve` (`SIDP_PASSWORD`) | Same as 3.4; USB request mid-session takes over, the MCP server reopens and says the old session is gone | |
| 3.16 | MCP end to end: I'm asked "why does this target crash?" with `crash.elf` | I find `divide` by zero through `average` and `crash`, from the capture | |

## Part 4: traps

| # | Test | Pass when | Result |
|---|------|-----------|--------|
| 4.1 | Arm a `fault` trap (reflex `none`, handler `{ action: 'resume', data }`) on `crash.bin`, then close the agent and unplug USB data (power only) | Status `armed`; incident written; `incident show --elf` has the capture, the events before it and `data` | Pass without the USB-unplug part: incident with the decoded capture (`divide+0xe`, DIVBYZERO, FORCED), UTC, and the handler's `data`. `events_before` also held another session's events (see below) |
| 4.2 | Restart the device (CH340 open, then a power cycle) | Trap starts again from the top; boot count in the next incidents goes up | Pass for an ESP restart (CH340 open): trap starts again by itself, incidents kept, the target not reset by it. Power cycle: needs a hand |
| 4.3 | `yield` mode: GDB attaches, then detaches | GDB works; trap starts again about 3 s after the detach | Pass. Paused "a debugger attached"; GDB works (`stepi` 12.3 ms); trap back after the detach |
| 4.4 | `yield` mode: manual run and auto run on detect | Runs work; trap back after them | Pass for manual runs: paused "a programming run", back 3.2 s after the run, the next crash is an incident. Auto run on detect: needs a hand. Found: every trap start stalled SIDP for 5 s, fixed (37ba76d) |
| 4.5 | `exclusive` mode | GDB and runs refused while armed | Pass: job runs refused (status 43); GDB refused, first as "SWD error", fixed to "busy" (e000c2c, sidp_client 82f0e26) |
| 4.6 | Session while armed | Refused; `trap status`, events and captures still readable | Pass. Session refused (status 43); status readable |
| 4.7 | Rate limit with `resetloop.bin` and a `reset` watch | 12 an hour, then `suppressed` counts | Pass. 12 incidents, then 15 counted as suppressed in 40 s |
| 4.8 | `--incidents-per-hour 3600`, more than 32 incidents | Oldest dropped; count stays at 32 and under 512 KB | Pass. 32 kept (ids 37–68), 106 KB |
| 4.9 | Target unplugged for a minute, plugged back | Backoff while gone; trap back within a few seconds of the target answering | |
| 4.10 | Module that throws after starting | Restarted with backoff, gives up after 5 with a note; arming a module whose top level throws is refused with the error | Pass after a fix: a top level that throws is refused at arm; one that throws later was never restarted (trap stayed "running"), fixed (c42ff27): 5 starts, then `failed` |
| 4.11 | UTC times | With the clock set (`log pull` sets it), incidents have UTC; after a restart with no computer, boot count and uptime | Partly: incidents carry UTC once a command set the clock. Not yet checked: after a restart with no computer |
| 4.12 | Disarm | Target left as it is; sessions work again; the device goes back to light sleep (PM lock released) | Pass for GDB and sessions after disarm. Light sleep: can't be seen while USB powers the device (light sleep is off then) |
| 4.13 | Soak, 48 to 72 h: `rare.bin`, a trap keeping the last 64 state values, USB power only | One incident per fault (count against the target's own fault counter), no device restarts, `starts` stays 1, heap steady on the UART log | |

Not covered on hardware: the crash quarantine across an ESP32 panic (no way
to cause one on demand; covered in esp-emu), and `esp:ws` (Rev 8 doesn't
build `CONFIG_SI_JS_NETWORK`).

## Findings

### Fixed (committed and pushed)

| What | Commits |
| ---- | ------- |
| A script that ran out of memory hung: a job hook until its 30 s timeout, an agent eval until the agent gave up. QuickJS-NG makes "out of memory" uncatchable, and an async function that dies of it never settles its promise. The loop now stops with `QJS_ESP_STOP_ERROR`; the hook or eval fails at once with "InternalError: out of memory". | esp-quickjs-ng 1eea730, soul-injector 6fe9f35, sidp_agent 87da0e6 |
| One read past the end of RAM (a watch's default stack capture near the top of RAM does it) left STICKYERR set: every later access in the session failed, the capture had no stack. swd_esp clears the sticky flags after every FAULT ACK; captures keep what's readable. | swd_esp 9c4282c, soul-injector 7bbe2a9 |
| In a session, sync calls were `BUSY` while a `delay`, `poll32` or `waitHalt` was pending, so a watch handler couldn't read memory while the module's loop waited, and the core stayed held. | soul-injector 99461fe, sidp_agent 9f76961 |
| Every trap start (boot, after GDB, after every run, every retry) stalled USB and network SIDP for 5 s while it waited for the module's top level. Now started and checked between requests. | soul-injector 37ba76d |
| A trap module whose top level threw after the start window was never restarted: the trap said "running" with nothing running. Now restarted with backoff, `failed` after five. | soul-injector c42ff27, sidp_agent 6a39452 |
| Debug attach (GDB, sessions, trap restarts) reset a target that didn't answer at once, against the backend contract ("must not reset unless connect_under_reset"); a sleeping target lost its state. Now it fails with a hint; programming still retries with reset. | swd_esp e7c9bd3, soul-injector 37ea74b, sidp_agent f518307 |
| GDB attach said "SWD error: the target didn't answer" when a trap or run had the target, and "busy" when there was no job. Now `BUSY` (4) and `NEEDS_CORTEX_M` (42). | sidp_client 82f0e26 and c5d9188, soul-injector e000c2c and 37ea74b, sidp_agent 036a1e1 (its message names soul-injector "165d31e": that's e000c2c before an amend) |
| `agent eval` had two `--timeout-ms`; clap panics in debug builds and one value was lost in release. The eval's is now `--wait-ms`; a test checks the CLI definition. | sidp_agent f90ff50 |
| `agent run` didn't read events for 5 s and lost the first ~130 of a busy module silently. Now 1 s, and `{"kind":"missed"}` lines. | sidp_agent bce50df |
| `qjs_esp_bc.py` raced itself in parallel builds (3 of 4 failed). | esp-quickjs-ng 0babcbf |
| Added: one log line per script runtime with the arena peak and free heap. | soul-injector adc3bdd |

None of the test_app additions has run in esp-emu (it isn't on this
machine); they compile for the S31, and each fix was tried on the board.

### Questions for you

**Q1. Power loss during an upload damages `/data` (important).** An ESP
reset in the middle of `job push` (my mistake: I pushed while the device was
rebooting) left the FAT reading as erased: `job.pb` and the script cut at
4 KB, "4 KiB free of 5740". The device then had no job and nothing over SIDP
could repair it; I erased the partition over UART (image kept in
`si-wle5-debug/results/data-partition-corrupt.bin`, files in
`data-partition-extracted/`). Rev 8 uses `CONFIG_WL_SECTOR_SIZE_4096` for
speed (your 2026-11-13 decision), and ESP-IDF's "Safety" sector mode exists
only for 512-byte sectors, so a sector being rewritten is lost on a reset;
FatFs isn't transactional either. In the field this is a power cut during a
job push. Options: back to 512 + Safety (uploads ~15 KiB/s again); LittleFS
for `/data` (power-fail safe, a bigger change); and in any case a way to
recover: detect a damaged store at boot and reformat, or a SIDP
"format data" request. Which do you want?

**Q2. A passed run can leave the target halted.** `release_transport()`
only tri-states the pins. A `postProgram` that halts, or uses
`flash.erase`/`flash.write` (which leave the core in the flash algorithm),
ends "passed" with the firmware not running (DHCSR `0x00030003`) until a
power cycle. Should the device resume or reset the target at the end of
every run, or only document that `postProgram` must leave it running?

**Q3. `number[]` for bytes.** `api.d.ts` declares `Bytes` as
`Uint8Array | ArrayBuffer | number[]`; the firmware refuses plain arrays in
`writeMem`, `flash.write`, `sha256` ("ArrayBuffer object expected"). Accept
arrays in `get_bytes()` (friendlier for agents, a copy per call), or narrow
the declaration?

**Q4. First boot after a ragged EN pulse locks up.** Opening the CH340
gives a short, uneven EN pulse; the S31 then takes an instruction access
fault in early ESP-IDF startup (`esp_ocode_calib_init`), faults again in the
panic handler (`CPU_LOCKUP_RESET`) and restarts by itself 0.5 s later. 6 of 6
port opens, 0 of 10 clean 100 ms pulses. Harmless on the bench; an EN glitch
in the field would do the same. Worth checking the EN RC on Rev 8 and the S31
errata. Anything you want me to try?

**Q5. Rare lost request over USB.** Twice in about 900 connections the
first request after connecting got no answer (host timeout 5 s, nothing in
the device log), both right after a programming run had finished. Not
reproduced in 300 + 120 back-to-back connections since. Shall I dig into the
CDC transport (I'd add a debug counter for frames received vs answered), or
leave it?

**Q6. Should `--session-end reset` need `--allow-reset`?** It works without
it today; the person running the server chooses it, not the agent, so I left
it.

**Q7. 3.13 and the WiFi tests (1.5, 3.15)** need the device's WiFi settings
again (wiped since 2026-11-13) and the WiFi password; 3.10, 4.1 (USB data
cut), 4.2 (power cycle), 4.4 (auto run on detect) and 3.8 (reset button)
need your hands. The MCP tests (3.15, 3.16) need the Claude Code restart.

### Smaller (not fixed yet)

- `test()` starts with the core halted in the flash algorithm (DHCSR
  `0x00030003`): a script must `reset()` to see the new firmware run. The
  docs don't say so.
- Every `flash.erase`/`flash.write` call reloads the flash algorithm and
  costs about 0.55 s even for 16 bytes, mostly a 0.5 s wait in SWD init.
- Argument errors in `flash.*` (6 bytes, outside the flash, an erase not
  page aligned) come back as `FAULT`, like a target failure. An unaligned
  erase is refused, not rounded; the docs say "erases whole sectors".
- `readReg('s0')` on a core without an FPU returns a value instead of
  `UNSUPPORTED`.
- `onFail`'s `err.message` is "Error: …" (the name in the message).
- A pinned script that doesn't match says "not the image this job pins".
- An incident's `events_before` includes events from earlier sessions.
- Trap status shows `incidents_per_hour: 0` for "the default, 12".
- `gpio: conflict found for GPIO[9]` on every SWD init (BOOT pin set up
  twice). Cosmetic.
- `device info` shows neither the hardware revision nor a build with
  uncommitted changes.
- The target-detect input reads "no target" though the RAK3172 is connected
  and answers over SWD (adaptor wiring?).
- swd_esp `tests/test_init.py` didn't build since connect-under-reset; fixed
  with e7c9bd3.

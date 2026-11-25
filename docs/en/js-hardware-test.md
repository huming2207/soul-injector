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
| 3.16 | MCP end to end: I'm asked "why does this target crash?" with `crash.elf` | I find `divide` by zero through `average` and `crash`, from the capture | Pass without the ELF part (2026-11-26): the server was started without `--elf`, so I resolved the capture's addresses on the host. Guide read; `run_script` with the guide's fault recipe, `eval` reset, `wait_event` got the watch, `get_capture`: DIVBYZERO/FORCED, stacked PC `0x080000ce` = `divide` (main.c:97), LR and the stack's return addresses = `average` ← `crash` ← `main`, r0 50 / r1 0. With `--elf crash.elf` in the server's args the names come from the server itself (not seen yet) |

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
| 4.11 | UTC times | With the clock set (`log pull` sets it), incidents have UTC; after a restart with no computer, boot count and uptime | Pass. With the clock set, UTC; after a restart with no `log pull` since, incidents show "boot 111 +97.2 s" |
| 4.12 | Disarm | Target left as it is; sessions work again; the device goes back to light sleep (PM lock released) | Pass for GDB and sessions after disarm. Light sleep: can't be seen while USB powers the device (light sleep is off then) |
| 4.13 | Soak, 48 to 72 h: `rare.bin`, a trap keeping the last 64 state values, USB power only | One incident per fault (count against the target's own fault counter), no device restarts, `starts` stays 1, heap steady on the UART log | First 7 h pass (2026-11-25 23:45 to 06:40, build 41b1c31): 50 faults, 50 incidents (309-358; the target's fault counter in each capture equals its capture id, so none missed), 0 suppressed, `starts_this_boot` 1, no ESP restart (uptime 6.9 h), UART log has no errors or panics. The 32 newest kept (about 3.9 KB each). Heap isn't logged while a trap runs, so "heap steady" is only "no restart". Stopped there to try the review fixes. In the 50 minutes after that (ESP reflashed and restarted, trap back each time) one fault was **missed**: the target sat halted at the HardFault entry with DFSR VCATCH, counter 51, no incident. Fixed (a371cf9, c7f195c, below). **Started again 2026-11-26 08:24:01** on build bbdfb54+c7f195c, trap `soak` on `rare.bin`, target fault counter 78 at the start: incidents from then on should equal the counter's growth |

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
| About one USB request in 300 got no answer (host timeout 5 s): it arrived after DTR but before the busy service task accepted the connection, and was flushed. Now kept for the session. 30 runs + status, 200 incident lists during incident writes: none lost. | sidp_client 7009216, soul-injector 909fd6a |
| Added: one log line per script runtime with the arena peak and free heap. | soul-injector adc3bdd |
| **A fault while the probe restarted was lost for good.** The target's vector catch survives an ESP restart, so a fault in that window halts the core with nobody polling; the restarted trap's first look only noted "halted" and never fired. The soak's fault 51 sat halted for 50 minutes. Now a core found halted with a stop cause in DFSR (VCATCH, BKPT, DWT, EDBGRQ) fires the watch (or a `stop` event) once the module's watches are in; a core halted by a debugger's halt request is left alone. Then the cause stayed in DFSR only until a stop was *seen*, so a restart while a held watch's handler worked (an incident takes 0.7 s to write) left it halted again; now DFSR is cleared only when the stop is dealt with (resume or reset), and a still-held stop fires again at the next start. Bench: 18 ESP resets at random times on `rarequick.bin`, 14 faults = 14 incidents, target running; a halt trap on `bkpt.bin` armed 5 times right as the core stopped: found every time; a handler holding 1.5 s of every 2 s, the trap disarmed and armed 10 times at random: each start found the held stop again. | soul-injector a371cf9 and c7f195c, sidp_agent a4a286d |
| The first USB request after the device had been busy (a trap writing an incident) as the host reconnected was lost: the DTR edges closed the old session and kept the new request, then the busy owner tore the old connection down and `close_session()` drained the queue again. Seen as `trap status` timing out right after arming a trap that fired at once (every time). Closing a session that's already closed now does nothing; host test added. Bench: the same sequence 5 times, answered in 0.65 s. | sidp_client 9016a19, soul-injector bbdfb54 |
| Added: a log line per incident with how long it took to write (0.63 to 0.70 s on Rev 8). | soul-injector bbdfb54 |

Found in the code review afterwards; test_app cases added; tried on the
board 2026-11-26:

| What | Commit |
| ---- | ------ |
| A reset in the middle of an incident append made the whole incident unreadable ("length mismatch"): `init()` now trusts the header's total and truncates a partial last section. Board: boot with 32 incidents reads them all; the cut-append case itself wasn't reached, because the first reset during an incident write erased the whole incident directory instead (Q1). | soul-injector 937fc46 |
| An incident's `events_before` included events of earlier sessions; now only the trap's own (events after arming). Board: pass, incident 440 holds only seq 24 (its own `console`), not the 23 events of the sessions before. | soul-injector 5983410 |
| A job script runtime that died between hooks (a spin or out of memory in `onProgress`) made the remaining hooks, `postProgram` included, silently skipped: the run passed without them. Now the next hook fails the run with "script ended between hooks: …". Board: `hw-progdie` fails at the next exported hook (`test`): "Self test: script: script ended between hooks: script ran too long without yielding". | soul-injector bfe2fcd |
| `log.record()` with a value JSON can't hold (a BigInt) left it in the results, so every later `record()`/`check()` of the run threw; now every failure rolls back. Board: `hw-log` passes, the run record has `before`, `after` and the check, nothing of the refused records. | soul-injector 04b071b |

Also 2026-11-26 on the new build: `hw-ops` passes, and 50 `hw-ops` runs in a
row with `device info` after each: 50 pass, no request lost.

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
job push.

It happened a second time the same evening, from ESP resets (a reflash and a
logger restart) while a trap wrote an incident every 1.6 s: the store held
33 files (about 110 KB) but both FAT copies marked 1435 of 1437 clusters
used, "4 KiB free", and job pushes and trap arming failed ("needs 6988
bytes, 4096 free"). Image: `results/data-partition-corrupt-2.bin`. Then 15
clean EN resets during incident writes didn't collapse it, but each leaked
about 14 KiB (5676 → 5472 KiB free while the incidents stayed capped at 32):
clusters a cut write left allocated, which FatFs never reclaims. A trap
that runs for days on a bench that loses power will fill `/data` this way,
and then the device can't take a new job. Options: back to 512 + Safety (slower than the old 15 KiB/s, see below); LittleFS
for `/data` (power-fail safe, a bigger change); and in any case a way to
recover: detect a damaged store at boot and reformat, or a SIDP
"format data" request. Which do you want?

*Worse than that (2026-11-26):* the **first** ESP reset during an incident
write erased the whole incident directory. 32 incidents (398-429) gone at
once; the boot after said "0 incidents, next 430". The partition dump
(`results/data-partition-dirloss.bin`) shows why: the `INC` directory's
cluster reads all `0xFF`, i.e. the WL layer had erased the 4 KB sector and
the reset came before it was written back. The 72 incident bodies are still
in their clusters, ~70 of them still marked used in the FAT (leaked). Every
incident append updates that directory sector (FatFs rewrites the file's
entry on close), so with traps writing incidents any power cut at the
wrong ~0.1 s loses every incident, and a reset during a job upload can do
the same to the job. Neither 512-byte Safety mode nor LittleFS has this
failure. A smaller step that only covers incidents: keep them in their own
raw partition as a ring (no directory to lose). I haven't changed anything
here; it's your call.

*Decision (2026-11-26): leave `data` as it is on Rev 8.* Later hardware
gets a battery and an ext4 partition on an SD card. Conditions for that to
hold:

- **ESP resets.** Every loss seen here came from ESP resets with the power
  on, and a battery doesn't stop those (watchdog, panic, OTA, the reset
  button, the debug UART). ext4's journal covers them, as long as the card
  stays powered through the reset. If an ESP GPIO switches the card's power,
  that switch must default to on when the pin floats.
- **What goes on the card.** The job store and incidents move to ext4; on
  internal FAT they keep this problem.
- **Mount and recovery.** At mount, the ext4 port must replay the journal
  and check the file system (lwext4: check that it recovers the journal).
  The device must also work with no card fitted, or a dead one.
- **Atomic file replace.** File contents still need "temp file, fsync,
  rename" to be replaced all or nothing.
- **Low battery.** Stop writing and unmount before the brown-out.
- **The card.** An industrial card is still the safer choice, in case the
  battery runs flat.

The LittleFS recommendation below is kept for the record.

*Recommendation (2026-11-26): LittleFS for `data`, plus a SIDP "format
data" request.*

- **Back to 512-byte sectors doesn't help by itself.** Before Rev 8 the
  setting was 512 with `CONFIG_WL_SECTOR_MODE_PERF`, not Safety. The old
  15 KiB/s was measured in that mode. It has the same window: to change one
  512-byte sector it erases the whole 4 KB block (8 sectors) and writes it
  back from RAM. Safety mode (`WL_Ext_Safe.cpp`) closes the window. It first
  copies the block to a spare sector and records a state sector, and puts the
  block back at mount. That costs about three erases per 512 bytes written,
  so it would be slower than 15 KiB/s (not measured) with about three times
  the wear. It also leaves the leak. FatFs writes the FAT, the directory
  entry and the data as separate sectors with no transaction, so a cut write
  still leaves clusters allocated that nothing points to. FatFs has no check
  that would get them back, so `/data` would still fill up slowly on a bench
  that loses power.
- **A journal of our own: no.** FatFs doesn't tell the disk layer where an
  update starts and ends (only `f_sync`/`f_close`). A journal under it would
  be a copy-on-write sector map written and tested by us, which is what
  LittleFS already is.
- **LittleFS** (`joltwallet/littlefs`, in the IDF component registry):
  - Metadata is copy-on-write with commits. After a cut, a file is as it was
    at its last sync or close. `rename` is atomic, so "upload to `.part`, then
    rename" replaces a job all or nothing.
  - Blocks are allocated by scanning what is reachable, so nothing leaks.
  - It does its own wear levelling on the raw partition in 4 KB blocks.
    There's no WL layer and no erase-then-rewrite of a live block, so uploads
    should be at least as fast as now (to be measured).
  - USB mass storage is off (`CONFIG_TINYUSB_MSC_ENABLED=n`), so nothing
    needs the PC to read a FAT image.
  - Code to change:
    - the mount in `bootstrap_fsm.cpp`;
    - `esp_vfs_fat_info` in `asset_store.cpp` and `sidp_service.cpp`;
    - `esp_vfs_fat_create_contiguous_file` for the upload's `.part` file in
      `asset_store.cpp` (LittleFS can't preallocate, so the free-space check
      becomes explicit);
    - the partition subtype. `data` gets formatted once, as with the 4 KB
      change.
  - Costs:
    - a few KB of RAM per open file;
    - an occasional metadata compaction stall of tens of ms;
    - slow listings with many files in one directory (incidents are capped
      at 32).
- **Either way, a way out:** mount failure already reformats. Add a SIDP
  request that formats `data` for a store that mounts but is wrong, so a
  device in the field never needs a UART.
- **`log` (on9rstore) can stay FAT for now.** Its files are created once at
  full size and then only overwritten in place, so the FAT and directories
  don't change after creation. A cut loses the 4 KB block being written.
  Still to check: whether on9rstore recovers when that block is part of
  `manifest.db`. It gets the same reset test.
- **Test before calling it done:** `esp_reset_faults.py`-style random EN
  resets, 100+ each, during job pushes and during incident writes. The
  result should be no loss beyond the file being written, and free space
  that stays the same.

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

**Q5.** (Lost USB requests: found and fixed, see above.)

**Q6. Should `--session-end reset` need `--allow-reset`?** It works without
it today; the person running the server chooses it, not the agent, so I left
it.

**Q7. 3.13 and the WiFi tests (1.5, 3.15)** need the device's WiFi settings
again (wiped since 2026-11-13) and the WiFi password; 3.10, 4.1 (USB data
cut), 4.2 (power cycle), 4.4 (auto run on detect) and 3.8 (reset button)
need your hands. The MCP tests (3.15, 3.16) need the Claude Code restart.

**Q8. Throttling in a trap holds the target.** Past 10 firings a second a
watch is throttled and the core is held (documented). In a session that's
right; in an unattended trap it stops a boot-looping target until someone
looks. Keep it, or let traps throttle by skipping the capture and letting
the core run?

**Q9. Something reset the target between 07:38 and 07:42 on 2026-11-26.**
After I found the soak's missed fault (target halted at the HardFault entry,
boots 68, faults 51), the next look showed boots 69, faults 51: a reset, not
the firmware's own handler (that counts the fault first). In that window I
closed an MCP session (`asis`), reflashed the ESP over UART, restarted the
logger (an ESP reset) and armed the trap. I then repeated each of those
steps on a halted target and none of them reset it. Were you at the bench
then (cable or board touched)? If not, it's something I can't reproduce yet.

**Q10. `job push` is refused (`BUSY`, status 4) while a `yield` trap is
armed, but `job run` goes ahead and pauses the trap.** Should a push also
pause a `yield` trap (it changes the job the trap's target config comes
from, so the trap would restart on the new job), or should both be refused
until you disarm? Today the first is surprising: `prog.sh` fails half-way
(push refused, then the old job runs).

### Morning checklist (2026-11-25, done 2026-11-26)

1. Soak (4.13), armed 2026-11-25 23:45:08 as trap `soak` on `rare.bin`
   (target fault counter was 1 at the start):
   `sidp-agent trap status --port /dev/ttyACM0` (`starts_this_boot` should
   still be 1, `state` running) and `sidp-agent incident list`. The last
   incident's `data.status` bytes 8-11 are the target's own fault counter:
   incidents should equal that counter minus 1, unless more than 12 came in
   one hour (`incidents_suppressed`). The UART log is in
   `si-wle5-debug/results/uart.log` (look for panics and "runtime ended").
2. Then flash the current build (it has 937fc46, 5983410, bfe2fcd,
   04b071b, not yet on the board): boot log "N incidents" should match
   `incident list`; run `js/run-case.sh hw-progdie crash.bin` (expect
   "failed at post-program … script ended between hooks"), `hw-ops` and
   the 50-run loop again.
3. Hands needed: 3.8 reset button, 3.10 target power, 4.1 USB data cut,
   4.2 power cycle, 4.4 auto run on detect; WiFi password for 1.5, 3.13,
   3.15; Claude Code restart for the MCP tests 3.15 and 3.16.

### Next (2026-11-26)

1. Soak (4.13) runs again since 2026-11-26 08:24:01 (fault counter 78 at
   the start): `sidp-agent trap status` (`starts_this_boot` 1, `running`),
   `sidp-agent incident list`; incidents should equal the target's fault
   counter minus 78 (read it with `tools/boots.sh` after `trap disarm`).
   UART log: `results/uart.log` ("incident N written in", "found stopped").
2. Q1 decides how `/data` should survive power cuts; until then, don't
   reset the ESP while a trap writes incidents if the incidents matter.
3. Still needs you: 3.8 reset button, 3.10 target power, 4.1 USB data cut,
   4.2 power cycle, 4.4 auto run, 4.9 target unplugged; WiFi password for
   1.5, 3.13, 3.15; `--elf` in the MCP server's args for the named-frame
   part of 3.16.

### Smaller (not fixed yet)

- A trap incident is written on the SIDP service task and takes about
  0.7 s (create, NVS commit for the next id, four appends, each a FAT
  update). Every USB request and GDB packet waits behind it; at the default
  12 incidents an hour that's fine, with `--incidents-per-hour 3600` and a
  firing every 2 s the device answered in 0.4 to 1.0 s. Moving the writes
  to their own task is a bigger change; it goes with Q1.

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
- Trap status shows `incidents_per_hour: 0` for "the default, 12".
- `gpio: conflict found for GPIO[9]` on every SWD init (BOOT pin set up
  twice). Cosmetic.
- `device info` shows neither the hardware revision nor a build with
  uncommitted changes.
- The target-detect input reads "no target" though the RAK3172 is connected
  and answers over SWD (adaptor wiring?).
- swd_esp `tests/test_init.py` didn't build since connect-under-reset; fixed
  with e7c9bd3.

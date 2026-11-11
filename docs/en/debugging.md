# Debugging with GDB

`sidp-agent` turns a Soul Injector into a GDB server for ARM Cortex-M targets.
Debugging runs over the same USB connection as job management. ESP32 targets
cannot be debugged this way.

A debug session takes the target's flash range and RAM regions from the device's
job, so a Cortex-M job must be on the device. A debug session and a programming
run cannot happen at the same time: whichever starts second is refused.

## Starting a session

To program the target and start debugging from its first instruction:

```sh
sidp-agent debug --port /dev/ttyACM0 --target target.yaml \
    --image firmware.bin=build/app.bin
```

`debug` takes the same job options as `job push`, programs the target once, and
fails before attaching if the run does not pass. It sets the job's trigger to
manual, so run `job push --auto` afterwards to re-arm a job that runs when a
target is plugged in.

To attach to whatever the target is already running, without programming it:

```sh
sidp-agent gdb --port /dev/ttyACM0
```

Both print `GDB listening on 127.0.0.1:3333`. Then connect an ARM-capable GDB:

```gdb
arm-none-eabi-gdb build/app.elf
(gdb) target remote 127.0.0.1:3333
```

One agent process serves one GDB connection and then exits. On a clean `detach`
the target stays halted, unless the agent was started with `--resume-on-detach`.

## Commands

The agent supports `break`, `tbreak`, `hbreak`, `continue`, `stepi`, `next`,
`finish`, Ctrl-C, `backtrace` (including through a fault), and register and RAM
access. [Hardware test status](#hardware-test-status) lists which have been
tested on a real board.

Breakpoints in flash use the core's hardware comparators, and GDB picks them
automatically from the memory map the agent provides. A Cortex-M4 has six. A
plain `break` in RAM fails, because software breakpoints are not supported.

`monitor reset` resets the target with a system reset, and `monitor reset nrst`
pulses the reset pin. Both leave the target halted at its reset vector. GDB does
not re-read registers after a `monitor` command, so run this to see them:

```gdb
(gdb) maint flush register-cache
```

Watchpoints, peripheral register reads, RTOS thread awareness and GDB `load` are
not supported. Memory outside the target's flash and RAM cannot be read.

## Firmware that sleeps or turns off SWD

Firmware that switches the SWD pins to another use or sleeps soon after boot
cannot be reached by a normal connection. Add `--connect-under-reset` to `debug`
or `gdb`:

```sh
sidp-agent gdb --port /dev/ttyACM0 --connect-under-reset
```

This holds the target's reset pin low for the whole SWD connection, then releases
it with the reset catch armed, so the target halts at its reset vector before any
firmware runs. Without the option, the attach fails on such a target. The
target's reset pin must be connected.

A programming run needs no option. If a normal connection fails, it retries under
reset and logs `retrying under reset`. Targets that connect normally are not
affected. Procedure steps in `pre_prog.yaml` and `post_prog.yaml` then run with
the target halted at its reset vector.

## When the target stops answering

If the firmware sleeps or turns off SWD while GDB is attached, the agent reports
the target as lost and exits. The Soul Injector retries its cleanup for about
three seconds, logs `target is not answering, abandoning it`, and then accepts
new USB connections. An abandoned target can keep its debug settings, such as
breakpoints, until its next reset or attach.

## Hardware test status

Tested on 2026-11-11 on a Rev 8 board with an STM32WLE5 target, using a small
bare-metal test firmware that deliberately crashes and can lock out SWD:

| Feature | Result |
| --- | --- |
| Attach, halt, continue, Ctrl-C, register and memory access | Passed |
| Plain `break`, `hbreak`, `next` | Passed |
| Backtrace through a HardFault | Passed |
| `monitor reset` (system reset) | Passed |
| `sidp-agent debug` | Passed |
| `--connect-under-reset` on firmware that locks SWD at boot | Passed |
| Programming firmware that locks SWD at boot | Passed |
| Cleanup when the target stops answering | Passed |

Not yet tested on hardware: Rev 7.1 boards with these features, `monitor reset
nrst`, `finish`, `stepi`, and pre-program or post-program steps on a target that
locks SWD at boot.

# Agent sessions

An agent session lets an LLM agent (Claude Code, or any other MCP client)
watch and drive a Cortex-M target through a Soul Injector. The agent writes
JavaScript; `sidp-agent` compiles it, and the Soul Injector runs it right
next to the target. A script can catch a fault the moment it happens, hold
the core there, and take a snapshot, without waiting for a round trip to the
computer or for the agent to think.

Like a GDB session, an agent session takes the target's flash range and RAM
regions from the device's job, so a Cortex-M job must be on the device. It
also needs a firmware with scripts (`CONFIG_SI_JS_ENABLE`, Rev 8). While a
session is open the Soul Injector owns the target: GDB can't attach and jobs
don't run, and the other way round.

## Starting the MCP server

The agent starts `sidp-agent mcp` itself and talks to it over stdin and
stdout. For Claude Code, in the project's `.mcp.json`:

```json
{
  "mcpServers": {
    "soul-injector": {
      "command": "sidp-agent",
      "args": ["mcp", "--port", "/dev/ttyACM0", "--elf", "build/app.elf"]
    }
  }
}
```

or `claude mcp add soul-injector -- sidp-agent mcp --port /dev/ttyACM0`.

The server connects when the agent first needs the target, and ends the
session when the agent exits or calls `close_session`.

| Option | Does |
| ------ | ---- |
| `--port` and the other connection options | As for every `sidp-agent` command. Over WiFi, give `--password` or `SIDP_PASSWORD`: stdin is the MCP channel. |
| `--allow-write` | Scripts may write memory and core registers, and call target functions. |
| `--allow-flash` | Scripts may erase and write flash, through the job's flash algorithm. |
| `--allow-reset` | Scripts may reset the target. |
| `--session-end asis\|resume\|reset` | How the target is left when the session ends. `asis` (the default) leaves a running target running and a halted one halted. |
| `--connect-under-reset` | Connect with the target held in reset; it starts halted. |
| `--elf FILE` | The target firmware, to name code addresses (`main+0x1c`) in events, registers and captures. |

Without any `--allow-*`, the agent can read memory and registers, halt and
resume, and set watches, but can't change the target. The device enforces
the permissions, whatever the agent sends.

## What the agent gets

The server gives the agent two documents to read (the session guide and the
script API, as TypeScript declarations) and these tools:

| Tool | Does |
| ---- | ---- |
| `eval` | Runs a snippet in the session's runtime and returns its value. |
| `run_script` | Replaces the runtime with an ES module, for watches and long-running code. |
| `wait_event` | Waits for events: watches firing, halts, console output, errors. |
| `get_capture` | Decodes the snapshot a watch took: registers, fault registers explained, the stacked exception frame, stack and memory. |
| `read_memory`, `registers`, `halt`, `resume`, `reset` | The usual. |
| `list_watches`, `stop_script`, `lookup_symbol`, `device_info`, `close_session` | |

Scripts use the job script modules (`si:target`, `si:flash`, `si:crypto`, see
[Job scripts](job-scripts.md)) plus `target.watch()` and `si:agent`. Sync
calls work at any time in a session, not only inside hooks.

## Watches

`target.watch({ on, reflex, capture }, handler)` makes the device poll the
target every 5 ms for one kind of event:

- `fault`: HardFault, MemManage, BusFault or UsageFault, caught by vector
  catch as the handler starts.
- `halt`: a halt the session didn't ask for (a breakpoint, a watchpoint).
- `lockup`: the core locked up.
- `reset`: the core reset.

When one fires, the device applies the reflex at once (keep the core halted,
let it run, or reset it), takes a capture, and then calls the handler, whose
return value can resume or reset a held core. The agent sees every firing as
an event.

## Traps

A session lasts as long as the agent's connection. For a bug that takes
hours or days to show up, the agent (`arm_trap`) or you can arm a *trap*
instead: a module the Soul Injector keeps running with no computer attached,
through disconnects and its own restarts. Each time one of its watches
fires, or it calls `agent.incident()`, it saves an *incident* to the
internal flash: what happened, the capture, the events just before, and any
data the script attached.

```sh
sidp-agent trap arm hunt.js --name hunt --port /dev/ttyACM0
sidp-agent trap status --port /dev/ttyACM0
sidp-agent incident list --port /dev/ttyACM0
sidp-agent incident show 12 --elf build/app.elf --port /dev/ttyACM0
sidp-agent trap disarm --port /dev/ttyACM0
```

| `trap arm` option | Does |
| ----------------- | ---- |
| `--name` | The trap's name, in its status and incidents. |
| `--mode yield\|exclusive` | `yield` (the default): GDB attaching or a programming run pauses the trap, and it starts again 3 s after they finish. `exclusive`: they are refused while the trap is armed. |
| `--incidents-per-hour` | More are only counted. 0 for the device's default (12). |
| `--allow-*`, `--connect-under-reset` | As for sessions; the trap keeps them. |

- Arming replaces the armed trap, and only takes when the target answers and
  the module's top level runs.
- The module starts again from the top after a restart, after yielding,
  after the target stopped answering, and when its top level throws (with a
  growing pause; after five failures in a row the trap gives up, `failed`).
  Keep what it remembers small.
- While a trap is armed, an agent can read its status, events, captures and
  incidents, but can't open a session; disarm it first.
- The device keeps 32 incidents in 512 KB and deletes the oldest first
  (Kconfig `SI_AGENT_INCIDENTS_MAX`, `SI_AGENT_INCIDENTS_KB`). Incidents
  have the time when a computer set the clock since the device started;
  otherwise the boot count and uptime.
- A trap keeps the Soul Injector at full CPU speed while it holds the
  target.

## Limits

- A session's scripts run in the same arena as job scripts, with the same
  limits: 200 ms of JavaScript at a time, running out of memory ends the
  runtime (it can't be caught), a script that crashed the device is refused
  until the next restart.
- There are no timers; scripts wait with `target.delay()`.
- The device keeps the newest 64 events and 4 captures; `console` and
  `agent.emit()` together get 20 events a second.
- Reads of peripheral registers can have side effects, and a halted target
  misses its deadlines. The agent is told so, but keep an eye on targets that
  drive hardware.

The protocol is described in sidp_client's
[docs/sidp-agent.md](../../components/sidp_client/docs/sidp-agent.md).

## Test status

The script side of sessions and traps is tested in esp-emu with a mock target
(`components/qjs_binder/test_app`), and the MCP server and trap tools
against a simulated device. Neither has been tried on a real board yet, and
the trap's storage, restarts and yielding have only been built, not run.

## Planned

- TODO: SD card support, for more room than the internal flash gives:
  incidents, longer logs and the trace buffers of a later phase. Until then
  incidents get a small part of the internal flash.

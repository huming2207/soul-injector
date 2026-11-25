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

## Limits

- A session's scripts run in the same arena as job scripts, with the same
  limits: 200 ms of JavaScript at a time, out of memory throws, a script that
  crashed the device is refused until the next restart.
- There are no timers; scripts wait with `target.delay()`.
- The device keeps the newest 64 events and 4 captures; `console` and
  `agent.emit()` together get 20 events a second.
- Reads of peripheral registers can have side effects, and a halted target
  misses its deadlines. The agent is told so, but keep an eye on targets that
  drive hardware.

The protocol is described in sidp_client's
[docs/sidp-agent.md](../../components/sidp_client/docs/sidp-agent.md).

## Test status

The firmware side is tested in esp-emu with a mock target
(`components/qjs_binder/test_app`), and the MCP server against a simulated
device. Neither has been tried on a real board yet.

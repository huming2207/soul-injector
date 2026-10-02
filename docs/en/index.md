# SoulInjector Documentation

## Getting started

- [Getting started](getting-started.md) — project overview, hardware revision
  builds, flash layout, and terminology.

## Configuration

- [target.yaml reference](target-yaml.md) — target selection, flash algorithms,
  ESP32 image configuration, and self tests. Compiled into `job.pb` on a PC.
- [Pre/post programming procedure YAML](procedure-yaml.md) — `pre_prog.yaml`
  and `post_prog.yaml` step format and execution rules.

## Operations

- [Programming flow](programming-flow.md) — the state machine run for each
  target.
- [Jobs and files](storage-files.md) — pushing, running and checking the
  device's job with `sidp-agent`, the files kept on `/data`, and collecting
  the production log.

## Engineering notes

- [Future low-power optimisations](low-power-optimisation.md) — S31 and modem
  sleep, MAIN_RI wake, proposed changes, and measurement plan.

# Storage and files

The Soul Injector has a USB port. When connected to a computer, it shows up as
a USB Mass Storage Class (MSC) device.

If any of the files mentioned below was altered, **ALWAYS MAKE SURE** that you
unmount/eject the Soul Injector before you unplug it from your computer.

## Files on device storage

The Soul Injector uses these files **at the root directory of the USB MSC
partition**, mounted internally at `/data`:

- `job.pb`: required programming job, compiled on a PC with `sidp-agent`.
- `firmware.bin`: firmware image for SWD Cortex-M targets. ESP32-family image
  files are listed in `target.yaml` (for example `bootloader.bin`,
  `partitions.bin`, `firmware.bin`).
- `.sha256` sidecars, such as `job.pb.sha256` or `firmware.bin.sha256`:
  optional `sha256sum` outputs.

If a `.sha256` sidecar exists, the matching file is verified once when assets
are loaded. If the sidecar is absent, the check is skipped. Having sidecars in
place is recommended to avoid flash corruption.

## Creating job.pb

The device no longer reads YAML. Write `target.yaml` and the optional
`pre_prog.yaml`/`post_prog.yaml` as before, then compile them on a PC:

```sh
sidp-agent compile --target target.yaml --pre pre_prog.yaml --post post_prog.yaml -o job.pb
sha256sum job.pb > job.pb.sha256   # optional
```

Copy `job.pb` (and the sidecar, if any) to the root of the USB MSC partition.
Add `--variant <name>` when `target.yaml` lists more than one variant.

See [target.yaml reference](target-yaml.md) and
[pre/post programming procedure YAML](procedure-yaml.md) for the input formats.

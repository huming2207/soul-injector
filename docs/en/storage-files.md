# Storage and files

The Soul Injector has a USB port. When connected to a computer, it shows up as
a USB Mass Storage Class (MSC) device.

Due to limitations of the ESP32-S3, USB MSC is very slow. If any of the files
mentioned below was altered, **ALWAYS MAKE SURE** that you unmount/eject the
Soul Injector before you unplug it from your computer.

## Files on device storage

The Soul Injector uses these files **at the root directory of the USB MSC
partition**, mounted internally at `/data`:

- `target.yaml`: required target description and programming configuration.
- `firmware.bin`: firmware image for SWD Cortex-M targets. ESP32-family image
  files are listed in `target.yaml` (for example `bootloader.bin`,
  `partitions.bin`, `firmware.bin`).
- `pre_prog.yaml`: optional procedure to run before programming.
- `post_prog.yaml`: optional procedure to run after self tests.
- `.sha256` sidecars, such as `target.yaml.sha256` or
  `firmware.bin.sha256`: optional `sha256sum` outputs.

If a `.sha256` sidecar exists, the matching file is verified once when assets
are loaded. If the sidecar is absent, the check is skipped. Having sidecars in
place is recommended to avoid flash corruption.

See [target.yaml reference](target-yaml.md) and
[pre/post programming procedure YAML](procedure-yaml.md) for the file formats.

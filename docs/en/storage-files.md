# Jobs and files

The Soul Injector is managed over USB with `sidp-agent`. It shows up on the PC
as a USB serial (CDC) port; it no longer shows up as a USB drive. Files are
uploaded with SHA-256 checks, so nothing needs to be ejected before unplugging.

## One job per device

The device keeps exactly one programming job. A job is compiled on a PC from
`target.yaml`, the optional `pre_prog.yaml`/`post_prog.yaml` and the image
files it programs, then pushed to the device:

```sh
sidp-agent job push --port /dev/ttyACM0 \
    --target target.yaml --pre pre_prog.yaml --post post_prog.yaml \
    --image firmware.bin=build/app.bin
```

- `--image NAME=FILE` names each image the job programs: `firmware.bin` for
  SWD Cortex-M targets, and for ESP32 targets every `images[].path` in
  `target.yaml` (for example `/data/bootloader.bin` needs
  `--image bootloader.bin=...`).
- `--name` sets the name the device reports (default: the variant name);
  `--variant` selects a variant when `target.yaml` lists several.
- `--auto` runs the job whenever a target is plugged in. Without it the job
  only runs on `sidp-agent job run`. The setting survives reboots.

Images the device already holds are not uploaded again, and pushing the job
that is already active only updates `--auto`. To switch products, push the
other product's job; the previous one is replaced.

## Running and checking

```sh
sidp-agent job run --port /dev/ttyACM0 --wait   # one run; non-zero exit unless it passed
sidp-agent job status --port /dev/ttyACM0       # job, trigger, last result
sidp-agent job cancel --port /dev/ttyACM0       # stop the run in progress
sidp-agent device info --port /dev/ttyACM0      # serial, firmware, free storage
```

A cancelled run stops between programming stages and may leave the target
partly programmed. While a run is in progress the device refuses to change the
job or its files.

## What is stored on the device

Under `/data`:

- `job.pb`: the active job and `job.pb.sha256`, its hash.
- The images the job programs, each with a `<name>.sha256` hash file written
  by the device after it checked the upload.

The production log is kept separately under `/log`.

The job pins the SHA-256 of every image. The device refuses to activate a job
whose images are missing or different, and re-checks them at the start of
every run, so replacing an image with `sidp-agent asset push` makes the next
run fail until the matching job is pushed.

## Production log

Every run (pass, fail or cancelled) is recorded in the production log, together
with each boot of the device and its reset reason. The log lives on its own
2 MB `log` partition and survives reboots and job changes. Collect it with:

```sh
sidp-agent log pull --port /dev/ttyACM0 --output production.jsonl
```

New entries are appended to the file, one JSON object per line, tagged with the
device serial number. Only after the file is written does the device mark them
collected; `--no-ack` leaves them on the device to be pulled again.

`log pull`, `job push` and `job run` also set the device clock from the PC,
once per boot. Entries get a `utc_ms` time when the clock was set at any point
during the same boot, including entries written before the PC connected;
otherwise they only carry the time since boot (`uptime_us`).

The device never overwrites entries that have not been collected. The log holds
several thousand runs; when it is full the device shows **LOG FULL**, refuses to
program (`sidp-agent job run` reports it as well), and keeps the record of the
run that did not fit in memory until the log is pulled. `sidp-agent device info`
shows whether entries are waiting.

Planned: target crashes during long debug sessions will be logged in the same
place, as a new entry type.

`sidp-agent compile ... -o job.pb` writes the same job to a file without a
device, for example to inspect or archive it.

See [target.yaml reference](target-yaml.md) and
[pre/post programming procedure YAML](procedure-yaml.md) for the input formats.

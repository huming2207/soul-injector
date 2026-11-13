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
  `--image bootloader.bin=...`). Device file names use only lowercase letters,
  digits, `.`, `_` and `-`, and cannot start or end with `.`: the device's FAT
  file system ignores case and trailing dots, so other spellings would name
  the same file.
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

- `job.pb`: the active job.
- The images the job programs.
- `<name>.part`: an upload in progress. Upload progress is kept in memory, so
  these are deleted when the device restarts.

The production log is kept separately under `/log`.

The job pins the SHA-256 of every image. Before activating a job, and again at
the start of every run, the device hashes each image file and refuses to go on
if one is missing or different. Replacing an image with `sidp-agent asset push`
therefore makes the next run fail until the matching job is pushed, and a file
damaged on the device is caught before the target is erased.

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

`log pull`, `job push` and `job run` also set the device clock, once per boot.
The PC takes the time from an NTP server (`--ntp-server`, default
`pool.ntp.org`) rather than its own clock, so the device needs neither Wi-Fi
nor 4G for it. All times are UTC; they are converted to local time only for
display. Entries get a `utc_ms` time when the clock was set at any point
during the same boot, including entries written before the PC connected;
otherwise they only carry the time since boot (`uptime_us`).

The device never overwrites entries that have not been collected. The log holds
several thousand runs. A run only starts when its record is sure to fit, so
every run that starts is recorded. When the log is full the device shows
**LOG FULL** and refuses to program (`sidp-agent job run` reports it as well)
until the log is pulled; this survives restarts. If writing a record ever
fails, the device shows **LOG ERROR** and refuses to program until it is
restarted. `sidp-agent device info` shows whether entries are waiting.

Planned: target crashes during long debug sessions will be logged in the same
place, as a new entry type.

`sidp-agent compile ... -o job.pb` writes the same job to a file without a
device, for example to inspect or archive it.

See [target.yaml reference](target-yaml.md) and
[pre/post programming procedure YAML](procedure-yaml.md) for the input formats.

# Programming flow

A run programs one target with the device's job. It starts when
`sidp-agent job run` asks for one, or when a target is plugged in and the job
was pushed with `--auto`. Each run goes through these states:

1. Check that the job exists and its images still match the hashes it pins.
2. Run the pre-program steps if the job has any.
3. Detect the target through the selected backend (SWD or ESP32 UART).
4. Erase the target flash.
5. Program the configured firmware image(s).
6. Verify the programmed firmware.
7. Run the self tests listed in the job.
8. Run the post-program steps if the job has any.
9. Finish, or run the optional production-rig current test when enabled.

The programming backend follows the `family` key of the `target.yaml` the job
was compiled from:

- `cortex-m` (default) uses the SWD backend and `/data/firmware.bin`.
- `esp32` uses the UART backend and the image list in `target.yaml`.

`sidp-agent job status` shows the result of the last run: passed, failed (with
the stage that failed) or cancelled, and how long it took. `sidp-agent job
cancel` stops a run between stages. Every finished run is also recorded in the
production log, which `sidp-agent log pull` collects; when the log is full the
device refuses to start a run until it is collected.

See [jobs and files](storage-files.md) for pushing and running the job,
[target.yaml reference](target-yaml.md) for backend selection and
[pre/post programming procedure YAML](procedure-yaml.md) for the optional
step files.

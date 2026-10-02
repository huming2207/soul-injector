# Programming flow

The host runs these states for each target:

1. Load assets from `/data/job.pb` and the firmware/image files.
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

See [storage and files](storage-files.md) for compiling the job,
[target.yaml reference](target-yaml.md) for backend selection and
[pre/post programming procedure YAML](procedure-yaml.md) for the optional
step files.

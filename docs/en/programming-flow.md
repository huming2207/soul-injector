# Programming flow

The host runs these states for each target:

1. Load assets from `/data/target.yaml` and the firmware/image files.
2. Run `pre_prog.yaml` if available.
3. Detect the target through the selected backend (SWD or ESP32 UART).
4. Erase the target flash.
5. Program the configured firmware image(s).
6. Verify the programmed firmware.
7. Run self tests from `target.yaml`.
8. Run `post_prog.yaml` if available.
9. Finish, or run the optional production-rig current test when enabled.

The programming backend is selected from the `family` key in `target.yaml`:

- `cortex-m` (default) uses the SWD backend and `/data/firmware.bin`.
- `esp32` uses the UART backend and the image list in `target.yaml`.

See [target.yaml reference](target-yaml.md) for backend selection and
[pre/post programming procedure YAML](procedure-yaml.md) for the optional
step files.

# 编程流程

一次烧录用设备上的 job 烧录一个目标。`sidp-agent job run` 请求烧录时，或 job
以 `--auto` 推送且插入目标时开始。每次烧录依次执行以下状态：

1. 检查 job 存在，且其镜像仍与 job 固定的哈希一致。
2. 如果 job 中有编程前步骤，执行它们。
3. 通过所选后端（SWD 或 ESP32 UART）检测目标。
4. 擦除目标 flash。
5. 编程配置的固件镜像。
6. 校验已编程的固件。
7. 执行 job 中列出的自检项。
8. 如果 job 中有编程后步骤，执行它们。
9. 完成，或执行可选的生产治具电流测试（若启用）。

编程后端由编译 job 时所用 `target.yaml` 中的 `family` 键决定：

- `cortex-m`（默认）使用 SWD 后端，烧录 `/data/firmware.bin`。
- `esp32` 使用 UART 后端，烧录 `target.yaml` 中列出的镜像列表。

`sidp-agent job status` 显示上次烧录的结果：通过、失败（含失败阶段）或已取消，
以及耗时。`sidp-agent job cancel` 在两个阶段之间停止烧录。每次完成的烧录还会记入生产日志，
由 `sidp-agent log pull` 收集；日志写满时设备拒绝开始烧录，直到日志被收集。

job 的推送与烧录见 [Job 与文件](storage-files.md)，后端选择见 [target.yaml 参考](target-yaml.md)，可选步骤文件见
[pre/post 编程步骤 YAML](procedure-yaml.md)。

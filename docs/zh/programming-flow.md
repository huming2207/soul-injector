# 编程流程

主机对每个目标执行以下状态：

1. 从 `/data/job.pb` 和固件/镜像文件加载资产。
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

job 的编译方法见 [存储与文件](storage-files.md)，后端选择见 [target.yaml 参考](target-yaml.md)，可选步骤文件见
[pre/post 编程步骤 YAML](procedure-yaml.md)。

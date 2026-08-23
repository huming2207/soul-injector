# 存储与文件

Soul Injector 有一个 USB 口。连接到电脑后，它会显示为一个 USB Mass Storage
Class（MSC）设备。

由于 ESP32-S3 的限制，USB MSC 非常慢。如果修改了下面提到的任何文件，
**务必确保**在拔下 Soul Injector 之前先卸载/弹出该 USB 设备。

## 设备存储上的文件

Soul Injector 使用 **USB MSC 分区根目录**下的这些文件，它们会在内部挂载到
`/data`：

- `target.yaml`：必需的目标描述和编程配置。
- `firmware.bin`：SWD Cortex-M 目标的固件镜像。ESP32 家族的镜像文件在
  `target.yaml` 中列出（例如 `bootloader.bin`、`partitions.bin`、
  `firmware.bin`）。
- `pre_prog.yaml`：可选的编程前步骤文件。
- `post_prog.yaml`：可选的自检后步骤文件。
- `.sha256` 伴生文件，如 `target.yaml.sha256` 或 `firmware.bin.sha256`：
  可选的 `sha256sum` 输出。

如果存在 `.sha256` 伴生文件，资产加载时会校验一次对应文件；如果伴生文件
不存在，则跳过校验。建议保留伴生文件以避免 flash 内容损坏。

文件格式见 [target.yaml 参考](target-yaml.md) 和
[pre/post 编程步骤 YAML](procedure-yaml.md)。

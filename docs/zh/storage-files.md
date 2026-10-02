# 存储与文件

Soul Injector 有一个 USB 口。连接到电脑后，它会显示为一个 USB Mass Storage
Class（MSC）设备。

如果修改了下面提到的任何文件，**务必确保**在拔下 Soul Injector 之前先卸载/弹出该 USB 设备。

## 设备存储上的文件

Soul Injector 使用 **USB MSC 分区根目录**下的这些文件，它们会在内部挂载到
`/data`：

- `job.pb`：必需的编程 job 文件，在电脑上用 `sidp-agent` 编译生成。
- `firmware.bin`：SWD Cortex-M 目标的固件镜像。ESP32 家族的镜像文件在
  `target.yaml` 中列出（例如 `bootloader.bin`、`partitions.bin`、
  `firmware.bin`）。
- `.sha256` 伴生文件，如 `job.pb.sha256` 或 `firmware.bin.sha256`：
  可选的 `sha256sum` 输出。

如果存在 `.sha256` 伴生文件，资产加载时会校验一次对应文件；如果伴生文件
不存在，则跳过校验。建议保留伴生文件以避免 flash 内容损坏。

## 生成 job.pb

设备不再读取 YAML。照常编写 `target.yaml` 以及可选的
`pre_prog.yaml`/`post_prog.yaml`，然后在电脑上编译：

```sh
sidp-agent compile --target target.yaml --pre pre_prog.yaml --post post_prog.yaml -o job.pb
sha256sum job.pb > job.pb.sha256   # 可选
```

把 `job.pb`（以及伴生文件，如有）复制到 USB MSC 分区根目录。`target.yaml`
中有多个 variant 时，加上 `--variant <名称>`。

输入文件格式见 [target.yaml 参考](target-yaml.md) 和
[pre/post 编程步骤 YAML](procedure-yaml.md)。

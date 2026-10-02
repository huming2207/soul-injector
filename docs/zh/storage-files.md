# Job 与文件

Soul Injector 通过 USB 用 `sidp-agent` 管理。连接电脑后它显示为一个 USB 串口
（CDC），不再显示为 USB 磁盘。文件上传时带 SHA-256 校验，拔线前无需弹出设备。

## 每台设备一个 job

设备只保存一个编程 job。job 在电脑上由 `target.yaml`、可选的
`pre_prog.yaml`/`post_prog.yaml` 和要烧录的镜像文件编译生成，再推送到设备：

```sh
sidp-agent job push --port /dev/ttyACM0 \
    --target target.yaml --pre pre_prog.yaml --post post_prog.yaml \
    --image firmware.bin=build/app.bin
```

- `--image 名称=文件` 指定 job 烧录的每个镜像：SWD Cortex-M 目标为
  `firmware.bin`；ESP32 目标为 `target.yaml` 中的每个 `images[].path`
  （例如 `/data/bootloader.bin` 需要 `--image bootloader.bin=...`）。
- `--name` 设置设备显示的 job 名称（默认为 variant 名称）；`target.yaml` 中有多个
  variant 时用 `--variant` 选择。
- `--auto`：每次插入目标时自动烧录。不加时只在 `sidp-agent job run` 时烧录。
  该设置重启后保留。

设备上已有的镜像不会重复上传；推送已在使用的 job 只会更新 `--auto` 设置。
更换产品时推送另一个产品的 job 即可，旧 job 会被替换。

## 烧录与查看

```sh
sidp-agent job run --port /dev/ttyACM0 --wait   # 烧录一次；未通过时以非零值退出
sidp-agent job status --port /dev/ttyACM0       # job、触发方式、上次结果
sidp-agent job cancel --port /dev/ttyACM0       # 停止正在进行的烧录
sidp-agent device info --port /dev/ttyACM0      # 序列号、固件版本、剩余空间
```

取消会在两个烧录阶段之间生效，目标可能只烧录了一部分。烧录进行中设备拒绝修改
job 或其文件。

## 设备上保存的内容

位于 `/data`：

- `job.pb`：当前 job，以及其哈希文件 `job.pb.sha256`。
- job 烧录的镜像，每个镜像都有设备在校验上传后写入的 `<名称>.sha256`。

job 固定了每个镜像的 SHA-256。镜像缺失或不一致时设备拒绝启用该 job，并且每次
烧录开始时都会重新比对；因此用 `sidp-agent asset push` 替换镜像后，下次烧录会失败，
直到推送与之匹配的 job。

`sidp-agent compile ... -o job.pb` 不连接设备，把同一个 job 写入文件，用于检查或存档。

输入文件格式见 [target.yaml 参考](target-yaml.md) 和
[pre/post 编程步骤 YAML](procedure-yaml.md)。

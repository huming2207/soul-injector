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
  （例如 `/data/bootloader.bin` 需要 `--image bootloader.bin=...`）。设备上的文件名
  只能用小写字母、数字、`.`、`_` 和 `-`，且不能以 `.` 开头或结尾：设备的 FAT 文件系统
  不区分大小写并忽略结尾的点，其它写法会指向同一个文件。
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

- `job.pb`：当前 job。
- job 烧录的镜像。
- `<名称>.part`：正在上传的文件。上传进度只保存在内存中，设备重启时会删除这些文件。

生产日志单独存放在 `/log`。

job 固定了每个镜像的 SHA-256。启用 job 之前以及每次烧录开始时，设备都会计算每个
镜像文件的哈希，镜像缺失或不一致时不再继续。因此用 `sidp-agent asset push` 替换镜像后，
下次烧录会失败，直到推送与之匹配的 job；设备上损坏的文件也会在擦除目标之前被发现。

## 生产日志

每次烧录（通过、失败或取消）都会记入生产日志，每次开机及其复位原因也会记录。
日志位于独立的 2 MB `log` 分区，重启和更换 job 后都保留。用以下命令收集：

```sh
sidp-agent log pull --port /dev/ttyACM0 --output production.jsonl
```

新条目追加到文件中，每行一个 JSON 对象，带设备序列号。文件写好之后设备才把这些
条目标记为已收集；`--no-ack` 则保留在设备上，下次会再次读出。

`log pull`、`job push` 和 `job run` 还会设置设备时钟（每次开机一次）。时间由电脑从 NTP
服务器获取（`--ntp-server`，默认 `pool.ntp.org`），而不是用电脑自己的时钟，因此设备
不需要 Wi-Fi 或 4G。所有时间都是 UTC，只在显示时才转换为本地时间。
同一次开机中只要设置过时钟，条目就有 `utc_ms` 时间，包括电脑连接之前写入的条目；
否则只有开机后的时间（`uptime_us`）。

设备从不覆盖未收集的条目。日志可容纳数千次烧录。只有确定放得下记录时才开始烧录，
所以每次开始的烧录都会被记录。日志写满时设备显示 **LOG FULL** 并拒绝烧录
（`sidp-agent job run` 也会提示），直到日志被收集，重启后依然如此。如果写入记录失败，
设备显示 **LOG ERROR** 并拒绝烧录，直到重启。`sidp-agent device info` 显示是否有待收集的条目。

后续计划：长时间调试中的目标崩溃等事件也将作为新的条目类型记入同一日志。

`sidp-agent compile ... -o job.pb` 不连接设备，把同一个 job 写入文件，用于检查或存档。

输入文件格式见 [target.yaml 参考](target-yaml.md) 和
[pre/post 编程步骤 YAML](procedure-yaml.md)。

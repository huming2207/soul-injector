# 快速开始

SoulInjector 是一个基于 ESP32-S31 的离线编程器，支持：

- 通过 SWD 和 flash algorithm 编程 ARM Cortex-M 目标。
- 通过 UART、ROM bootloader 和 flasher stub 编程 Espressif 目标。

电脑通过 USB 用 `sidp-agent` 推送编程 job 及其固件镜像；之后设备独立烧录目标，
按请求烧录或在插入目标时自动烧录。每次烧录都记入生产日志，由电脑稍后收集。

## 硬件版本构建

在配置或构建项目时选择板卡版本：

```sh
idf.py --preview -B build-rev71 -D SI_HW_REV=rev71 build
```

目前唯一支持的版本是 `rev71`，也是默认值。需要 ESP-IDF v6.1 或更新版本；
ESP32-S31 在该版本中仍是 preview 目标。每个版本使用各自构建目录下的
`sdkconfig`，因此以后新增版本时不会沿用其他板卡的引脚配置。

Rev 7.1 配置启用了分体式、SN74AXC2T245 电平转换的 SWD 接口，并使用
Rev 7.1 KiCad 原理图中的 GPIO 分配。其 NT279VJ-C10-01-V1 LCD 使用 NV3007
面板驱动，默认启用。

ESP-IDF v6.1 的 FatFs 关闭了生产日志需要的 `f_expand()`。在 Espressif 发布上游
commit `74a7a4b` 之前，请在 ESP-IDF 的 `components/fatfs/src/ffconf.h` 中设置
`#define FF_USE_EXPAND 1`，否则链接时报 `undefined reference to 'f_expand'`。

## Flash 布局

`partitions.csv` 对 16 MB flash 的分配：

- `factory`：2 MB 应用程序。
- `nvs`：256 KB 设置（job 的 trigger）。
- `data`：5 MB FAT，挂载于 `/data`，保存 job 及其镜像。
- `log`：2 MB FAT，挂载于 `/log`，保存生产日志。

新分区加在已有分区之后，因此用 `idf.py flash` 烧录新固件时 `data` 上的 job 和镜像
会保留。

## 其它板卡

ESP32-S3 板卡（Rev 3、Rev 5 和 Rev 6）不再从此分支构建，最后的固件版本为
`legacy-s3-yaml` tag。

## 术语

除非另有说明：

- **目标（target）** 指被编程的微控制器：SWD 下的 ARM Cortex-M 设备，或
  UART 下的 Espressif 芯片。
- **主机（host）** 指 Soul Injector 设备本身。
- **电脑（PC）** 指运行 `sidp-agent`、通过 USB 连接 Soul Injector 的计算机。

# 快速开始

SoulInjector 是一个基于 ESP32-S3 的离线编程器，支持：

- 通过 SWD 和 flash algorithm 编程 ARM Cortex-M 目标。
- 通过 UART、ROM bootloader 和 flasher stub 编程 Espressif 目标。

它从设备存储分区加载固件和目标配置，无需上位机即可运行编程流程。

## 硬件版本构建

在配置或构建项目时选择板卡版本：

```sh
idf.py -B build-rev6 -D SI_HW_REV=rev6 build
```

支持的版本有 `rev3`、`rev5` 和 `rev6`；默认仍为 `rev5`。每个版本使用各自
构建目录下的 `sdkconfig`，因此切换版本不会沿用其他板卡的引脚配置。

Rev 6 配置启用了分体式、SN74AXC2T245 电平转换的 SWD 接口，并使用 Rev 6
KiCad 原理图中的 GPIO 分配。其 NT279VJ-C10-01-V1 LCD 使用 NV3007 面板驱动，
默认启用。

## 术语

除非另有说明：

- **目标（target）** 指被编程的微控制器：SWD 下的 ARM Cortex-M 设备，或
  UART 下的 Espressif 芯片。
- **主机（host）** 指 Soul Injector 设备本身。

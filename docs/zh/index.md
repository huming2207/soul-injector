# SoulInjector 文档

## 快速开始

- [快速开始](getting-started.md) — 项目简介、硬件版本构建、flash 布局和术语说明。

## 配置

- [target.yaml 参考](target-yaml.md) — 目标选择、flash algorithm、ESP32
  镜像配置和自检项。在电脑上编译成 `job.pb`。
- [pre/post 编程步骤 YAML](procedure-yaml.md) — `pre_prog.yaml` 与
  `post_prog.yaml` 的步骤格式和执行规则。

## 运维

- [编程流程](programming-flow.md) — 每个目标执行的状态机流程。
- [Job 与文件](storage-files.md) — 用 `sidp-agent` 推送、烧录和查看设备上的 job，
  `/data` 上保存的文件，以及收集生产日志。

## 工程笔记

- [后续低功耗优化](low-power-optimisation.md) — S31 与模组睡眠、MAIN_RI
  唤醒、建议变更及测量计划。

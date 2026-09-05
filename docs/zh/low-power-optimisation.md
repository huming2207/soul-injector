# 后续低功耗优化

[English](../en/low-power-optimisation.md)

本文记录 Rev 7.1 及后续硬件的低功耗优化方向，适用于 **ESP32-S31**（不是
ESP32-S3）和 Quectel EG800Z。依据为 2026-09-05 审查的项目代码及本地
ESP-IDF v6.1 源码。以下内容是后续工作建议，不代表已经实现或通过硬件验证。

## 基线与测量条件

此前报告的板级测量结果约为：

| 状态 | 报告电流 |
| --- | --- |
| ESP 浅睡眠，模组关机 | 600 µA |
| ESP 深睡眠，模组 `QSCLK=1` | 6 mA |
| ESP 浅睡眠并启用 UART FIFO 唤醒，模组 `QSCLK=1` | 11–12 mA |
| 移除 UART 唤醒配置，模组 `QSCLK=1` | 5–6 mA |

这些结果来自不同测试，并非严格控制条件下的器件功耗拆分。比较时应保持供电电压、
测量点、网络条件和平均时间窗口一致。不同电压电源轨上的电流不能直接相加；比较
输入功率时还需考虑电源转换效率。

本次审查的代码实际发送 `AT+QSCLK=3,5,"10100001","00000001"`，随后在
`setup_modem()` 返回后立即调用 `esp_deep_sleep(1000000000)`。该命令请求
一分钟的 TAU 周期和两秒的活动时间，ESP 则深睡眠 1,000 秒。实际定时器取值受
网络分配影响。这与此前的 `QSCLK=1` 测试不同。ESP 深睡眠前没有等待网络注册，
模组可能仍在注册过程中；ping 任务等待 PPP 连接，在此路径下不会发送数据。

应检查的配置是 `build-rev71/sdkconfig.rev71`，而非根目录的 `sdkconfig`。
每次采集都应记录实际烧录的固件版本和配置。

## 优先级

| 优先级 | 建议变更 | 目的与验证要求 |
| --- | --- | --- |
| 1 | 用 LP GPIO 上的 MAIN_RI 替代 HP UART FIFO 唤醒 | 允许关闭 UART 相关时钟；验证 URC 和 PPP 接收行为 |
| 2 | 关闭 DTR 内部下拉 | 消除 DTR 输出高电平时的电阻负载 |
| 3 | 将 UART 任务的 100 ms 轮询改为事件等待 | 减少无意义唤醒，同时保留任务停止及接收恢复机制 |
| 4 | 试验更长的睡眠时钟校准间隔 | 减少入睡开销，并验证计时与稳定性 |
| 5 | 配置 UART 状态保持及外设域掉电 | 降低 ESP 剩余睡眠电流，确认实际发生掉电 |
| 6 | 分别测量模组与接口负载 | 定位剩余电流及 VDD_EXT 电压偏低的原因 |

### UART 唤醒与 MAIN_RI

审查的 IDF 在配置 HP UART FIFO 唤醒时，会保持 XTAL 供电并开启 UART 和
IO-MUX 时钟。S31 PMU 在 XTAL 保持供电时还会调整模拟电路的睡眠参数。因此，
测得的差值不只是 UART 接收器本身的电流。`esp_pm_dump_locks()` 无法显示所有
睡眠时钟及电源域要求。运行时更换唤醒模式，需要撤销旧模式配置；仅禁用唤醒源
并不等同于撤销 `uart_wakeup_setup()` 的设置。

Rev 7.1 已将 MAIN_RI 接到 U9/TXS0108E 的 A4，B4（16 脚）尚未连接。
下一版可将该电平转换后的信号接到空闲的 S31 LP GPIO，也可先飞线验证。若不再
需要其扩展接口功能，GPIO2 是一个候选。在模组或转换器关闭时，ESP 侧应保持
确定的非触发高电平；可验证弱外部上拉方案与转换器的兼容性。不要将模组的
1.8 V 引脚直接上拉到 3.3 V。

| 睡眠配置 | 唤醒引脚选择 |
| --- | --- |
| 普通浅睡眠，外设域保持供电 | 任意空闲且有效的 GPIO |
| 外设域掉电的浅睡眠，或深睡眠 | S31 LP/RTC GPIO0–GPIO7 |

外设域掉电时，将引脚配置为输入，并使用：

```cpp
ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
    BIT64(CELL_MAIN_RI_PIN), ESP_GPIO_WAKEUP_GPIO_LOW));
```

数字外设域关闭时，不应采用普通的 `gpio_wakeup_enable()` 加
`esp_sleep_enable_gpio_wakeup()` 路径。按所选 API 的说明配置上下拉，包括使用
外部上拉时如何设置 `CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS`。

Quectel 文档规定 MAIN_RI 空闲为高，在 UART 输出 URC/SMS 数据前保持至少
120 ms 的低电平。`AT+QCFG="risignaltype","physical"` 用于配置物理 URC
指示，还需检查 URC 输出端口及 RI 设置。**这不代表每个 PPP 数据突发都会获得
相同的提前通知。** 将 RI 作为唯一唤醒源前，必须抓取 PPP 下行期间的 RI、RX、
TX、DTR 和 RTS，验证流控能否在 ESP UART 就绪前阻止数据丢失。ESP 应在
RI 到数据之间的间隔以及接收事务期间保持唤醒，避免过早再次入睡，也要避免 RI
持续为低导致反复触发电平唤醒。

### GPIO 电平与电平转换器负载

审查的 `modem_manager::init()` 对 DTR 使用 `GPIO_PULLDOWN_ONLY`，而
`QuectelDTE::allow_sleep()` 又将其输出拉高。应关闭这个方向相反的内部下拉，
并确保睡眠时 DTR 保持高电平。PWRKEY/reset 应保持释放状态，RTS 应有明确的
睡眠状态；ESP 不应将模组输出信号配置为输出并驱动。

针对 S31 的修正：当前 PMU 配置会在浅睡眠关闭 TOP 域时自动保持 HP 和 LP
引脚状态。不要在未检查该机制的情况下给所有输出增加手动 hold。入睡前先设置
正确的电平和上下拉，再进行实测。普通 GPIO 睡眠隔离和 TOP 域掉电是不同路径。

此前测得的 VDD_EXT 为 1.6 V，原因仍未确定。Quectel 规定其范围为
1.71–1.89 V、最大输出电流为 4 mA，并将其用途描述为外部上拉电路供电。
需要核实它为 U9 供电的适用性及实际负载，并分别在空闲和活动时测量模组引脚及
转换器电源端电压。即使不存在输出对打，TXS0108E 内部上拉也会在信号为低时
消耗电流。移除 UART 唤醒开销，并不能证明这个独立的电气问题已经解决。
如果 MAIN_RI 仍通过 U9 到达 ESP，就不能在睡眠时关闭 U9。

### 周期唤醒与时钟校准

项目内的 esp-modem UART 任务执行 `get_event(event, 100)`，超时后检查接收
缓冲区。即使没有数据，也会每秒唤醒任务约十次。其停止握手同样依赖退出这次
等待。改为事件驱动时，必须提供显式的停止唤醒机制，并保留必要的接收缓冲区
恢复逻辑；直接改成无限等待可能破坏资源释放流程。

当前配置为 `CONFIG_PM_LIGHTSLEEP_RTC_OSC_CAL_INTERVAL=1`。取得基线后，
可先试验 16，对比反复睡眠期间的电流和计时。更长的间隔减少校准开销，但不能
同样及时地跟踪振荡器漂移。板上已有的 32.768 kHz 晶振也值得评估，因为当前
选择的是内部 RC。不要认为切换到晶振就会消除所有校准；睡眠路径还会校准
RC_FAST。

### 外设状态保持与存储器

当前配置已启用 CPU 掉电、flash deep-power-down 命令及 PSRAM half-sleep，
尚未启用数字外设域掉电。

开启 `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` 前，需要通过
`uart_config.flags.allow_pd` 配置 UART 状态保持。项目内 esp-modem 将此标志
保留为零，并且在 `uart_driver_install()` 前调用 `uart_param_config()`。
应按 IDF 状态保持资源的初始化要求检查调用顺序；若 retention 模块尚未初始化，
仅设置该标志仍不够。检查分配警告及实际电源域行为。其他已初始化驱动也可能
阻止掉电，或需要相应的状态保持支持。

该选项并不关闭整块板的电源。LP 唤醒逻辑和保留的内存仍需供电；PSRAM 必须
保留应用的有效数据。LCD、LED、转换器及稳压器需要分别处理。避免初始化不使用
的外设，但不要认为跳过驱动就消除了外部器件负载。测量时可对比断开 USB、关闭
不必要控制台输出的结果，并在测量窗口之外恢复诊断输出。

### 模组睡眠策略

普通睡眠基线使用 `QSCLK=1`。试验 PSM 时，应明确选择 TAU 周期，允许重新
注册，并确认实际进入 PSM。Quectel 说明 `QSCLK?` 返回请求/参考定时器值，
网络实际分配值应使用 QLog 检查。测量连续睡眠区间时，不要反复轮询 AT 命令。

本次审查的初始化没有发送 `AT+ECSIMCFG="SimPowerSave",1`；应检查模组的
实际设置，不要假设之前的测试设置仍然有效。若 PSM 使用 DTR 唤醒，可在确认
固件支持及唤醒行为后评估 `AT+QPSMUART=0`，关闭模组侧 UART 唤醒。它与
ESP 的 UART 唤醒不是同一个设置。模组 USB 的连接条件也应符合 Quectel
低功耗文档要求。

恢复 PPP 和周期 ping 后，应测量完整的发送至再次入睡周期。无线传输和等待
网络释放连接的时间可能主导平均电流。在产品延迟要求允许的情况下，合并通信
或延长上报周期。临时使用 `CFUN=0` 测量可帮助分离无线/网络活动与其他负载，
但此状态不能提供正常蜂窝连接。

## LP UART：需要单独 PoC 的选项

审查的 S31 实现具有 16 字节 LP UART FIFO，支持 FIFO 阈值、边沿阈值、
起始位和字符序列唤醒。信号可通过 LP GPIO matrix 路由到 GPIO0–GPIO7。
原生 TX/RX 是 GPIO6/7，RTS/CTS 是 GPIO4/5；现有模组的 GPIO42/43/46/47
无法直接作为 LP UART 引脚。

本地 S31 能力定义没有启用 `esp_sleep_enable_uart_wakeup()` 的直接 LP UART
分支。审查到的支持路径是 LP UART 唤醒 LP 核心，再由 LP 核心调用
`ulp_lp_core_wakeup_main_processor()`，主 CPU 则启用 ULP 唤醒。标准 LP
启动流程在处理 S31 持续触发的唤醒状态时会清空 UART 缓冲区，因此保留触发
唤醒的模组数据需要额外处理。16 字节 FIFO 配合 921600 波特率，也需要仔细
验证流控及响应延迟。

LP UART FIFO 唤醒仍需运行接收时钟。应实测其 S31 功耗；目前没有可用于比较
LP UART 与 RI 的实测结果。不能把它视为可直接替换 HP UART 唤醒、且不会丢失
数据的方案。

## 验证顺序

1. 记录固件/配置、供电测量点、USB 连接及模组模式。
2. 先测模组关闭时的 ESP 基线，再测模组稳定进入 `QSCLK=1` 的状态；分别记录
   静默区间电流和积分平均电流。
3. 移除 DTR 下拉，实际测量 DTR 和 VDD_EXT。
4. 对比普通浅睡眠和定时器唤醒的外设域掉电浅睡眠。MAIN_RI 尚未接线时也能
   进行此测试，但它无法提供异步模组接收唤醒。
5. 分别比较 UART 轮询和时钟校准变更的效果。
6. 反复测试 MAIN_RI 唤醒，覆盖 URC、PPP 下行、主机发送、重连和模组电源
   循环，检查丢字节及反复唤醒问题。
7. 单独比较深睡眠，并考虑 GPIO 状态变化、重启及重新初始化的能量开销。
   不要仅用两种不同睡眠模式的差值推断单个器件电流。
8. 逐步恢复生产环境通信和外设，采集多个完整上报周期。

## 依据与参考资料

- 项目：[modem manager](../../main/comm/modem_manager.cpp)、
  [Quectel DTE](../../main/comm/quectel_dte.cpp)、
  [bootstrap](../../main/bootstrap_fsm.cpp)、
  [ping test](../../main/comm/ping_test.cpp)。
- 审查使用的本地 IDF：`/home/hu/esp/esp-idf`。相关文件：
  `components/esp_driver_uart/src/uart_wakeup.c`、
  `components/esp_driver_uart/src/uart.c`、
  `components/esp_hw_support/port/esp32s31/pmu_sleep.c`、
  `components/esp_hw_support/port/esp32s31/private_include/pmu_param.h`、
  `components/esp_pm/Kconfig`、
  `components/ulp/lp_core/lp_core/lp_core_utils.c`。
- [Espressif S31 硬件指南](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s31/schematic-checklist.html#gpio)。
- [TI TXS0108E 资料](https://www.ti.com/product/TXS0108E)。
- `/home/hu/Downloads/quectel` 中的本地 Quectel 文档：
  `Quectel_EG800Z_Series_Low_Power_Mode_Application_Note_V1.0.0.pdf`
  （§§2.1.3–2.1.5、3.2–3.3），以及
  `Quectel_EG800Z_Series_Hardware_Design_V1.2.pdf`
  （VDD_EXT 引脚规格及 §4.9.3 MAIN_RI）。

升级 IDF 后应重新检查 S31 源码和 Espressif 指引；能力标志、状态保持实现及
LP UART 启动行为都可能发生变化。

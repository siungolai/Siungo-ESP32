# Siungo-ESP32 · status-oled3

本项目 context 的术语表。**只收本项目独有的概念**；通用编程概念（timeout、retry、JSON）不属于这里。

命名约定沿用仓库规则：引用硬件事实必须带 [`../../HARDWARE.md`](../../HARDWARE.md) 的 `§节号` 定位；引用决策必须带 ADR 编号；三屏拓扑术语（**上游 / 通道 / 选通 / 断开**）沿用 [`../pet-oled3/CONTEXT.md`](../pet-oled3/CONTEXT.md)，本表不重复定义。

## 硬件层

**聚焦屏**：
当前接收按钮操作的那一块屏，标题栏以反色标记。上电默认为 ESP32 状态屏。
_Avoid_: 当前屏、活动屏、选中屏

**页**：
一块屏内的一份内容单位。服务器屏有多页（CPU / 内存 / 磁盘 / 网络），ESP32 状态屏与邮箱屏各一页。
_Avoid_: 界面、画面、屏、section

**翻页**：
聚焦屏在自身页之间循环切换的动作，由按钮 A 短按触发。页号越界时回到第一页。
_Avoid_: 切页、下一页、scroll

**有源蜂鸣器**：
内部自带振荡电路、通直流电即发出固定频率声音的蜂鸣器。本项目已实测为此类型，只能通过**通断时长**编码信息，无法通过 PWM 改变音高。接 GPIO 12，见 [ADR-0007](docs/adr/0007-gpio-assignment-buttons-and-buzzer.md)。
_Avoid_: 蜂鸣器、buzzer（未加定语时含义模糊）、有源喇叭

**轻触按钮**：
按下导通、松开断开的瞬时接触开关。本项目使用两只，分别承担翻页与切换聚焦屏。接 GPIO 10 / GPIO 11，内部上拉、按下接地为低电平，见 [ADR-0007](docs/adr/0007-gpio-assignment-buttons-and-buzzer.md)。
_Avoid_: 按键、开关、button（泛指）、键盘

**空闲脚**：
本机未被任何既有项目占用、且不在禁区表内的 GPIO。按 [`../../HARDWARE.md`](../../HARDWARE.md) §1.2 核定，本项目实际取用 GPIO 10 / 11 / 12 三个连号脚（GPIO 45/46 为 strapping 脚、GPIO 47 紧邻 strapping 未采用），完整候选与排除依据见 [ADR-0007](docs/adr/0007-gpio-assignment-buttons-and-buzzer.md)。
_Avoid_: 可用引脚、空脚、free pin

## 数据层

**停更**：
聚合端点返回的数据在设备侧超过约定时限没有更新。服务器宕机、端点故障、设备断网、TLS 握手失败（含**证书过期**）、DNS 解析失败都会表现为停更——停更是**可观测的结果，不是原因**。设备侧的诊断责任是区分「连不上」与「连上了但数据不对」，屏上只显示最后更新时间，具体阈值见 [ADR-0004](docs/adr/0004-fixed-server-polling-and-staleness.md)。
_Avoid_: 超时、掉线、断线、失联

**聚合端点**：
运行在被监控的阿里云主机上的一个接口，读取本机指标与各邮箱未读数，输出一个 JSON 供设备轮询。设备侧不持有任何服务端凭据。部署形态见 [ADR-0005](docs/adr/0005-aggregator-as-standalone-service-with-shared-secret.md)，地址见 [ADR-0006](docs/adr/0006-aggregator-url-pinning.md)。
_Avoid_: API、接口、后端、探针、agent

**最后更新时间**：
设备成功收到一份有效数据的时刻。停更判定与屏上提示都以它为基准，显示为"多久之前"。
_Avoid_: 时间戳、最后同步、last seen

**邮箱未读数**：
一个邮箱中尚未读过的邮件数量，由聚合端点代查。本项目只关心这个数字，不取邮件内容。M1 先接 1–2 个主要邮箱并汇总为「未读总数」，邮箱清单与授权码只存在于服务器侧，不进固件。
_Avoid_: 邮件数、新邮件、inbox count

**轮询周期**：
设备向聚合端点发起一次 GET 的时间间隔。本项目固定为 **15 秒**，停更阈值 **60 秒**（4 次无响应），见 [ADR-0004](docs/adr/0004-fixed-server-polling-and-staleness.md)。
_Avoid_: 刷新率、采样率、拉取间隔

## 设备状态层

**本机状态**：
ESP32 自身可自省的信息：通电时长、WiFi 信号强度、IP、剩余堆内存、三块屏各自的在线情况。不依赖任何外部请求。
_Avoid_: 系统状态、设备状态、自检

**屏在线**：
某通道上的 SSD1306 在启动探测时应答正常，且后续 `flush()` 能正常完成。离线的屏 `flush()` 立即返回，不产生任何 I2C 事务（沿用 `pet-oled3` 的 **掉屏 Screen Absent** 语义，见 [`../pet-oled3/CONTEXT.md`](../pet-oled3/CONTEXT.md)）。
_Avoid_: 屏正常、屏挂了、connected

**通知提醒**：
蜂鸣器发出的一次声音事件，对应一个具体的状态变化（新邮件、服务器停更、设备断网等）。节奏映射见 [ADR-0003](docs/adr/0003-active-buzzer-on-off-encoding.md)。
_Avoid_: 提示音、音效、告警、alarm

## 里程碑

**M0（硬件 bringup）**：
在写任何界面代码之前，先把 PCA9548A + 三屏链路、两个按钮、蜂鸣器全部实测跑通。理由：三屏链路在本机从未验证（[`../pet-oled3/README.md`](../pet-oled3/README.md) 只到「规格已核实」），按钮与蜂鸣器在 [`../../HARDWARE.md`](../../HARDWARE.md) 中零记录。先验证硬件再写上层，避免「代码写完发现屏不亮」。
_Avoid_: 阶段一、第一阶段、phase 0

**M1（数据链路）**：
聚合端点在服务器侧上线（[ADR-0005](docs/adr/0005-aggregator-as-standalone-service-with-shared-secret.md)），设备侧轮询打通（[ADR-0004](docs/adr/0004-fixed-server-polling-and-staleness.md)）。邮箱先接 1–2 个主要邮箱，汇总为未读总数。
_Avoid_: 阶段二、联调

**M2（完整界面）**：
三屏完整内容、聚焦屏翻页交互、蜂鸣器全部提示节奏落地。
_Avoid_: 阶段三、收尾

# Siungo-ESP32 · status-oled3

本项目 context 的术语表。**只收本项目独有的概念**；通用编程概念（timeout、retry、JSON）不属于这里。

命名约定沿用仓库规则：引用硬件事实必须带 [`../../HARDWARE.md`](../../HARDWARE.md) 的 `§节号` 定位；引用决策必须带 ADR 编号；三屏拓扑术语（**上游 / 通道 / 选通 / 断开**）沿用 [`../pet-oled3/CONTEXT.md`](../pet-oled3/CONTEXT.md)，本表不重复定义。

## 硬件层

**聚焦屏**：
当前接收按钮操作的那一块屏，标题栏以反色标记。上电默认为 ESP32 状态屏。
_Avoid_: 当前屏、活动屏、选中屏

**页**：
一块屏内的一份内容单位。ESP32 状态屏与邮箱屏各一页；服务器屏两页（CPU/内存/负载 + 磁盘/网络/服务探活）。
_Avoid_: 界面、画面、屏、section

**翻页**：
聚焦屏在自身页之间循环切换的动作，由按钮 A 短按触发。页号越界时回到第一页。**只有聚焦屏有可翻的页**——按钮 A 的语义永远是「翻聚焦屏」，不是「翻某一块指定的屏」。
_Avoid_: 切页、下一页、scroll

**有源蜂鸣器**：
内部自带振荡电路、通直流电即发出固定频率声音的蜂鸣器。本项目已实测为此类型（两端通 3.3V 即响），只能通过**通断时长**编码信息，无法通过 PWM 改变音高。接 GPIO 21，见 [ADR-0007](docs/adr/0007-gpio-assignment-buttons-and-buzzer.md)。
_Avoid_: 蜂鸣器、buzzer（未加定语时含义模糊）、有源喇叭

**轻触按钮**：
按下导通、松开断开的瞬时接触开关。本项目使用两只，分别承担翻页与切换聚焦屏。按钮 A 接 GPIO 6、按钮 B 接 GPIO 7，内部上拉、按下接地为低电平，见 [ADR-0007](docs/adr/0007-gpio-assignment-buttons-and-buzzer.md)。
_Avoid_: 按键、开关、button（泛指）、键盘

**空闲脚**：
本机未被任何既有项目占用、且不在禁区表内的 GPIO。按 [`../../HARDWARE.md`](../../HARDWARE.md) §1.2 核定，本项目实际取用 GPIO 6 / 7 / 21 三个空闲脚（GPIO 12 是 MTDI strapping 脚，已避开），完整候选与排除依据见 [ADR-0007](docs/adr/0007-gpio-assignment-buttons-and-buzzer.md)。
_Avoid_: 可用引脚、空脚、free pin

## 数据层

**停更**：
聚合端点返回的数据在设备侧超过约定时限没有更新。服务器宕机、端点故障、设备断网、TLS 握手失败（含**证书过期**）、DNS 解析失败都会表现为停更——停更是**可观测的结果，不是原因**。设备侧的诊断责任是区分「连不上」与「连上了但数据不对」，屏上只显示最后更新时间，具体阈值见 [ADR-0004](docs/adr/0004-fixed-server-polling-and-staleness.md)。
_Avoid_: 超时、掉线、断线、失联

**聚合端点**：
运行在被监控的阿里云主机上的一个接口，读取本机指标与各邮箱未读数，输出一个 JSON 供设备轮询。设备侧不持有任何服务端凭据。**部署形态已被 [ADR-0008](docs/adr/0008-server-side-snapshot-via-cron.md) 取代**（原 ADR-0005 的独立 systemd 服务方案已被用户否决），地址见 [ADR-0006](docs/adr/0006-aggregator-url-pinning.md)。
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

**多路复用器（PCA9548A）**：
接在 I2C 上游、把一条总线分成八路通道的芯片，本项目用来带三块地址相同的 SSD1306。三块屏分占通道 0 / 1 / 2，术语与拓扑见 [`../pet-oled3/CONTEXT.md`](../pet-oled3/CONTEXT.md)，接线见 [`../../HARDWARE.md`](../../HARDWARE.md) §4.5。**2026-10-17 已实测可用**（`pca9548a 0x70 : online`，ch0/ch1/ch2 三块屏全部 online）。
**上游 SDA/SCL 线序（2026-10-17 bench 实测）**：实物接线是 **GPIO4 → 模块 SCL、GPIO5 → 模块 SDA**（用户回报原文「g4连scl;g5连sda」），与初版策划案的 GPIO8/9 及固件最初的 `I2C_SDA_PIN=4 / I2C_SCL_PIN=5` 都不同。故 `config.h` 现为 `I2C_SDA_PIN = 5` / `I2C_SCL_PIN = 4`。**两线交叉时总线电气完全正常**（idle HIGH/HIGH、上拉在线、轨 3152mV、全段 `nack_addr` 且 `timeout=0`），只是没有器件能应答——I2C 的 START/STOP/ACK 全定义在「SDA 相对 SCL 的边沿」上，互换后从机永远识别不出 START。**以后 0x70 不应答，先查线序再查别的**（见下条）。
**v6 空总线对照（2026-10-17）**：GPIO10/GPIO11 本项目无接线，作空总线对照扫描，三趟（空总线 / 模块总线 / 模块总线重扫）结果逐字一致（全段 `nack_addr=112 timeout=0`，0x70→2、0x3C→2），但 `idle` 分别是 LOW/LOW（浮空）与 HIGH/HIGH（外部上拉在线）。⟹ ESP32 I2C 外设与固件配置正常、SDA/SCL 两根线与模块导通且上拉接着带电的轨，但 0x70 全程 NACK。
**v7 线序测试（2026-10-17，破案）**：三趟扫描各自独立 `Wire.end()`——A1 空总线（对照）/ A2 `SDA=GPIO5 SCL=GPIO4`（匹配实物）/ A3 `SDA=GPIO4 SCL=GPIO5`（固件原状）。结果 **A2 的 0x70 → ACK，A3 全段 NACK**，一次 flashing 定案。之前所有「对调」pass 都因缺 `Wire.end()` 而是假的（见下条），所以这个组合此前从未真正测过。详见 `status-oled3/.doc/i2c-absent-investigation-2026-10-17.md`。
_Avoid_: 扩展板、IO 扩展、hub

**Wire.end() 与 I2C 重初始化**：
框架 `Wire.cpp:300-303` 的 `begin()` 在 `i2cIsInit(num)` 为真时直接跳到函数尾返回 true，**从不调用 `initPins()`**；`Wire.cpp:118-144` 的 `setPins()` 同理只打 `log_e("bus already initialized. change pins only when not.")`。因此**在本框架里换脚位或重新初始化 I2C，必须先 `Wire.end()`（`i2cDeinit` + `freeWireBuffer`）再 `Wire.begin()`**，否则第二次 `begin()` 是彻底 no-op，扫到的是上一次 `Wire.begin()` 留下的引脚与状态。
2026-10-17 v4/v5 探针因缺 `Wire.end()` 报出假的「全段 error 5（bus timeout）」并在 100kHz 段静默挂死，当时误判为 ADC 段干扰；v6 补上 `Wire.end()` 后全段恢复 `error 2`。**教训：换脚位 = `Wire.end()` + `Wire.begin()`，缺一不可。**
_Avoid_: 重新 begin、re-init

**逻辑屏 / 物理面板**：
两个不同的概念，单屏拓扑下尤其容易混。**逻辑屏**是软件里的三个屏幕——ESP32 状态屏、服务器屏、邮箱屏，各有自己的绘制函数、分页与刷新节奏，任何拓扑下都是三个。**物理面板**是真正接线的那块 SSD1306：三屏拓扑下三块，单屏拓扑下一块。
**单屏直连**（`config::ACTIVE_SCREENS = 1`）：多路复用器不在信号路径里，一块面板直连 GPIO4/GPIO5，三个逻辑屏共用它，按钮 B 选哪个逻辑屏显示。此时 `screen::showable()` 取代 `screen::online()` 成为「该不该画它」的判据——只有聚焦屏 showable，否则两个非聚焦逻辑屏每秒的脏标记会把用户正在看的那一屏覆盖掉。切回三屏只改这一个常量。
_Avoid_: 主屏、子屏、虚拟屏

**通知提醒**：
蜂鸣器发出的一次声音事件，对应一个具体的状态变化（新邮件、服务器停更、设备断网等）。节奏映射见 [ADR-0003](docs/adr/0003-active-buzzer-on-off-encoding.md)。
**按钮反馈**（2026-10-17 补）：两个按钮每一次被接受的短按都极短响 1 次（20ms），**不论它是否改变了屏幕内容**。修订前只给按钮 A 的空操作路径接了提示音，导致「A 只在什么都没做时响、B 永远不响」的不一致。
_Avoid_: 提示音、音效、告警、alarm

## 里程碑

**M0（硬件 bringup）**：
在写任何界面代码之前，先把 PCA9548A + 三屏链路、两个按钮、蜂鸣器全部实测跑通。理由：三屏链路在本机从未验证（[`../pet-oled3/README.md`](../pet-oled3/README.md) 只到「规格已核实」），按钮与蜂鸣器在 [`../../HARDWARE.md`](../../HARDWARE.md) 中零记录。先验证硬件再写上层，避免「代码写完发现屏不亮」。
**当前状态（2026-10-17）**：**已全部实测通过**。卡了 36 小时的 `0x70` 无应答根因是 **SDA/SCL 在实物与固件之间交叉**（实物 GPIO4→模块 SCL、GPIO5→模块 SDA，固件却写 I2C_SDA_PIN=4 / I2C_SCL_PIN=5）；改为 `I2C_SDA_PIN=5 / I2C_SCL_PIN=4` 后三屏全部 online，`pca9548a 0x70 : online`，ch0/ch1/ch2 齐备。教训见「多路复用器」与「Wire.end() 与 I2C 重初始化」词条。
_Avoid_: 阶段一、第一阶段、phase 0

**M1（数据链路）**：
服务器侧以 cron + 静态 JSON 产出指标快照（[ADR-0008](docs/adr/0008-server-side-snapshot-via-cron.md)），设备侧先接主站现成的 `/api/health` 打通 HTTPS 与停更判定（[ADR-0009](docs/adr/0009-phase-in-with-existing-health-endpoint.md)），再切到快照端点（[ADR-0004](docs/adr/0004-fixed-server-polling-and-staleness.md)）。邮箱先接 1 个测试账号，每 5 分钟由独立 cron 拉取，汇总为未读总数。**M1 的服务器侧部署以 M0 链路通为前提**——链路不通时先做设备侧代码。
_Avoid_: 阶段二、联调

**M2（完整界面）**：
三屏完整内容、聚焦屏翻页交互、蜂鸣器全部提示节奏落地。
_Avoid_: 阶段三、收尾

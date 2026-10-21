# status-oled3

ESP32-S3 + PCA9548A + 三块 SSD1306 的桌面状态显示器。三块屏分别显示 **ESP32 自身状态**、**阿里云服务器状态**、**邮箱未读状态**；两个轻触按钮切换功能，一个有源蜂鸣器做通知提醒。

## 屏幕拓扑：三屏多路复用（当前）/ 单屏直连（2026-10-17 的一次绕行，一个常量切换）

**当前是三屏多路复用**：`src/config.h` 的 `ACTIVE_SCREENS = 3`（自 commit `53218a2`「结案三屏 bringup」起就是 3），boot report 打印 `pca9548a 0x70 : online` + ch0/ch1/ch2 三屏全 online（见下文 M0 结案）。

**单屏直连是 2026-10-17 中间的一次绕行**：那天 PCA9548A 模块在 `0x70` 无应答，增强版探针 [`tools/i2c-probe`](tools/i2c-probe/) 已排除 ESP32 侧与物理层（v6 空总线对照证明外设与固件配置正常；全段 NACK 无超时；地址全段覆盖；上拉在线且轨带电），卡点一度收敛到模块本身或其四根线；当时在工作树里改跑单屏直连（`ACTIVE_SCREENS = 1`）让 M1a 的数据链路先跑起来。**根因后来查清是 SDA/SCL 交叉**（见下节），三屏已恢复——`ACTIVE_SCREENS = 1` 那次绕行**从未进过 git**（`git log -p -- src/config.h` 里该常量自 `53218a2` 起一直是 3），所以只有本文档与 `.doc/` 里的过程记录留下了这段痕迹。判据与当时的过程记录见 [`.doc/single-screen-topology-2026-10-17.md`](.doc/single-screen-topology-2026-10-17.md) 与 [`.doc/i2c-absent-investigation-2026-10-17.md`](.doc/i2c-absent-investigation-2026-10-17.md)（过程产物，已 gitignore）。

**拓扑仍由一个常量决定**：`src/config.h` 的 `ACTIVE_SCREENS` 为 3 时多路复用器在链路里、为 1 时三个逻辑屏共用一块直连面板。没有其他开关，因此不存在「某两个设置组合起来是错的」这种状态。切换的语义细节（尤其 `pca9548a::begin()` 与 `select()` 返回值方向相反是刻意的）记录在同目录 `.doc` 文档里。

## 硬件拓扑（三屏版，`ACTIVE_SCREENS = 3` 时）

沿用 `pet-oled3` 的三屏链路：PCA9548A（0x70，A0/A1/A2 接地）挂在上游 I2C（**SDA=GPIO5 / SCL=GPIO4**）上，通道 0/1/2 各接一块 SSD1306（0x3C），一次只选通一路。

> GPIO 口径说明：2026-10-16 bench 实测接线为 GPIO4/GPIO5（非 `pet-oled3` 初版策划案的 GPIO8/9）。GPIO4/5 正是 [`HARDWARE.md`](../HARDWARE.md) §2.3 记录的本机实测跑通 I2C 脚位。
> 📌 **2026-10-21**：`pet-oled3` 已按用户决策**与本项目硬件完全对齐**（SDA=GPIO5 / SCL=GPIO4、RST=GPIO8、按钮=6/7、有源蜂鸣器=21、WS2812=48），见 [`../pet-oled3/docs/adr/0006-hardware-interface-aligned-with-status-oled3.md`](../pet-oled3/docs/adr/0006-hardware-interface-aligned-with-status-oled3.md)。本表两个项目现在指同一套接线。

| 器件 | 连接 | 说明 |
|---|---|---|
| 按钮 A（聚焦屏翻页） | GPIO 6，内部上拉 | 短按 = 聚焦屏翻到下一页 |
| 按钮 B（切换聚焦屏） | GPIO 7，内部上拉 | 短按 = 聚焦屏 0→1→2→0 循环 |
| 有源蜂鸣器 | GPIO 21，推挽通断 | 有源型，只能通断编码；**固件把该脚驱动强度提到 `GPIO_DRIVE_CAP_3`**（默认 20 mA 临界，见下文「接线」小节） |

引脚依据见 [`docs/adr/0007-gpio-assignment-buttons-and-buzzer.md`](docs/adr/0007-gpio-assignment-buttons-and-buzzer.md)。初版规划曾是 GPIO10/11/12，2026-10-16 按 bench 实际接线订正——**理由是保持与实物一致**，不是 GPIO 12 有禁忌（datasheet v2.2 核实：strapping 脚只有 GPIO 0/3/45/46，MTDI 是 GPIO 41，GPIO 12 是 P2 普通 IO）。

## 数据来源

设备 HTTPS 轮询服务器上的数据源，读本机指标（`/proc`、`df`、负载）与邮箱未读数。设备侧不持有任何服务端凭据（WiFi 自身的 SSID/密码是例外，属连网前提）。轮询 15 秒一次，60 秒无有效数据判**停更**。

- 服务器侧形态：[ADR-0008](docs/adr/0008-server-side-snapshot-via-cron.md)（cron 每分钟生成静态 JSON + nginx 直接伺服；**取代已被否决的 [ADR-0005](docs/adr/0005-aggregator-as-standalone-service-with-shared-secret.md) 独立 Go 服务方案**）
- 分两步打通：[ADR-0009](docs/adr/0009-phase-in-with-existing-health-endpoint.md)（M1a 先接主站现成的 `/api/health`，服务器零改动；M1b 切到 `/status/snapshot.json`）
- 端点地址：[ADR-0006](docs/adr/0006-aggregator-url-pinning.md)（域名 + 路径，不存 IP/端口，依赖通配符证书公开 CA 链）
- 轮询与停更：[ADR-0004](docs/adr/0004-fixed-server-polling-and-staleness.md)（15s / 60s，判定依据是设备本地单调时钟）
- 提示音触发规则：[ADR-0011](docs/adr/0011-edge-triggered-notification-rules.md)（停更、断网、新未读都按上升沿一次性触发）

## 里程碑

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 硬件 bringup：PCA9548A + 三屏链路实测，按钮消抖，蜂鸣器通断 | **已上板实测通过**（2026-10-17，`pca9548a 0x70 : online` + ch0/ch1/ch2 三屏全 online，字模方向正确无镜像，见下文） |
| M1a | 设备侧数据链路：WiFi + HTTPS `GET /api/health` + 停更判定 + 断网/停更提示音 | **已上板实测通过**（2026-10-17，串口 `[poll] ok` + `stale=no`，见下文） |
| M1b | 服务器侧 cron 快照 + 邮箱 cron，设备切到 `/status/snapshot.json` | **已上线并实测验收（2026-10-17）**：服务器侧 4 个产物已部署到阿里云（无密钥/错密钥 403，正确密钥 200，白名单外 403，cron 每分钟刷新）；设备侧固件已烧录并持续取数（`[poll] snapshot ok` / `[poll] mailbox ok`），见下文「当前状态」的验收表 |
| M2 | 完整界面：服务器屏两页、邮箱屏未读数、聚焦屏翻页 | 服务器屏两页 + 邮箱未读数已随 M1b 设备侧落地，剩余 M2 = 证书固定（`setCACert` 取代 `setInsecure`）与逐账号邮箱状态数组 |

M0 先行的理由：三屏 + PCA9548A 链路在本机**从未实测**，按钮与蜂鸣器在 [`HARDWARE.md`](../HARDWARE.md) 中零记录。先验证硬件再写上层，避免「代码写完了发现屏不亮」。

### M0 结案：`0x70` 无应答的根因是 SDA/SCL 交叉（2026-10-17）

卡了约 36 小时。最终的根因一句话：**实物接线是 GPIO4 → 模块 SCL、GPIO5 → 模块 SDA，而固件写的是 `I2C_SDA_PIN=4 / I2C_SCL_PIN=5`，两线在实物与固件之间交叉了。**

I2C 的 START/STOP/ACK 全定义在「SDA 相对 SCL 的边沿」上，互换后从机永远识别不出 START。**总线电气完全正常**，所以之前的排查一路误判：

| 现象 | 当时的误判 | 真实解释 |
|---|---|---|
| `idle` 读 HIGH/HIGH、ADC 3152mV | 「上拉在线、接线导通」✓（这条没错） | 两线都有上拉，与线序无关 |
| 全段 `nack_addr=2`、`timeout=0` | 「总线电气完整，只是无器件应答，模块坏」 | 主控时序跑得完，但从机认不出 START |
| 空总线对照三趟逐字一致 | 「ESP32 外设和固件都没问题」✓（这条没错） | 与线序无关，仍是有效对照 |

**破案证据（i2c-probe v7，一次 flashing 定案）**：三趟扫描各自独立 `Wire.end()`——

| 扫描 | SDA / SCL | 结果 |
|---|---|---|
| A1 空总线 GPIO10/11（对照） | — | `nack_addr=112 timeout=0` |
| A2 **匹配实物** | **GPIO5 / GPIO4** | **`0x70 -> ACK`** |
| A3 交叉线序 | GPIO4 / GPIO5 | `0x70 -> 2 nack_addr` |

修复 = `config.h` 两行：`I2C_SDA_PIN = 5` / `I2C_SCL_PIN = 4`。改完 boot report 三屏全 online。

> **为什么查了 36 小时**：之前每一版探针都有「SDA/SCL 对调」pass，但**全都是假的**——缺 `Wire.end()`，而框架 `Wire.cpp:300-303` 的 `begin()` 在 `i2cIsInit(num)` 为真时直接跳到函数尾返回 true，**从不调用 `initPins()`**，所以第二次 `Wire.begin()` 同脚位是彻底 no-op，对调 pass 实际还在扫 GPIO4/GPIO5。这就是它们输出逐字一样的原因。**教训：本框架里换脚位或重初始化 I2C，必须 `Wire.end()` + `Wire.begin()`，缺一不可。**
>
> 同一棵坑里还栽过一次：v4 报出假的「全段 error 5 + 100kHz 挂死」，当时判为「ADC 段干扰 I2C」，也是同一个根因。

**已排除（不要再查）**：地址跳线（0x08–0x77 全段扫过，覆盖 A0/A1/A2 全部 8 种组合）、总线速度（100k/400k 一致）、SDA/SCL 被外部拉死、SDA/SCL 互相短接、模块被按住复位（RST 接 GPIO8，驱动低再释放无效，`as found=HIGH`）、上拉缺失、ESP32 I2C 外设坏（空总线对照）、HARDWARE.md §6.1 同型故障（那是 GPIO1/2 全段 error 5）。完整排查史见 `.doc/i2c-absent-investigation-2026-10-17.md`（过程产物，已 gitignore）。

### M0 实测结论（2026-10-17）

```
=== status-oled3 M1a boot report ===
pca9548a 0x70      : online
screen ch0 (ESP32) : online
screen ch1 (SERVER): online
screen ch2 (MAILBOX): online
focused screen     : ESP32
data source        : https://www.siungo.top/api/health
poll interval      : 15 s, stale after 60 s
==================================
wifi: connected, IP=192.168.1.100 RSSI=-47 dBm
[poll] ok uptime=63h48m32s commit=7153d58 built=2026-10-14T12:36:39Z
```

> 📌 **boot report 新增一行**（2026-10-21）：`buzzer pin         : GPIO21 drive cap=3 (want 3) readback ok`。这是把蜂鸣器脚的**驱动强度从寄存器读回来**打印，不是打印一个常量——「代码里写了」不等于「硬件生效了」。**2026-10-22 已上板复测通过**，读到 `drive cap=3`（见下节「2026-10-22 上板复测」）。
>
> - **字模方向**：三块屏上的字无上下镜像，`0xC8`（COM 扫描反向）正确，`fontDirProbe()` 自检通过
> - **按钮**：A（GPIO6）翻页、B（GPIO7）切换聚焦屏，聚焦屏标题栏反色可见
> - **蜂鸣器**：修订后两个按钮的每次短按都极短响 1 次（见 [ADR-0003](docs/adr/0003-active-buzzer-on-off-encoding.md) 的「按钮反馈」修订）
> - HARDWARE.md §4.8 七项 checklist 全部有结论

### 2026-10-22 上板复测（驱动强度读回）

烧录 `9827529`（`fix(oled3): 蜂鸣器脚驱动强度提到 CAP_3`）后复测，COM4 @ 115200：

```
=== status-oled3 M1b boot report ===
pca9548a 0x70      : online
screen ch0 (ESP32) : online
screen ch1 (SERVER) : online
screen ch2 (MAILBOX) : online
focused screen     : ESP32
data source        : https://www.siungo.top/status/snapshot.json + /status/mailbox.json
poll interval      : 15 s, stale after 60 s; mailbox every 300 s, stale after 480 s
pages              : local=1 server=2 mailbox=1
buzzer pin         : GPIO21 drive cap=3 (want 3) readback ok
==================================
```

| 验项 | 结果 |
|---|---|
| **驱动强度读回** | **`drive cap=3 (want 3) readback ok`** —— `gpio_get_drive_capability()` 读回寄存器值 = 3，与 `gpio_set_drive_capability()` 请求的 `GPIO_DRIVE_CAP_3` 一致。**驱动强度确实落到硬件上了**，不再是「代码里写了就算」 |
| PCA9548A | `0x70 : online` |
| 三块屏 | ch0 / ch1 / ch2 **全部 online**（三屏拓扑，不是单屏直连绕行） |
| WiFi | 关联成功，`IP=192.168.1.100` |
| 轮询 | `[poll] snapshot ok polls=N` 连续递增，`net=up snap=fresh age=0s` |

> 📌 **板子复位后 boot report 之后紧跟 `edge: wifi lost` 是预期行为**——`rst:0x15 (USB_UART_CHIP_RESET)` 后 WiFi 必然先掉一次再重连，**不是故障**。
>
> 📌 原始记录见 [`.doc/m1b-boot-20261022.txt`](.doc/m1b-boot-20261022.txt)（过程产物，已 gitignore）。

完整排查史与证据链见 [`.doc/i2c-absent-investigation-2026-10-17.md`](.doc/i2c-absent-investigation-2026-10-17.md)（过程产物，已 gitignore）。

**在链路通之前可并行推进的工作**：~~M1a 的设备侧数据链路代码（WiFi + HTTPS + JSON 解析 + 停更判定）不依赖三屏链路是否真的点亮——它只需要串口日志即可验证。~~ M1b 的服务器侧部署则仍以链路通为前提。

## M0 代码结构与实测清单

`src/` 采用与 `clock-oled` 一致的裸驱分层，外加本项目的两个输入设备：

| 文件 | 职责 |
|---|---|
| `config.h` | 引脚、地址、时长常量。改硬件只动这里 |
| `pca9548a.h/.cpp` | 八路选通器驱动。刻意不暴露 mask 写入（见文件头注释） |
| `ssd1306_i2c.h/.cpp` | SSD1306 裸驱，从 `clock-oled` 逐字复用 |
| `screen.h/.cpp` | 三屏抽象：在线缓存、脏标记、标题栏反色、页状态 |
| `button.h/.cpp` | 两只按钮的软件消抖状态机，非阻塞 |
| `buzzer.h/.cpp` | 有源蜂鸣器通断编码，[ADR-0003](docs/adr/0003-active-buzzer-on-off-encoding.md) 的节奏表；`begin()` 还负责把 GPIO21 的驱动强度提到 `GPIO_DRIVE_CAP_3`（默认 20 mA 不够，见下文「接线」小节） |
| `main.cpp` | M1b 主循环：五个独立节奏（按钮 / 网络 / ESP32 屏 1s / 另两屏按需 / 串口日志 60s）+ 三屏绘制 + 上升沿提示 |

M1a 新增三个文件：

| 文件 | 职责 |
|---|---|
| `jsonfield.h/.cpp` | 手写扁平 JSON 取值器（`field()` / `number()` / `hasKey()`），[ADR-0010](docs/adr/0010-hand-written-json-value-extractor.md) |
| `health.h/.cpp` | WiFi 连接、HTTPS 轮询、停更判定的全部状态；M1b 起轮询两份文档并直接按字面键暴露字段，`main.cpp` 不持有任何服务端状态 |
| `secrets.h.example` | WiFi 凭据模板，复制成 `src/secrets.h` 后填值（已 gitignore，先例 `clock-oled`） |

编译（本机 Windows，无需服务器侧工具链）：

```powershell
# 首次编译前必须先建 src/secrets.h，否则编译不过（缺 secrets.h 报错）
copy src\secrets.h.example src\secrets.h
# 然后编辑 src\secrets.h 填入 WIFI_SSID / WIFI_PASS
pio run --project-dir W:\Github\Siungo-ESP32\status-oled3
pio run --project-dir W:\Github\Siungo-ESP32\status-oled3 -t upload
pio device monitor -b 115200   # 或 VS Code 的 PlatformIO 串口监视器
```

M1b 设备侧资源占用（编译实测，ESP32-S3）：RAM 16.3%（53536/327680）/ Flash 26.9%（898873/3342336）。对比 M1a 的 15.5% / 26.6%，差额约 +2.8 KB RAM / +11 KB Flash，来自两份文档的双轮询器、19+5 字段的 `hasKey()` 门面、服务器屏两页与邮箱屏的渲染路径。

烧录后按 [`HARDWARE.md`](../HARDWARE.md) §4.8 的 checklist 逐项验，串口会打印 M0 boot report。**卡点状态的实机输出（2026-10-16，SDA/SCL 交叉未修时）——历史留档，已不是当前状态**：

```
=== status-oled3 M0 boot report ===
pca9548a 0x70      : ABSENT
screen ch0 (ESP32) : absent
screen ch1 (SERVER): absent
screen ch2 (MAILBOX): absent
focused screen     : ESP32
==================================
```

修好线序后的输出见上文「M0 实测结论（2026-10-17）」。`pca9548a 0x70 : online` + 三屏全 `online` = 链路打通。若有 `absent`，按报告定位是哪一段；按钮按下串口应打印 `page:` 或 `focus:`，蜂鸣器在空操作时极短响一次。

- [`GLOSSARY.md`](GLOSSARY.md) — 本项目术语表
- [`docs/adr/`](docs/adr/) — 决策记录（ADR-0001 ~ ADR-0012，其中 [ADR-0005](docs/adr/0005-aggregator-as-standalone-service-with-shared-secret.md) 已被否决并由 ADR-0008 取代）
- [`tools/i2c-probe/`](tools/i2c-probe/) — 总线诊断固件（I2C 扫描 + 线路完整性 + SDA/SCL 对调判别）
- [`tools/check-row-overlap.py`](tools/check-row-overlap.py) — 屏幕排版静态检查：同一屏同一帧内，任何一行不得被两段文字写入（`drawText5x7` 是 OR 写入，撞行会叠成重影）。`python tools/check-row-overlap.py`
- [`tools/check-row-overlap-selftest.py`](tools/check-row-overlap-selftest.py) — 上面那个检查器的自检：喂进已知的坏排版，断言它确实报错。改了检查器就跑一次 `python tools/check-row-overlap-selftest.py`
- [`docs/server/`](docs/server/) — M1b 服务器侧产物（脱敏模板，密钥一律 `<MONITOR_KEY>` 占位）

### M1b 设备侧实现要点（已烧录上线，持续取数中）

`src/health.cpp` 轮询两份文档、各自裁决：

| 文档 | 轮询周期 | 停更阈值 | 屏幕 |
|---|---|---|---|
| `snapshot.json` | 15 s | 60 s | 服务器屏（两页） |
| `mailbox.json` | 300 s | 480 s | 邮箱屏 |

**`/status/mailbox.json` 与 `/status/snapshot.json` 共享同一个 nginx location**，因此一处 403 会让两份文档同时失败——但**停更是按文档分别判定的**：邮箱屏连续 8 分钟没有数据才宣称停更，不会因为快照端点坏了就跟着响（health.h 文件头「两个文档两个裁决」）。

**邮箱屏未读上升沿**用双槽一次轮询滞后实现（`g_unread_prev` / `g_unread_last`），而不是一个可被调用方错过的存储标志位：一次成功轮询做 `prev ← last; last ← new`，`unreadRose()` = `last_known && prev_known && last > prev`。这样上升沿在整个 5 分钟窗口内稳定为真，`main.cpp` 自己 latch（`g_announced_rose`）避免同一窗口重复响。**首次拉取时 `prev_known == false`，天然满足 [ADR-0011](docs/adr/0011-edge-triggered-notification-rules.md) 「首次成功拉取后必须把 prevUnread 同步为实际值而非 0」那条最容易踩的坑**；`unread` 变 `null` 时 `last_known = false`，下次恢复时 `prev_known` 也是 false，不会产生幽灵沿。

**19 个 snapshot 字段不做逐字段存储**：保留响应原文，`number(key)` / `string(key)` / `hasNumber(key)` 现场调 `jsonfield`。零额外存储，截断按实收解析（[ADR-0010](docs/adr/0010-hand-written-json-value-extractor.md) edge case 4）。`net_rx_bytes` / `net_tx_bytes` 走 `string()`——`double` 只有 53 位尾数，装不下 PB 级字节数。

**两道接受门**（`health.cpp` 的 `pollSnapshot()`）：`looksComplete()` 挡截断，`jsonfield::hasKey(body, "generated_at")` 挡错文档。实测第二道门的现实来源：`GET https://www.siungo.top/status/snapshot.json` 目前返回 HTTP 200 + `<!doctype html>`（SPA fallback），第一道门已把它挡掉，串口打印 `[poll] snapshot body is not a complete flat JSON object`、`snap=STALE age=0s polls=0`——**这是端点未上线的预期表现，不是固件缺陷**。

**服务器屏两页的内容划分**（[ADR-0001](docs/adr/0001-three-screen-role-split.md) 正面第 2 条「服务器指标一屏放不下，翻页是刚需」）：页 0 = up / cpu / mem / mem 条 / swap / load / data 状态行；页 1 = disk 条 / disk 百分比 / free / net_if / rx / tx / svc 三探活 / cert 天数。128 px @ 6 px/char = 21 列的硬约束逼出三处取舍：`disk=9873408/41251136`（22 字符）放不下 → 改「条 + 百分比 + avail」三行；`rx`/`tx` 各 9 位必须独占一行；`svc=web=1 api=1 up=1` 双重等号可读性差 → `svc=web1 api1 up1`。页号进标题栏（`SERVER 1/2`），不占正文行。

**邮箱屏一页**（ADR-0001：未读数一屏列得下）：状态行（unconfigured / no data / stale / fresh）+ 未读大数字 + 账号/失败数 + 21 段 mailbox 段位条。按钮 A 在邮箱屏是空操作，按 ADR-0001 负面第 2 条与 ADR-0011 备注给 20 ms 极短提示音。

### M1b 服务器侧产物（`docs/server/`，已部署到阿里云）

四个文件，全部已在本机（Windows + Python 3.10）真跑验证：

| 文件 | 作用 | 本地验证结果 |
|---|---|---|
| [`status-snapshot.py`](docs/server/status-snapshot.py) | 每分钟采集 19 个字段，临时文件 + `os.replace()` 原子写 `snapshot.json` | Windows 真跑 exit=0，降级行为符合设计（`/proc` 缺失 → 对应字段发 `null`，不发 0 不省略） |
| [`status-mailbox.py`](docs/server/status-mailbox.py) | 每 5 分钟查 IMAP 未读总数，原子写 `mailbox.json` | `--selftest` 两种形态正确；无凭据文件真跑 exit=0 产出 `available:0` 的合法文档 |
| [`nginx-and-cron.md`](docs/server/nginx-and-cron.md) | nginx `location` + `status-key.conf` + crontab + 部署后门禁的脱敏模板 | 模板，无副作用 |
| [`status-check.py`](docs/server/status-check.py) | 部署前字段契约门禁（[ADR-0012](docs/adr/0012-json-field-contract-for-server-side-documents.md)） | 正路径 exit 0；4 类负路径（缺字段 / 数组值 / 布尔值 / 非 JSON）全部 `FAIL` exit 1 |

> **旧 bash 采集脚本 `status-snapshot.sh` 已删除（2026-10-17）**：第一版用 bash，在 git-bash 下「测过」是被 MSYS 骗的（`/var/www/...` 被重写，写入静默失败），已全部改用 Python3。那段踩坑史留在 [ADR-0008](docs/adr/0008-server-side-snapshot-via-cron.md) 与 git 历史里，脚本本身不是部署产物，留着只会让人误抄——`nginx-and-cron.md` 的 crontab 只引用 `.py`。

**字段契约已冻结在 [ADR-0012](docs/adr/0012-json-field-contract-for-server-side-documents.md)**：两份 JSON 只发字符串、数字、`null` 三种类型；「是/否」一律编码成 1/0；`null` 表示采不到。理由是设备侧解析器（[ADR-0010](docs/adr/0010-hand-written-json-value-extractor.md)）只会读字符串与数字——字段改名或改类型不会报错，只会让屏幕上那个字段永远显示 `-`。

**部署顺序不可颠倒**：服务器侧先上线并通过 `status-check.py`，设备侧才切 `DATA_PATH`。否则设备会连续停更并触发三连短响，屏幕失去判断力。

> 踩坑记录：第一版用 bash 写采集脚本，在 git-bash 下「测过」是被 MSYS 骗的（`/var/www/...` 路径被重写导致写入静默失败）。已全部改用 Python3，服务器与 Windows 都能真跑。

## 当前状态

**M0 代码已就绪并通过编译**（ESP32-S3：RAM 6.8% / Flash 8.6%），已提交并推送（`7fbd2bc`，含 M0 固件与 7 条 ADR）。**M1a 设备侧数据链路已上板实测通过**（RAM 15.5% / Flash 26.6%）。**M1b 全线打通并已上线**（2026-10-17）：服务器侧 cron + nginx location 已部署到阿里云，设备侧固件（双文档轮询 + 字段门面 + 两页服务器屏 + 未读上升沿）已烧录并持续取数，RAM 16.3% / Flash 26.9%。

实测验收（2026-10-17 22:08，从服务器侧与设备侧双向确认）：

| 检查项 | 结果 |
|---|---|
| `/status/snapshot.json` 无密钥 / 错密钥 | 403 / 403 |
| `/status/snapshot.json` 带正确密钥 | 200 `application/json`，`Cache-Control: no-store` |
| `/status/mailbox.json` 带正确密钥 / 无密钥 | 200 / 403 |
| `/status/anything`、`/status/secret.json` | 403（白名单外一律拒绝） |
| 主站 `/`、`/api/health`、SPA 路由 `/works` | 200 / 200 / 200 |
| `studio.siungo.top`、`select.siungo.top` | 200 / 200（其它 vhost 未受影响） |
| `status-check.py` 门禁 | `OK ... 19 fields` + `OK ... 5 fields`，exit 0 |
| cron 自动刷新 | snapshot 每分钟刷新（实测 mtime 22:08:01） |
| 设备串口 | `[poll] snapshot ok polls=16/17/18`，无解析失败 |
| 邮箱 IMAP | `available=1 unread=41`，设备 `[poll] mailbox ok available=1 unread=41` |

`src/secrets.h` 的 `MONITOR_KEY` 已填入主站真值（44 字符，**不进 git**）。

**部署过程中踩到并已回写文档的两个坑**（[ADR-0008](docs/adr/0008-server-side-snapshot-via-cron.md) 与 [`docs/server/nginx-and-cron.md`](docs/server/nginx-and-cron.md) 已修正模板）：

1. **鉴权必须写在内层正则 location 里**。若 `if ($status_ok = 0)` 只放外层 `location /status/`，请求一旦匹配嵌套的 `.json` location，外层的 `if` 就不再对它求值——实测 `/status/anything` 返回 403 而 `/status/snapshot.json` 无密钥返回 200。
2. **root 的 crontab 没有 user 字段**。`* * * * * root /usr/bin/python3 ...` 是 `/etc/crontab` 的格式，写进 `crontab -e` 会让 cron 去执行名叫 `root` 的命令。

另：`map` 只能出现在 `http{}` 里，`conf.d/status-key.conf` 由 `nginx.conf` 的 `include /etc/nginx/conf.d/*.conf;` 加载，server 块里 **不能** 再 include 一次（否则报 `map directive is not allowed here`）。

M1a 的服务器侧**零改动**：设备轮询的是主站已有的公开端点 `GET /api/health`（nginx 已在反代）。M1b 切到 `/status/snapshot.json` 后，`/api/health` 不再被设备轮询（[ADR-0009](docs/adr/0009-phase-in-with-existing-health-endpoint.md) 的过渡角色结束，端点本身仍作为主站健康检查保留）。

**SPA catch-all 的教训保留在此**：M1b 端点上线前，`GET /status/snapshot.json` 返回 HTTP 200 + `<!doctype html>`（2026-10-17 用 `web_fetch` 核实），固件的 `[poll] snapshot body is not a complete flat JSON object` 是**预期行为**——第一道门（大括号配平）挡掉了 SPA 页面，没有把 HTML 当成数据。这是 [ADR-0008](docs/adr/0008-server-side-snapshot-via-cron.md) 「`location /status/` 必须插在 SPA fallback 之前」那条警告的现实来源，也是「两道接受门」设计价值的实证。

M1a 上板实测（2026-10-17，COM4，串口日志）。**当时的单屏直连绕行拓扑下**（现已恢复三屏）：

```
=== status-oled3 M1a boot report ===
topology           : 1 panel direct on GPIO4/GPIO5
panel              : absent
focused screen     : ESP32
data source        : https://www.siungo.top/api/health
poll interval      : 15 s, stale after 60 s
==================================
edge: wifi lost
wifi: connecting to "<ssid>" ...
wifi: connected, IP=192.168.1.100 RSSI=-37 dBm
[poll] ok uptime=52h35m6s commit=7153d58 built=2026-10-14T12:36:39Z
[poll] ok uptime=52h35m21s commit=7153d58 built=2026-10-14T12:36:39Z
net=up stale=no data_age=0s polls=4 up=52h35m51s commit=7153d58
```

**链路已打通**：WiFi 连接、HTTPS 轮询、JSON 取值、15s 节拍、`stale=no` 判定全部实测通过。`uptime=52h35m6s` 说明服务器 Go 进程连续跑了 52 小时，`commit=7153d58 built=2026-10-14T12:36:39Z` 是 `-ldflags` 注入的真实构建信息（非 `dev`/`unknown`）。**拓扑改动没有触及数据链路**——这是「逻辑屏与物理面板分离」设计的直接验证。

`panel : absent` 的原因：这次读数时屏幕没有接在 GPIO4/GPIO5 上（用户确认直连可行的那块屏当时不在板子上）。等屏接上，这一行应变 `online`，并且 ESP32 状态屏应显示 1s 递增的 uptime。

> 📌 **这段 M1a 记录是单屏直连绕行期的读数**（见文首「屏幕拓扑」一节）。三屏恢复后，设备侧 boot report 打印的是 `pca9548a 0x70 : online` + 三块通道各自 online。**M1a 的数据链路结论与拓扑无关**——`[poll] ok` / `stale=no` 在两种拓扑下都成立。

**上板时发现并已修正的一个 TLS 配置陷阱**（记在这里避免重犯）：第一次烧录后 `start_ssl_client: -1` 连续失败，而同一主机的明文 TCP 443 探测却通过。根因在框架里——`WiFiClientSecure` **既不调 `setCACert()` 也不调 `setInsecure()` 时**，`ssl_client.cpp:226-228` 会直接 `return -1`（「无 CA、无 bundle、且非 insecure」是它不接受的组合），与构造函数默认的「要校验」语义相反。现已在 `fetchOnce()` 里显式 `setInsecure()`，并在 `src/health.cpp` 头部注释与 [ADR-0006](docs/adr/0006-aggregator-url-pinning.md) 记录了取舍：负载公开、无凭据、字段形状可校验，伪造的最坏后果是屏幕数字错 + 提示音错；证书固定（保留校验）作为 M2 升级路径。**这不是「关掉校验就好」，是明确记录在案的安全姿态。**

> 📌 **本段原为 2026-10-17 的「未提交改动」清单，那些内容后来都已入库**（M1a 在 `53218a2`、M1b 在 `435b64f`、排版检查在 `282e68d`）。**当前工作树里 `status-oled3` 的未提交内容有三批**：① 2026-10-21 的一批文档订正（`README.md` / `GLOSSARY.md` / ADR-0001~0007、0010 的脚位口径订正、`src/config.h` 的 GPIO 12 注释订正、`tools/i2c-probe` 的线序常量订正）；② 2026-10-21 晚些时候的**功能代码改动**——`src/buzzer.cpp` / `src/buzzer.h` / `src/config.h` / `src/main.cpp` 把 GPIO21 的驱动强度提到 `GPIO_DRIVE_CAP_3` 并在 boot report 读回（见下文「接线」小节与 [ADR-0007](docs/adr/0007-gpio-assignment-buttons-and-buzzer.md)）；③ 2026-10-22 的**上板复测记录**（读回 = 3，见上文「2026-10-22 上板复测」）。**这版固件已编译通过（RAM 16.3% / Flash 26.9%）并已上板复测**（`drive cap=3 (want 3) readback ok`）。按用户规则，无明确授权不 commit / push。

> 注：本仓库的 `HARDWARE.md` 是仓库级文档，`GLOSSARY.md` / `README.md` / ADR 是 status-oled3 内部同步，跨项目的订正（脚位口径、SDA/SCL 线序、`pet-oled3` 对齐）应放在同一批提交里以保持仓库一致。

实测接线与初版规划的偏差（config.h 已按实测订正，本文档的硬件拓扑表同步订正）。**表格里的「初版规划」是 2026-10-15 那版，`pet-oled3` 策划案同期的初版规划与之相同**——2026-10-21 `pet-oled3` 已按用户决策与本项目完全对齐：

| 器件 | 初版规划 | 2026-10-17 实测 |
|---|---|---|
| I2C SDA / SCL | GPIO 8 / GPIO 9 | **SDA=GPIO 5 / SCL=GPIO 4**（`pet-oled3` 2026-10-21 已同步对齐） |
| 按钮 A（翻页） | GPIO 10 | **GPIO 6** |
| 按钮 B（换聚焦屏） | GPIO 11 | **GPIO 7** |
| 有源蜂鸣器 | GPIO 12 | **GPIO 21** |

上板接线（按实测；**当前为三屏多路复用，PCA9548A 在链路里**）：

- 上游 I2C：ESP32 **GPIO5 → SSD1306 SDA、GPIO4 → SSD1306 SCL**，3V3 / GND（单屏直连绕行版则不经过 PCA9548A）
- 三屏版（当前）：**GPIO5 → PCA9548A SDA、GPIO4 → PCA9548A SCL**，3V3 / GND；PCA9548A A0 / A1 / A2 全部接地（= 0x70）；通道 0/1/2 各接一块 SSD1306（0x3C）；PCA9548A RST 接 GPIO8（拉高不动作，`as found=HIGH`，驱动低再释放不改变 0x70 无应答的结果）
- 按钮 A：GPIO6 与 GND 之间接轻触开关；按钮 B：GPIO7 与 GND
- 蜂鸣器：GPIO21 与 GND（注意极性，长脚为正）

> ⚠️ **蜂鸣器脚必须提到最强驱动档**（2026-10-21 补上，`pet-oled3` 同期）：GPIO21 的默认驱动档只有 **20 mA**（ESP32-S3 datasheet v2.2 §2.3.4 注 5：GPIO17/18 = 10 mA、GPIO19/20 = 40 mA、**其余 = 20 mA**），而有源蜂鸣器典型 **20–30 mA**——属临界档，30 mA 型号会欠压、音量下降甚至不响。`buzzer::begin()` 现在**先调 `gpio_set_drive_capability(GPIO_NUM_21, GPIO_DRIVE_CAP_3)`（40 mA 源电流）再 `pinMode()`**，boot report 会把驱动档**读回来**打印：`buzzer pin : GPIO21 drive cap=3 (want 3) readback ok`。
>
> 注意 Arduino 核心的 `pinMode()` **不设驱动强度**（`__pinMode()` 只调 `gpio_config()`，不碰 `FUN_DRV` 寄存器），必须显式调用。证据链见 [`../pet-oled3/docs/adr/0010-buzzer-direct-drive-needs-strongest-drive-cap.md`](../pet-oled3/docs/adr/0010-buzzer-direct-drive-needs-strongest-drive-cap.md)（同款蜂鸣器、同一个 GPIO 21），兜底方案是加 NPN 三极管（脚位不变）。

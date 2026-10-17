# status-oled3

ESP32-S3 + PCA9548A + 三块 SSD1306 的桌面状态显示器。三块屏分别显示 **ESP32 自身状态**、**阿里云服务器状态**、**邮箱未读状态**；两个轻触按钮切换功能，一个有源蜂鸣器做通知提醒。

## 屏幕拓扑：单屏直连（当前）/ 三屏多路复用（一个常量切换）

2026-10-17 起开发跑在**单屏直连**拓扑：一块 SSD1306 直连 GPIO4/GPIO5，**多路复用器不在信号路径里**。三个逻辑屏（ESP32 状态 / 服务器 / 邮箱）共用这一块面板，按钮 B 选哪个逻辑屏显示、按钮 A 在聚焦屏内翻页；数据链路、分页、按钮、蜂鸣器与三屏版完全一致。

触发原因：PCA9548A 模块在 `0x70` 无应答，增强版探针 [`tools/i2c-probe`](tools/i2c-probe/) 已排除 ESP32 侧与物理层（v6 空总线对照证明外设与固件配置正常；全段 NACK 无超时；地址全段覆盖；上拉在线且轨带电），卡点收敛到模块本身或其四根线；同时确认同一块屏直连 GPIO4/5 在本仓库其他程序可行。判据与实测记录见 [`.doc/single-screen-topology-2026-10-17.md`](.doc/single-screen-topology-2026-10-17.md) 与 [`.doc/i2c-absent-investigation-2026-10-17.md`](.doc/i2c-absent-investigation-2026-10-17.md)（过程产物，已 gitignore）。

**回三屏只改一个常量**：`src/config.h` 的 `ACTIVE_SCREENS` 由 1 改 3，多路复用器回到链路里，三个逻辑屏各占一个通道与一块面板。没有其他开关，因此不存在「某两个设置组合起来是错的」这种状态。切换的语义细节（尤其 `pca9548a::begin()` 与 `select()` 返回值方向相反是刻意的）记录在同目录 `.doc` 文档里。

## 硬件拓扑（三屏版，`ACTIVE_SCREENS = 3` 时）

沿用 `pet-oled3` 的三屏链路：PCA9548A（0x70，A0/A1/A2 接地）挂在上游 I2C（SDA=GPIO4 / SCL=GPIO5）上，通道 0/1/2 各接一块 SSD1306（0x3C），一次只选通一路。

> GPIO 口径说明：2026-10-16 bench 实测接线为 GPIO4/GPIO5（非 `pet-oled3` 策划案的 GPIO8/9）。GPIO4/5 正是 [`HARDWARE.md`](../HARDWARE.md) §2.3 记录的本机实测跑通 I2C 脚位。

| 器件 | 连接 | 说明 |
|---|---|---|
| 按钮 A（聚焦屏翻页） | GPIO 6，内部上拉 | 短按 = 聚焦屏翻到下一页 |
| 按钮 B（切换聚焦屏） | GPIO 7，内部上拉 | 短按 = 聚焦屏 0→1→2→0 循环 |
| 有源蜂鸣器 | GPIO 21，推挽通断 | 有源型，只能通断编码 |

引脚依据见 [`docs/adr/0007-gpio-assignment-buttons-and-buzzer.md`](docs/adr/0007-gpio-assignment-buttons-and-buzzer.md)。初版规划曾是 GPIO10/11/12，2026-10-16 按 bench 实际接线订正——GPIO 12 是 MTMI strapping 脚，避开它是订正的主要收益。

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
| M1b | 服务器侧 cron 快照 + 邮箱 cron，设备切到 `/status/snapshot.json` | 未开始 |
| M2 | 完整界面：服务器屏两页、邮箱屏未读数、聚焦屏翻页 | 未开始 |

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
| A3 固件原状 | GPIO4 / GPIO5 | `0x70 -> 2 nack_addr` |

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

- **字模方向**：三块屏上的字无上下镜像，`0xC8`（COM 扫描反向）正确，`fontDirProbe()` 自检通过
- **按钮**：A（GPIO6）翻页、B（GPIO7）切换聚焦屏，聚焦屏标题栏反色可见
- **蜂鸣器**：修订后两个按钮的每次短按都极短响 1 次（见 [ADR-0003](docs/adr/0003-active-buzzer-on-off-encoding.md) 的「按钮反馈」修订）
- HARDWARE.md §4.8 七项 checklist 全部有结论

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
| `buzzer.h/.cpp` | 有源蜂鸣器通断编码，[ADR-0003](docs/adr/0003-active-buzzer-on-off-encoding.md) 的节奏表 |
| `main.cpp` | M1a 主循环：五个独立节奏（按钮 / 网络 / ESP32 屏 1s / 另两屏按需 / 串口日志 60s） |

M1a 新增三个文件：

| 文件 | 职责 |
|---|---|
| `jsonfield.h/.cpp` | 手写扁平 JSON 取值器（`field()` / `number()`），[ADR-0010](docs/adr/0010-hand-written-json-value-extractor.md) |
| `health.h/.cpp` | WiFi 连接、HTTPS 轮询、停更判定的全部状态；`main.cpp` 只读 `netOnline()` / `stale()` / `secondsSinceData()` |
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

M1a 资源占用（编译实测）：RAM 15.5%（50736/327680）/ Flash 26.6%（887817/3342336）。对比 M0 的 6.8% / 8.6%，差额即 WiFi + TLS + HTTP 栈，与 [`HARDWARE.md`](../HARDWARE.md) §9 记录的 `clock-oled` 叠加 WiFi 后的量级一致（14.1% / 21.5%）。

烧录后按 [`HARDWARE.md`](../HARDWARE.md) §4.8 的 checklist 逐项验，串口会打印 M0 boot report。**当前实机输出（2026-10-16，卡点状态）**：

```
=== status-oled3 M0 boot report ===
pca9548a 0x70      : ABSENT
screen ch0 (ESP32) : absent
screen ch1 (SERVER): absent
screen ch2 (MAILBOX): absent
focused screen     : ESP32
==================================
```

`pca9548a 0x70 : online` + 三屏全 `online` = 链路打通。若有 `absent`，按报告定位是哪一段；按钮按下串口应打印 `page:` 或 `focus:`，蜂鸣器在空操作时极短响一次。

- [`GLOSSARY.md`](GLOSSARY.md) — 本项目术语表
- [`docs/adr/`](docs/adr/) — 决策记录（ADR-0001 ~ ADR-0011，其中 [ADR-0005](docs/adr/0005-aggregator-as-standalone-service-with-shared-secret.md) 已被否决并由 ADR-0008 取代）
- [`tools/i2c-probe/`](tools/i2c-probe/) — 总线诊断固件（I2C 扫描 + 线路完整性 + SDA/SCL 对调判别）

## 当前状态

**M0 代码已就绪并通过编译**（ESP32-S3：RAM 6.8% / Flash 8.6%），已提交并推送（`7fbd2bc`，含 M0 固件与 7 条 ADR）。**M1a 设备侧数据链路已上板实测通过**（RAM 15.5% / Flash 26.6%，串口 `[poll] ok` / `stale=no`，实测记录见上文）。**M1b 服务器侧（cron + nginx location）未开始。**

M1a 的服务器侧**零改动**：设备轮询的是主站已有的公开端点 `GET /api/health`（nginx 已在反代）。切到 M1b 只需改 `src/config.h` 的 `DATA_PATH` 一个常量，其余逻辑不变（[ADR-0009](docs/adr/0009-phase-in-with-existing-health-endpoint.md)）。

M1a 上板实测（2026-10-17，COM4，串口日志）。单屏直连拓扑下：

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

**上板时发现并已修正的一个 TLS 配置陷阱**（记在这里避免重犯）：第一次烧录后 `start_ssl_client: -1` 连续失败，而同一主机的明文 TCP 443 探测却通过。根因在框架里——`WiFiClientSecure` **既不调 `setCACert()` 也不调 `setInsecure()` 时**，`ssl_client.cpp:226-228` 会直接 `return -1`（「无 CA、无 bundle、且非 insecure」是它不接受的组合），与构造函数默认的「要校验」语义相反。现已在 `fetchOnce()` 里显式 `setInsecure()`，并在 `src/health.cpp` 头部注释与 [ADR-0006](docs/adr/0006-aggregator-url-pinning.md) 记录了取舍：负载公开、无凭据、字段形状可校验，伪造的最坏后果是屏幕数字错 + 提示音错；证书固定（保留校验）作为 M2 升级路径。**这不是「关掉校验就好」，是明确记录在案的安全姿态。**

未提交、未推送的改动（截至 2026-10-17）：`.gitignore` 补 `**/.doc/`、`src/config.h` 按实测订正引脚并新增 M1 网络常量与 `ACTIVE_SCREENS` 拓扑开关、`src/main.cpp` 接入 M1a 数据链路、`src/health.cpp` 链路探测与 TLS 姿态、`src/pca9548a.*` 与 `src/screen.*` 的 `isDirect()` / `showable()` / `setFocused()` 单屏拓扑支持、`HARDWARE.md` §1.2/§4.5/§4.8 回写实测、`GLOSSARY.md` 与 `README.md` 同步、`docs/adr/0006` 补 `setInsecure()` 取舍、`docs/adr/0007` 脚位结论改为 bench 实测值、新增 `tools/i2c-probe/`、ADR-0008/0009/0010/0011 与 M1a 源码（`health.*` / `jsonfield.*` / `secrets.h.example`）。**按用户规则，无明确授权不 commit / push**，等用户确认后一并提交。

> 注：未提交改动中的 `HARDWARE.md` 是对仓库级文档的回写，`GLOSSARY.md` / `README.md` / ADR 是 status-oled3 内部同步，两者应一起提交以保持仓库一致。

实测接线与初版规划的偏差（config.h 已按实测订正，本文档的硬件拓扑表同步订正）：

| 器件 | 初版规划 | 2026-10-16 实测 |
|---|---|---|
| I2C SDA / SCL | GPIO 8 / GPIO 9 | **GPIO 4 / GPIO 5** |
| 按钮 A（翻页） | GPIO 10 | **GPIO 6** |
| 按钮 B（换聚焦屏） | GPIO 11 | **GPIO 7** |
| 有源蜂鸣器 | GPIO 12 | **GPIO 21** |

上板接线（按实测；当前为单屏直连，PCA9548A 不接）：

- 上游 I2C：ESP32 GPIO4 → SSD1306 SDA，GPIO5 → SSD1306 SCL，3V3 / GND（**单屏版直连，不经过 PCA9548A**）
- 三屏版：GPIO4/GPIO5 → PCA9548A SDA/SCL，3V3 / GND；PCA9548A A0 / A1 / A2 全部接地（= 0x70）；通道 0/1/2 各接一块 SSD1306（0x3C）；PCA9548A RST 接 GPIO8（拉高不动作，`as found=HIGH`，驱动低再释放不改变 0x70 无应答的结果）
- 按钮 A：GPIO6 与 GND 之间接轻触开关；按钮 B：GPIO7 与 GND
- 蜂鸣器：GPIO21 与 GND（注意极性，长脚为正）

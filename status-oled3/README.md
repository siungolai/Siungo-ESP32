# status-oled3

ESP32-S3 + PCA9548A + 三块 SSD1306 的桌面状态显示器。三块屏分别显示 **ESP32 自身状态**、**阿里云服务器状态**、**邮箱未读状态**；两个轻触按钮切换功能，一个有源蜂鸣器做通知提醒。

## 硬件拓扑

沿用 `pet-oled3` 的三屏链路：PCA9548A（0x70，A0/A1/A2 接地）挂在上游 I2C（SDA=GPIO8 / SCL=GPIO9）上，通道 0/1/2 各接一块 SSD1306（0x3C），一次只选通一路。

| 器件 | 连接 | 说明 |
|---|---|---|
| 按钮 A（聚焦屏翻页） | GPIO 10，内部上拉 | 短按 = 聚焦屏翻到下一页 |
| 按钮 B（切换聚焦屏） | GPIO 11，内部上拉 | 短按 = 聚焦屏 0→1→2→0 循环 |
| 有源蜂鸣器 | GPIO 12，推挽通断 | 有源型，只能通断编码 |

引脚依据见 [`docs/adr/0007-gpio-assignment-buttons-and-buzzer.md`](docs/adr/0007-gpio-assignment-buttons-and-buzzer.md)。

## 数据来源

设备 HTTPS 轮询一个跑在被监控主机上的**聚合端点**，读本机指标（`/proc`、`df`、负载）与各邮箱未读数。设备侧不持有任何服务端凭据。轮询 15 秒一次，60 秒无有效数据判**停更**。

- 聚合端点形态：[ADR-0005](docs/adr/0005-aggregator-as-standalone-service-with-shared-secret.md)（独立 systemd 服务 + nginx 反代 + 共享密钥）
- 端点地址：[ADR-0006](docs/adr/0006-aggregator-url-pinning.md)（`https://www.siungo.top/status/`，域名 + 路径，不存 IP/端口）
- 轮询与停更：[ADR-0004](docs/adr/0004-fixed-server-polling-and-staleness.md)（15s / 60s）

## 里程碑

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 硬件 bringup：PCA9548A + 三屏链路实测，按钮消抖，蜂鸣器通断 | 代码已完成，**待上板实测** |
| M1 | 聚合端点上线（服务器侧）+ 设备侧轮询打通 | 未开始 |
| M2 | 完整界面：三屏内容、聚焦屏翻页、蜂鸣器提示 | 未开始 |

M0 先行的理由：三屏 + PCA9548A 链路在本机**从未实测**，按钮与蜂鸣器在 [`HARDWARE.md`](../HARDWARE.md) 中零记录。先验证硬件再写上层，避免「代码写完了发现屏不亮」。

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
| `main.cpp` | M0 主循环：三个独立节奏（按钮 / ESP32 屏 1s / 另两屏按需） |

编译（本机 Windows，无需服务器侧工具链）：

```powershell
pio run --project-dir W:\Github\Siungo-ESP32\status-oled3
pio run --project-dir W:\Github\Siungo-ESP32\status-oled3 -t upload
pio device monitor -b 115200   # 或 VS Code 的 PlatformIO 串口监视器
```

烧录后按 [`HARDWARE.md`](../HARDWARE.md) §4.8 的 checklist 逐项验，串口会打印 M0 boot report：

```
=== status-oled3 M0 boot report ===
pca9548a 0x70      : online | ABSENT
screen ch0 (ESP32) : online | absent
screen ch1 (SERVER): online | absent
screen ch2 (MAILBOX): online | absent
focused screen     : ESP32
```

三屏全 `online` = 链路打通。若有 `absent`，按报告定位是哪一段；按钮按下串口应打印 `page:` 或 `focus:`，蜂鸣器在空操作时极短响一次。

## 文档索引

- [`GLOSSARY.md`](GLOSSARY.md) — 本项目术语表
- [`docs/adr/`](docs/adr/) — 决策记录（ADR-0001 ~ ADR-0007）

## 当前状态

**M0 代码已就绪并通过编译**（ESP32-S3：RAM 6.8% / Flash 8.6%），尚未上板。编译与烧录命令见上文「M0 代码结构与实测清单」。M1（WiFi + 聚合端点轮询）尚未开始。

上板前需要准备的实物接线（与 `pet-oled3` 相同，见 [`HARDWARE.md`](../HARDWARE.md) §4.5）：

- 上游 I2C：ESP32 GPIO8 → PCA9548A SDA，GPIO9 → PCA9548A SCL，3V3 / GND
- PCA9548A A0 / A1 / A2 全部接地（= 0x70）
- 通道 0/1/2 各接一块 SSD1306（0x3C），电源从 PCA9548A 或板载 3V3 取
- 按钮 A：GPIO10 与 GND 之间接轻触开关；按钮 B：GPIO11 与 GND
- 蜂鸣器：GPIO12 与 GND（注意极性，长脚为正）

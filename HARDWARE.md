# 硬件与工具链参数 · Siungo-ESP32

本仓库各示例项目用到的硬件与工具链参数。**参数以本机实测为准**（esptool 读回 + `pio pkg list`），不是抄的规格书。

实测日期：2026-10-14（DHT11 一节 2026-10-15 复核）

## 阅读说明

- 本文件是**仓库级硬件事实的唯一来源**。其他文档引用硬件事实时必须带本文件定位，约定见 [`pet-oled3/CONTEXT.md`](pet-oled3/CONTEXT.md)：**优先用 `§节号`**（章节锚点，结构变动不过期），确需精确到行时才用 `:行号-行号`。
- 术语遵循 [`pet-oled3/CONTEXT.md`](pet-oled3/CONTEXT.md) 的命名约定：本文件用**上游/下游**（不叫主从）、**选通/断开**（不叫打开/关闭）、**tick**（不叫帧）。首次出现的缩写均已给出全称。
- 每个器件的状态分三类，文中分别标注：
  - **实测可用**：本机跑通的配置；
  - **规格已核实**：来自厂商数据手册的规格事实，未在本机验证；
  - **未调通/已废止**：本机失败或被放弃的方案，参数表保留作为设计依据。
- 未经本机实测的规划内容一律明确标注「待 bringup 验证」，不写成事实。

---

## 1. 主控开发板

### 1.1 板级参数

| 项目 | 参数 |
|---|---|
| 型号 | ESP32-S3-DevKitC-1（**N16R8**） |
| 芯片 | ESP32-S3 (QFN56) |
| 芯片版本 | **revision v0.2** |
| 封装/特性 | WiFi + BLE，内嵌 **8MB PSRAM**（AP_3v3） |
| 晶体 | 40 MHz |
| Flash | 16 MB |
| PSRAM | 8 MB（OPI，`qio_opi`） |
| USB 串口 | 板载原生 USB（USB CDC，Universal Serial Bus Communication Device Class），`-DARDUINO_USB_CDC_ON_BOOT=1` |
| MAC 地址 | **不公开记录**。本仓库文档中出现的 MAC 一律是虚构示例（当前统一用 `12:34:56:78:9A:BC`），真实值只在板子自己的屏幕上显示、或从串口读；需要真实值时用 `tools/i2c-scan` 同级的串口日志或 `pio device monitor` 看开机行 |
| 系统枚举 | 显示为 `USB VID:PID=303A:1001`（Espressif 原生 USB） |
| 连接端口（本机） | **COM4** |
| 板载 WS2812 | 本机这块是 **v1.0**，数据脚 **GPIO 48**（v1.1 起官方已取消） |
| IO 电平 | 3.3 V（**直接短到 GND 或 3V3 会烧 IO**） |
| PlatformIO board ID | `esp32-s3-devkitc-1` |

### 1.2 GPIO 资源与占用约束

ESP32-S3 共 45 个可用 GPIO（GPIO0–21 + GPIO26–48）。下表是本机已确认的占用与禁区：

| GPIO | 状态 | 依据 |
|---|---|---|
| GPIO 1 / GPIO 2 | **I2C 禁接** | 实测全段超时，见 [§6.1](#61-gpio-1--gpio-2-上-i2c-完全不通实测) |
| GPIO 19 / GPIO 20 | **禁占** | 原生 USB D+/D-（USB-Serial-JTAG），占用即失去板载串口下载，见 [§1.1](#11-板级参数) |
| GPIO 4 / GPIO 5 | I2C SCL / SDA（各项目实测） | 本机跑通的 I2C 脚位，见 [§2.3](#23-接线ssd1306-oled)。**`status-oled3` 与 `pet-oled3` 实测接在 4/5 上**（PCA9548A 上游，见 §4.5） |
| GPIO 6 / GPIO 7 | **同是 `status-oled3` 与 `pet-oled3` 的按钮脚**（2026-10-16 / 2026-10-21 实测）：GPIO 6 = 按钮 A、GPIO 7 = 按钮 B。**另：GPIO 7 与 `dht11-oled` 的 DHT11 数据脚二选一** | GPIO 7 上两者不能同时插——DHT11 模块自带 4.7k~10k 上拉，会与按钮要用的内部上拉并联，见 [§5.3](#53-接线dht11) |
| GPIO 21 | `status-oled3` 有源蜂鸣器（2026-10-16 实测）；**`pet-oled3`（2026-10-21 起）同接 21 做有源蜂鸣器** | 初版规划 GPIO 12，2026-10-16 按 bench 实测接线订正（**改动理由是与实物一致，不是 GPIO 12 有禁忌**——见下一行）。⚠️ **驱动强度必须提到 `GPIO_DRIVE_CAP_3`**：默认档只有 20 mA，有源蜂鸣器典型 20–30 mA，属临界（见下方驱动能力注记 + [`pet-oled3/docs/adr/0010`](pet-oled3/docs/adr/0010-buzzer-direct-drive-needs-strongest-drive-cap.md)）。**2026-10-21 起两个项目的固件都已落实**（`status-oled3` 同日补齐，见 §4.8 补充块） |
| GPIO 8 | `status-oled3` 与 `pet-oled3`（2026-10-21 起）共用：PCA9548A **RESET**（active-low） | 保留硬复位能力；bench 实测 `as found=HIGH`，驱动低再释放不改变结果 |
| GPIO 12 | **普通 IO**（datasheet v2.2 §2.3.4 列为 P2 = 可自由使用无限制），本机当前无归属 | ⚠️ 2026-10-21 订正：旧版本把它误记为「MTDI strapping 脚」。**datasheet v2.2 实测口径：strapping 脚只有 GPIO 0 / 3 / 45 / 46**（§3 Boot Configurations：GPIO0+GPIO46 = chip boot mode、GPIO45 = VDD_SPI 电压、GPIO46 = ROM 打印、GPIO3 = JTAG 信号源）；**MTDI = GPIO 41**（JTAG 组 GPIO 39/40/41/42 = MTCK/MTDO/MTDI/MTMS）。GPIO 12 复用功能为 FSPIIO6 / SUBSPICLK，非 strapping |
| GPIO 26–32 | SPI0/1，封装内 Flash/PSRAM 已占 | 规格已核实，不推荐他用 |
| GPIO 33–37 | 8 线 SPI 高位数据线 | 本板 OPI PSRAM 已占用 |
| GPIO 38 | **空闲**（2026-10-21）：原规划给 `pet-oled3` 的编码器按键，编码器取消后无归属 | 仍是可自由分配的普通 IO，留作 `pet-oled3` 加第三键的备位 |
| GPIO 43 / GPIO 44 | UART0（Universal Asynchronous Receiver-Transmitter，通用异步收发器） | 规格已核实 |
| GPIO 39–42 | JTAG（Joint Test Action Group）：MTCK=39 / MTDO=40 / **MTDI=41** / MTMS=42 | 规格已核实 |
| GPIO 0 / 3 / 45 / 46 | **strapping 脚，勿当普通 IO 用** | datasheet v2.2 §2.3.4 列为 P3；§3 Boot Configurations：GPIO0+GPIO46 = chip boot mode、GPIO45 = VDD_SPI 电压档位、GPIO46 = ROM 消息打印、GPIO3 = JTAG 信号源 |
| GPIO 48 | 板载 WS2812 数据脚 | 规格已核实 + 本机实测 |

> ESP32-S3 的 GPIO 分三档驱动能力：默认驱动强度 GPIO17/18 = 10mA、GPIO19/20 = 40mA、其余 20mA；累计 IO 输出电流上限 1500mA。数据手册未给出每脚绝对最大电流——常见引用的 40mA 是 `PAD_DRIVER=3` 档的典型源电流，不是绝对最大值。
>
> ⚠️ **2026-10-21 补充：驱动强度不是 `pinMode()` 设的。** Arduino 核心的 `pinMode()` 只调 `gpio_config()`，**不碰 `FUN_DRV` 寄存器**——驱动强度保持芯片默认档（`GPIO_DRIVE_CAP_DEFAULT = 2` = medium）。要提到 40 mA 源电流必须**显式调用**：
>
> ```cpp
> #include <driver/gpio.h>
> gpio_set_drive_capability(GPIO_NUM_21, GPIO_DRIVE_CAP_3);  // 档位：0=weak 1=stronger 2=medium 3=strongest
> ```
>
> `driver/gpio.h` 在 Arduino 工程里可直接 `#include`（`Arduino.h → esp32-hal.h → esp32-hal-dac.h` 已把它带进 include 链），**已用最小 Arduino 工程编译验证可用**。适用于**所有源端驱动大电流的场合**——目前本机只有 GPIO 21 蜂鸣器一处。详见 [`pet-oled3/docs/adr/0010-buzzer-direct-drive-needs-strongest-drive-cap.md`](pet-oled3/docs/adr/0010-buzzer-direct-drive-needs-strongest-drive-cap.md)。
>
> 📌 **落地状态（2026-10-22）**：**两个项目都已落实，且都已在硬件上验证**。`pet-oled3/src/buzzer.cpp` 与 `status-oled3/src/buzzer.cpp` 的 `begin()` 都在 `pinMode()` **之前**调 `gpio_set_drive_capability(GPIO_NUM_21, GPIO_DRIVE_CAP_3)`；两个项目的 boot report 都用 `gpio_get_drive_capability()` **把驱动档读回来**打印——`pet-oled3` 2026-10-21 M0 读回 **= 3**，`status-oled3` 2026-10-22 上板读回 **= 3**（`drive cap=3 (want 3) readback ok`）。
>
> 📌 **验收做法**：驱动强度这种「写在代码里」的项，**必须读回寄存器才算数**——「代码里写了」不等于「硬件生效了」。boot report 打印寄存器实际值，是这类项的固定验收方式。

### 1.3 关键板级配置（`platformio.ini`）

六个主项目（`uptime-oled` / `dht11-oled` / `clock-oled` / `lcd1602-uptime` / `blink3` / `pin-probe` / `status-oled`）
的 `platformio.ini` 板级配置完全一致：

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino

board_upload.flash_size = 16MB
board_build.arduino.memory_type = qio_opi

monitor_speed = 115200
build_flags = -DARDUINO_USB_CDC_ON_BOOT=1 -DBOARD_HAS_PSRAM
```

`uptime-oled` / `dht11-oled` / `clock-oled` / `lcd1602-uptime` 四个项目的 `build_flags` 另加 `-Wall -Wextra`；`blink3` / `pin-probe` 不加。各诊断工具（`tools/*`）同样只有 `-DARDUINO_USB_CDC_ON_BOOT=1`。

> 换成普通 8MB Flash、无 PSRAM 的 S3-DevKitC-1 时，去掉 `board_build.arduino.memory_type` 与 `-DBOARD_HAS_PSRAM` 两处。

#### 1.3.1 `-DBOARD_HAS_PSRAM` 为什么必须手写（2026-10-15 实测坐实）

`board=esp32-s3-devkitc-1` 在本机 platform 7.1.3 下解析出的板名是
`Espressif ESP32-S3-DevKitC-1-N8 (8 MB QD, No PSRAM)`，即**按 N8 变体**处理。
Arduino 的 PSRAM 开关来自 `boards.txt` 的 `esp32s3.menu.PSRAM.*` 菜单项，而菜单项
不会被 board 名自动选中，所以 `-DBOARD_HAS_PSRAM` 根本不会进入编译命令行。

后果链（已用反汇编坐实，不是推测）：

1. `cores/esp32/esp32-hal-psram.c` 的 `psramFound()` 被编译成**桩分支**
   （`#else` 里的 `return false`）。反汇编该目标文件得到：
   `entry a1, 32` / `movi.n a2, 0` / `retw.n` —— 恒返回 0。
2. `Esp.cpp` 的 `getPsramSize()` 内部是 `if(psramFound()){...} return 0;`，
   于是**永远返回 0**。
3. 但 IDF 侧其实已经把 PSRAM 加进堆了：`heap_caps_get_total_size(MALLOC_CAP_SPIRAM)`
   报 `8388607` 字节（= 2²³−1，首字节被预留），`heap_caps_malloc(1MB, MALLOC_CAP_SPIRAM)`
   能成功分配且读写回验正确。

**症状**：`status-oled` 的 P2 身份页 `MEM` 行显示 `16M+0K`，与本节记录的 8MB PSRAM
直接矛盾——是**屏上可见的错误信息**，不是纯内部问题。

**注意区分两个配置层次**：`board_build.arduino.memory_type = qio_opi` 只管
**链接期**的内存布局与镜像类型，管不到 `BOARD_HAS_PSRAM` 这个**编译期运行标志**。
两者必须同时存在，缺一不可。

修复后实测：`psramFound()` 由 0 变 1，`ESP.getPsramSize()` 报 `8386295` 字节，
串口开机行由 `psram 0 bytes` 变为 `psram 8386295 bytes`。

> 本仓库所有 `memory_type = qio_opi` 的项目都缺这一行，`status-oled` 是第一个
> 发现并修好的；其余项目在遇到 PSRAM 相关行为（`ps_malloc` 恒返回 NULL、
> `ESP.getPsramSize()` 恒为 0）时按本节处理。

---

## 2. 显示屏：SSD1306 OLED（实测可用）

### 2.1 规格与接口参数

| 项目 | 参数 |
|---|---|
| 型号 | **SSD1306** |
| 接口 | **I2C**（IIC，Inter-Integrated Circuit） |
| 分辨率 | **128 × 64** |
| I2C 7 位地址 | **0x3C**（实测应答）<br>模块丝印 `0x78` 是含读写位的 8 位写法；少数模块为 `0x3D`（丝印 `0x7A`） |
| 尺寸/颜色 | 未记录（0.96" / 1.3" 常见，本驱动与尺寸无关） |

> SSD1306 是 128×64 OLED 最通用的经典款，0.96"/1.3" 模块几乎都是它或兼容片。
> 本仓库的裸驱初始化序列与显存模型直接按 SSD1306 datasheet 编写。
> 顺带一提，SSD1315 是它的引脚与命令兼容替代，同一套代码也能跑 —— 但本机这颗**确认是 SSD1306**。
>
> 首轮调试时曾把型号记作「SSD13159」，那是输入笔误，该型号并不存在。

### 2.2 显存模型

| 项目 | 参数 |
|---|---|
| 显存总量 | 1024 字节（128 列 × 8 页） |
| 分页方式 | 8 页，每页 8 行像素 × 128 列 |
| 字节含义 | 1 字节 = 1 列中的 8 个垂直像素，**bit0 = 页内最上一行** |
| 寻址模式 | 页寻址（`0x20, 0x02`） |
| I2C 速率 | 400 kHz（诊断时降到 100 kHz 提高容错） |

> 字模取模方向必须与显存一致：**1 字节 1 列，bit0 = 页内最上一行**。
> 另行替换字库时务必对齐，否则字会显示成乱块。

### 2.3 接线（SSD1306 OLED）

| OLED 排针 | ESP32-S3 | 说明 |
|---|---|---|
| **SCL**（时钟线） | **GPIO 4** | 原为 GPIO1，2026-10-14 改为 4 |
| **SDA**（数据线） | **GPIO 5** | 原为 GPIO2，2026-10-14 改为 5 |
| VCC | 3V3 | 3.3V 供电时**必须开内部电荷泵**（`0x8D, 0x14`） |
| GND | GND | |

> 引脚编号写在 `src/main.cpp` 顶部的 `PIN_I2C_SCL` / `PIN_I2C_SDA`。
> ESP32 的 `Wire.begin(sda, scl, freq)` 可指定任意 GPIO，换脚只改这两个常量，不动 `platformio.ini`。
>
> **SCL 与 SDA 接反 = 完全不显示**，这是最常见的不亮原因。

### 2.4 I2C 报文格式与传输分块

裸驱直接操作 I2C 报文，控制字节决定后面跟的是命令还是显存数据：

| 控制字节 | 含义 |
|---|---|
| `0x00` | 后面是命令流 |
| `0x40` | 后面是显存数据流 |

`flush()` 把 1024 字节显存按页下发：每页 128 列，单次事务最多带 `kChunkPayload = 64` 字节载荷（ESP32 Arduino 的 `Wire` 缓冲约 128 字节，含控制字节与地址字节的余量），所以每页拆 2 次事务，一轮刷新共 **8 页 × 2 次 = 16 次事务**。

### 2.5 初始化序列

裸驱在 `begin()` 里下发 17 条命令，取自 SSD1306 数据手册：

| 序号 | 命令 | 作用 |
|---|---|---|
| 1 | `0xAE` | 关显示（初始化期间不亮） |
| 2 | `0xD5 0x80` | 时钟分频比 / 振荡器频率 |
| 3 | `0xA8 0x3F` | 多路复用比 = 64 |
| 4 | `0xD3 0x00` | 显示偏移 = 0 |
| 5 | `0x40` | 起始行 = 0 |
| 6 | `0x8D 0x14` | **电荷泵：3.3V 供电必须开** |
| 7 | `0x20 0x02` | 寻址模式 = 页寻址 |
| 8 | `0xA1` | 段重映射（左右镜像） |
| 9 | `0xC8` | 行扫描方向反转 |
| 10 | `0xDA 0x12` | COM 引脚硬件配置 |
| 11 | `0x81 0xCF` | 对比度 = 0xCF |
| 12 | `0xD9 0xF1` | 预充电周期 |
| 13 | `0xDB 0x40` | VCOMH 电平 |
| 14 | `0xA4` | 显存内容直出 |
| 15 | `0xA6` | 正常显示（不反色） |
| 16 | `0x2E` | 停止滚动 |
| 17 | `0xAF` | 开显示 |

`setCursor(page, col)` 由三条命令组成：`0xB0 | (page & 0x07)` 选页，`0x00 | (col & 0x0F)` 设列低 4 位，`0x10 | ((col >> 4) & 0x0F)` 设列高 4 位。

### 2.6 驱动接口

裸驱有两份：`uptime-oled` 版（`uptime-oled/src/ssd1306_i2c.h`，88 行）是 ASCII 绘制，
`clock-oled` 版（`clock-oled/src/ssd1306_i2c.h`，114 行）扩展中文与任意 y 定位。
两份的常量、构造函数、`begin()` / `flush()` / `probe()` 完全相同；
差异只在绘制接口的返回类型与中文能力，见下方各表。

#### 常量与构造函数

| 符号 | 值 / 签名 | 说明 |
|---|---|---|
| `OLED_WIDTH` | `128` | 列数 |
| `OLED_HEIGHT` | `64` | 像素行数 |
| `OLED_PAGES` | `8` | 页数 |
| `OLED_BUFFER_SIZE` | `1024` | 显存缓冲字节数 |
| `Ssd1306I2c(...)` | `Ssd1306I2c(uint8_t scl, uint8_t sda, uint8_t addr = 0x3C, uint32_t hz = 400000)` | 引脚、7 位地址、总线时钟 |

#### 生命周期

| 方法 | 说明 |
|---|---|
| `bool begin()` | 初始化 `Wire`（含 `setTimeOut(50)`）并下发初始化序列；内部自动调用 `fontDirProbe()` |
| `bool ready() const` | 探测是否成功；`flush()` 在失败时直接返回 |
| `void flush()` | 把显存缓冲下发到屏，16 次事务 |
| `void fontDirProbe()` | 画两条参照竖条并停留 **6000ms**，用于人工核对字模方向；串口给出判读指引 |
| `void clear()` | 清显存缓冲 |

#### ASCII 绘制（`uptime-oled`）

| 方法 | 说明 |
|---|---|
| `void drawChar5x7(uint8_t x, uint8_t page, char c)` | 在 (x, page) 画一个 5×7 字符，第 6 列清零作字距 |
| `void drawText5x7(uint8_t x, uint8_t page, const char* s)` | 画 ASCII 字符串。`uptime-oled` 版返回 `void`；`clock-oled` 版返回 `uint8_t`（下一个可用 x） |
| `void drawText5x7Scaled2x(uint8_t x, uint8_t page, const char* s)` | 2 倍放大绘制；字形高 14 像素，**跨 page 与 page+1 两页**，步进 12 列 |
| `void fillPageSpan(uint8_t x0, uint8_t x1, uint8_t page, uint8_t pattern)` | 用同一 pattern 填充一段像素（进度条用） |

#### 中文与任意定位绘制（`clock-oled` 扩展）

| 方法 | 说明 |
|---|---|
| `uint8_t drawText5x7Y(uint8_t x, uint8_t y, const char* s)` | 按像素行 y 定位，返回下一个可用 x |
| `void drawText5x7Scaled2xY(uint8_t x, uint8_t y, const char* s)` | 2 倍放大的 y 定位版 |
| `void drawCnChar(uint8_t x, uint8_t page, uint32_t codepoint)` / `drawCnCharY(uint8_t x, uint8_t y, uint32_t codepoint)` | 画一个汉字，每字 16 列 |
| `uint8_t drawTextCn(uint8_t x, uint8_t page, const char* s)` / `drawTextCnY(uint8_t x, uint8_t y, const char* s)` | 画含中文的字符串，返回下一个可用 x |
| `static uint8_t cnTextWidth(const char* s)` | 计算字符串像素宽度，**每汉字 16 列** |
| `void drawBits(uint8_t x, uint8_t y, const uint8_t* cols, uint8_t n, uint8_t rowsHeight)` | 直接画点阵；`rowsHeight` 取 8 或 16，**取 16 时每列 2 字节、低字节在前**；跨页自动 OR，暗点不写，越界即停 |

> page 版行距只能是 8 的倍数；y 版可任意像素行定位。字形缺失时画方框占位。

#### 底层命令

| 方法 | 说明 |
|---|---|
| `void cmd(uint8_t c)` | 写单条命令（控制字节 `0x00`） |
| `void cmd2(uint8_t c, uint8_t arg)` | 写带参数的命令 |
| `void setCursor(uint8_t page, uint8_t col)` | 设页与列 |
| `bool probe()` | `beginTransmission(addr)` + `endTransmission() == 0` |

驱动还用到一个匿名命名空间常量：`kChunkPayload = 64`（单次事务最大载荷字节）、
`kFontDirProbeMs = 6000`（字模自检图案停留时长，毫秒）。

---

## 3. 显示屏：1602A 字符液晶（I2C 版）—— 未调通，项目已废止

> **2026-10-14 实测结论：这块 1602 + PCF8574 背包在本机没有调通，
> `lcd1602-uptime` 项目已废止（代码保留，不上架）。**
> 下面的参数表是**设计时核实的事实**（规格与协议均经独立来源交叉验证），
> 不是"本机实测可用"的证明——实测恰好相反，器件始终无应答。
> 故障判读见本节末尾。

### 3.1 规格参数

| 项目 | 参数 |
|---|---|
| 型号 | **LCD1602A**（"1602" = 16 列 × 2 行） |
| 显示内容 | 字符，**16 × 2**，内置 5×8 点阵西文字库 |
| 控制器 | **HD44780**（或兼容片），**并口**屏 |
| 接口 | 屏本身是并口；市售模块背面焊 **PCF8574** 转接板，对外为 **I2C** |
| I2C 7 位地址 | **0x27**（PCF8574，A0/A1/A2 全接地，最常见）<br>少数模块为 **0x3F**（PCF8574A 芯片） |
| 工作电压 | **5V**（3.3V 下对比度余量很小，字往往很淡） |
| I2C 速率 | **100 kHz**（PCF8574 标称上限；400kHz 常能跑但非规范） |
| DDRAM 行偏移 | 每行 40 字节：第 1 行 `0x00`，第 2 行 **`0x40`** |
| 对比度调节 | 背包上的蓝色电位器（调 V0） |

### 3.2 PCF8574 → LCD 引脚映射

业界统一映射，与本仓库驱动的常量一致：

| PCF8574 位 | 驱动常量 | 接到 LCD | 含义 |
|---|---|---|---|
| P0 | `LCD_BIT_RS` = `0x01` | RS | 0=命令 1=数据 |
| P1 | `LCD_BIT_RW` = `0x02` | RW | 恒 0（只写不读） |
| P2 | `LCD_BIT_E` = `0x04` | E | E 脉冲（使能选通） |
| P3 | `LCD_BIT_BL` = `0x08` | 背光 | 1=开 |
| P4–P7 | — | D4–D7 | 数据高 4 位 |

HD44780 走 **4 位模式**：一个字节拆成高/低两个 nibble 分两次送，每次打一次 E 脉冲。

### 3.3 5V 与 ESP32 电平冲突

1602 要 5V 才有足够对比度，而 **ESP32-S3 的 IO 不是 5V 耐受的**——
绝对最大输入电压 `VDD+0.3V` ≈ **3.6V**。

关键在于 **PCF8574 背包板上的 4.7k 上拉电阻挂在背包 VCC 上**：
背包接 5V，SDA/SCL 的 HIGH 电平就是 5V，直连 ESP32 会经 ESD 保护二极管灌电流、
**慢性损坏引脚**（不会当场损坏）。

正确做法：背包 VCC 接 5V，**SDA/SCL 经双向电平转换模块**再进 GPIO4/GPIO5，两侧共地。
模块优先选 **MOSFET 型（BSS138）或 PCA9306**；TXB0104 有方向检测，在开漏总线上可能振荡。

> 第一次上电前建议用万用表量背包 SDA/SCL 对 GND 的空闲电压：
> 应等于转换模块 **LV 侧**的 3.3V。量到 5V 就先别接 ESP32。

### 3.4 接线（1602A，与 OLED 共用同一组 GPIO）

| 1602 背包排针 | ESP32-S3 | 说明 |
|---|---|---|
| **SCL**（时钟线） | **GPIO 4** | **5V 方案下必须经电平转换** |
| **SDA**（数据线） | **GPIO 5** | **5V 方案下必须经电平转换** |
| VCC | **5V**（VIN / 5V 脚） | 1602 是 5V 器件 |
| GND | GND | 必须与 ESP32 共地 |

> GPIO4/5 在 ESP32-S3 上都是普通 IO，无 strapping、非 SPI0/1，两屏可安全共用。
> 详见 [`lcd1602-uptime/README.md`](lcd1602-uptime/README.md) 第 2 节。

### 3.5 本机实测记录（2026-10-14）：器件始终无应答

最终按 **3.3V 供电**接线（把背包 VCC 接到 3V3，上拉随之变 3.3V，无需电平转换），
排除超压因素后复测：

| 检查项 | 实测 | 判读 |
|---|---|---|
| 背光 | **亮** | VCC/GND 已到背包 |
| `link-probe` SCL(GPIO4) | `pullup=HIGH float=HIGH` | 外部上拉在线 |
| `link-probe` SDA(GPIO5) | `pullup=HIGH float=HIGH` | 外部上拉在线 |
| `i2c-scan` 0x08–0x77 | **0 个应答，且 0 个 `[ERR]` 行** | 总线能正常完成事务，但无器件应答 |
| SDA/SCL 对调 | 背光仍亮，仍无应答 | 背光只依赖 VCC/GND，与信号线无关 |
| 主程序 | 每 5 秒 `[retry] probing LCD again...` | 0x27 与 0x3F 均不应答 |
| **拔掉 PCF8574 背包后**（2026-10-14 追加） | 背光**仍亮**，两线仍读到 `HIGH` | 见下方判读修订 |

**判读**：`float=HIGH`（不开内部上拉也是高）证明外部上拉电阻真实存在——
上拉是**无源**的，只要 VCC 通就有效，**即使芯片已损坏也能测到高电平**。
所以"上拉正常 + 供电正常 + 全段无应答"这个组合排除掉了接线接反、总线拉死、
地址跳线错误（全段扫描已覆盖 A0/A1/A2 的所有组合与 PCF8574A 段），
收敛到 **背包模块侧的硬件故障**：排针虚焊、背包与屏的 16 针并口虚接、或 PCF8574 坏片。

> **2026-10-14 修订（这条判读的第一步是错的，务必先做）**：
> 当晚做了终极测试——**把 I2C 背包整块拔下来**，"背光还亮吗、两线还是高吗"。
> 结果**背光仍然亮、两线仍然是高**。
> 背光由背包上的 PCF8574 的 P7 脚驱动；器件真被拔掉后背光**不可能**还亮。
> 所以这个现象只有两种解释，且**都指向同一件事**：
> 要么 A 排针从头到尾没插进面包板里（根本没建立电气连接），
> 要么 K 背包与屏体之间的 **16 针并口虚接**，电流从未经过 PCF8574。
> 结论：**"背光亮 = 供电到位"这条判读在本机不成立**，
> 上拉读到 HIGH 也不能证明 SDA/SCL 真的接到了背包。
>
> **正确的第一步**（不需要 ESP32，万用表就行）：量 **背包自身的排针**（不是屏体排针）
> 与屏体对应针之间的通断。开路了就重新压接/补焊 16 针并口，
> 再谈地址、总线、固件——在通断确认之前，那些全是无效劳动。
>
> 顺带修正一条口径：屏体上那 4 个脚一般**只是屏体自己的引出+背光限流**，
> 如果背包没插到底，就会出现"背光亮但器件不在总线上"的假象。

> 与 `endTransmission()` 错误码的关系：
> 返回 **5（超时）** 才是"总线没法完成事务"，会打出 `[ERR]` 行；
> 本次**一行 ERR 都没有**，说明总线电气与 ESP32 的 I2C 收发都正常。
> 看到全是"无应答"时，不要再查 ESP32 侧，直接查器件供电与焊接。

**结论**：与固件无关。`lcd1602-uptime` 的驱动、时序、诊断工具都是完整可用的，
若以后换一片背包/屏再试，可直接复用。项目当前状态见
[`lcd1602-uptime/README.md`](lcd1602-uptime/README.md) 顶部。

### 3.6 驱动接口

裸驱同样完整可用，接口如下。

#### 常量与构造函数

| 符号 | 值 / 签名 | 说明 |
|---|---|---|
| `LCD_COLS` | `16` | 列数 |
| `LCD_ROWS` | `2` | 行数 |
| `LCD_ROW_OFFSET` | `static const uint8_t LCD_ROW_OFFSET[2] = {0x00, 0x40}` | 两行在 DDRAM 中的起始偏移 |
| `Lcd1602I2c(...)` | `Lcd1602I2c(uint8_t scl, uint8_t sda, uint8_t addr = 0x27, uint32_t hz = 100000)` | 引脚、7 位地址、总线时钟 |

#### HD44780 命令码

| 常量 | 值 | 常量 | 值 |
|---|---|---|---|
| `LCD_CMD_CLEAR` | `0x01` | `LCD_CMD_SHIFT` | `0x10` |
| `LCD_CMD_HOME` | `0x02` | `LCD_CMD_FUNCTION_SET` | `0x20` |
| `LCD_CMD_ENTRY_MODE` | `0x04` | `LCD_CMD_SET_CGRAM` | `0x40` |
| `LCD_CMD_DISPLAY_CTRL` | `0x08` | `LCD_CMD_SET_DDRAM` | `0x80` |

#### 生命周期与绘制

| 方法 | 说明 |
|---|---|
| `bool begin()` | 用构造时给的地址初始化 |
| `bool beginAt(uint8_t addr)` | 换地址重新初始化（地址 fallback 用） |
| `bool ready() const` | 上次探测是否成功 |
| `uint8_t address() const` | 当前实际地址 |
| `void clear()` | 清屏（耗时 1.52ms） |
| `void home()` | 光标回原点 |
| `void setCursor(uint8_t col, uint8_t row)` | 定位到 (col, row)，col 越界钳到 `LCD_COLS - 1` |
| `void print(const char* s)` | 从当前光标开始打印 |
| `void printLine(uint8_t row, const char* s)` | 写整行并**补空格到 16 列**，防上一次内容的残影 |
| `void backlight(bool on)` | 背光开关 |
| `bool backlightOn() const` | 背光状态 |

#### 底层

| 方法 | 说明 |
|---|---|
| `void writeNibble(uint8_t nibble, bool isData)` | 送半个字节（nibble 左移到 D4–D7，RW 恒 0） |
| `void sendByte(uint8_t value, bool isData)` | **一次 I2C 事务写 4 个状态字节**完成一个字节 |
| `void writeRaw(uint8_t value)` | 直接写 PCF8574 影子寄存器 |
| `bool probe()` | 探测器件是否应答 |

`sendByte()` 的 4 个状态字节依次是 `hiData | E=1`、`hiData`、`loData | E=1`、`loData`。朴素写法一个字节要 4 次事务，合并后**写 1 个字符 = 1 次 I2C 事务**，整屏 32 字符从 128 次降到 32 次。

#### 时序常量

| 常量 | 值 | 依据 |
|---|---|---|
| `kPulseUs` | `1` | E 脉冲要求 > 450ns |
| `kCmdDelayUs` | `40` | 命令间隔要求 > 37us |
| `kInitDelayMs` | `5` | 上电初始化要求 > 4.1ms |
| `kClearDelayMs` | `2` | 清屏耗时 1.52ms |
| `kLongCmdUs` | `1600` | 慢命令余量 |

初始化序列：`delay(5)` → `writeNibble(0x03)` ×3（间隔 5ms / 200us / 200us）→ `writeNibble(0x02)` 切 4 位模式 → `sendByte(FUNCTION_SET | 0x08)`（4 位 / 2 行 / 5×8）→ `sendByte(DISPLAY_CTRL | 0x04)`（显示开、光标关）→ `clear()` → `sendByte(ENTRY_MODE | 0x02)`（写完光标右移）。

> PCF8574 没有读改写能力，驱动内部维护影子寄存器 `shadow_`，每次写都覆盖全 8 位。

---

## 4. I2C 多路复用器：PCA9548A（8 通道）—— 规格已核实，本机已验证

> **状态：规格已核实（TI 数据手册）+ 本机已验证（2026-10-17 `status-oled3` bench）。** 本节的"已核实"是**规格事实**（来自 TI 官方数据手册），[§4.5](#45-接线pca9548a-上游) 与 [§4.8](#48-已验证2026-10-17) 记录的是**本机实测**结论。
> 上游/下游上拉归属见 [§4.3](#43-上拉电阻归属)，
> 上电后第一件事是显式选通，见 [§4.2](#42-控制寄存器的三个实现细节) 的提示框。
> bringup 验证清单见 [§4.8](#48-已验证2026-10-17)。

### 4.1 规格参数

| 项目 | 参数 |
|---|---|
| 型号 | **PCA9548A**（TI） |
| 功能 | 8 通道双向 I2C/SMBus 开关（1-of-8），**用于解决同址器件冲突** |
| 通道选择 | 控制寄存器 1 字节，bit N = 1 选通通道 N；**支持任意组合同时选通** |
| I2C 7 位地址 | **由 A0/A1/A2 三个硬件脚决定**：`1110 A2 A1 A0`，全接地 = **0x70**；一条总线最多 8 片（0x70–0x77） |
| 上电默认 | **所有通道断开** —— 复位与上电后必须**软件显式选通**才通 |
| 供电范围 | **1.65 V – 5.5 V**（85–125 °C 时降至 1.65–3.6 V），可在 1.8/2.5/3.3/5 V 总线间电平转换 |
| I/O 耐压 | **5 V tolerant**（本方案全 3.3 V，用不到） |
| 频率上限 | **400 kHz** |
| 导通电阻 | RON 典型 5 Ω，最大 12 Ω（3–3.6 V，IO 15mA） |
| RESET 脚 | **active-low**，拉低即复位（下游总线卡死时的恢复手段） |
| 内部上拉 | **无** —— 每条被选通的下游通道需**各自**外部上拉 |
| 热插拔 | 支持（TI 标称 hot insertion） |

### 4.2 控制寄存器的三个实现细节

来自 TI `SCPS207H` 第 7.5.4 节，这三条直接决定代码怎么写：

1. **选通字节 = `1 << N`**：ch0=0x01、ch1=0x02、ch2=0x04、ch3=0x08；写 **0x00 = 全部断开**。
2. **通道在 STOP 条件之后才真正生效** —— 原文：*"When a channel is selected, the channel becomes
   active after a stop condition has been placed on the I2C bus."* 目的是保证切换瞬间所有
   SCn/SDn 都处于高电平，不产生假起始/假停止条件。
   ➡️ **代码含义：选通必须是一次完整的 `beginTransmission → write → endTransmission()`**，
   不能只 write 不收尾，也不能指望写在半途就切过去了。
3. **多字节只保留最后一个** —— *"If multiple bytes are received by the TCA9548A, it saves the last
   byte received."* 所以连续写两个字节不会叠加，后一个覆盖前一个。

> 📌 **上电后第一件事必须是显式选通某一路**（或确认全断），因为上电/复位/RESET 后所有通道都是
> 断开状态，此时**下游三块屏全部不可见**——扫不到 0x3C 是正常的，不是屏坏了。

### 4.3 上拉电阻归属

PCA9548A 不带内部上拉，上拉要分**上游**和**下游**两处看。本机结论：

| 位置 | 上拉来源 | 依据 | 结论 |
|---|---|---|---|
| **上游**（ESP32 ↔ PCA9548A） | PCA9548A **成品模块板载 10k** | 用户确认模块带 10k | ✅ 不需再加 |
| **下游**（PCA9548A ↔ 三块屏） | **SSD1306 模块板载** | [§3.5](#35-本机实测记录2026-10-14器件始终无应答) 的 `link-probe` 实测 `pullup=HIGH float=HIGH` | ✅ 不需再加 |

> ⚠️ **禁止项：不要再往 SDA/SCL 补 4.7k 上拉。** 上游已有 10k、下游每块屏自带，
> 重复并联会把等效阻值拉得过低，逼近 SSD1306 的低电平驱动极限（标称 sink ≈ 3mA）。
>
> 📌 上面那个 `link-probe` 实测**只证明屏那一侧有上拉**（当时总线上只挂了 SSD1306），
> **不能证明 PCA9548A 上游有上拉**。上游那条来自用户对成品模块的确认，两件事互相独立。

### 4.4 多通道并发时的上拉并联风险（设计约束）

PCA9548A **支持**多通道同时选通，但本方案**主动不用**（见 [ADR-0002](pet-oled3/docs/adr/0002-single-channel-sequential-refresh.md)），原因：

若三块屏的板载上拉在通道 0/1/2 上同时导通，会**并联**：

| 情形 | 等效上拉 | 评估 |
|---|---|---|
| 三块屏各 4.7k，仅屏上拉 | ≈ **1.57k** | 尚可，但已偏低 |
| 三块屏各 4.7k + 模块八路各挂 10k | **< 1k** | ⚠️ 逼近 SSD1306 低电平驱动极限 |

**本方案采用「一次只选通一路」**：`选通道 → 刷屏 → 断开 → 下一路`。任一时刻只有一块屏的上拉在线，不存在并联，且能故障隔离、逐通道观测应答。

### 4.5 接线（PCA9548A 上游）

| PCA9548A 排针 | ESP32-S3 | 说明 |
|---|---|---|
| **SDA** | **GPIO 5** | **2026-10-17 `status-oled3` bench 实测值**。GPIO4/5 是 §2.3 记录的本机实测跑通 I2C 脚位，但**线序与初版策划案相反**：实物是 GPIO5→模块 SDA、GPIO4→模块 SCL |
| **SCL** | **GPIO 4** | 同上 |
| VCC | 3V3 | 模块支持 1.65–5.5 V |
| GND | GND | |
| A0 / A1 / A2 | GND（跳线） | 目标地址 **0x70**，**2026-10-17 已由 v7 探针单点 ACK 证实**。地址表：LLL=0x70 / LLH=0x71 / LHL=0x72 / LHH=0x73 / HLL=0x74 / HLH=0x75 / HHL=0x76 / HHH=0x77 |
| RESET | **GPIO 8**（`status-oled3` / `pet-oled3` 实际接线；不用时也可接 3V3 或悬空） | active-low；多数模块已内部上拉。`status-oled3` bench 接 GPIO8 实测：`as found=HIGH`，驱动低再释放不改变结果。保留接线是为了「下游总线卡死时硬复位」 |

> **✅ 2026-10-17 结案：`0x70` 无应答的根因是 SDA/SCL 交叉。** 实物接线是 **GPIO4 → 模块 SCL、GPIO5 → 模块 SDA**（用户回报原文「g4连scl;g5连sda」），而固件最初写 `I2C_SDA_PIN=4 / I2C_SCL_PIN=5`，两线在实物与固件之间交叉。I2C 的 START/STOP/ACK 全定义在「SDA 相对 SCL 的边沿」上，互换后从机永远识别不出 START。
>
> **为什么电气指标全是正常的**：两线都有上拉、都在带电的轨上，所以 `idle` 读 HIGH/HIGH、ADC 3152mV、全段 `nack_addr` 且 `timeout=0`——**这些指标与线序无关，不能用来排除线序问题**。此前据此判「模块坏」，判错了。
>
> **破案证据（i2c-probe v7，三趟各自独立 `Wire.end()`）**：A1 空总线 GPIO10/11（对照）全段 NACK；**A2 `SDA=GPIO5 SCL=GPIO4`（匹配实物）→ `0x70` ACK**；A3 `SDA=GPIO4 SCL=GPIO5`（固件原状）全段 NACK。修复 = `config.h` 改 `I2C_SDA_PIN=5 / I2C_SCL_PIN=4`，改完 boot report `pca9548a 0x70 : online` + ch0/ch1/ch2 三屏全 online。
>
> **为什么查了 36 小时（教训）**：之前每一版探针都有「SDA/SCL 对调」pass，但**全都是假的**——缺 `Wire.end()`，框架 `Wire.cpp:300-303` 的 `begin()` 在 `i2cIsInit(num)` 为真时直接跳到函数尾返回 true，**从不调用 `initPins()`**（`Wire.cpp:118-144` 的 `setPins()` 同理只打 `log_e`），第二次 `Wire.begin()` 同脚位是彻底 no-op，对调 pass 实际还在扫 GPIO4/GPIO5，这就是它们输出逐字一样的原因。同一根因还让 v4 报出假的「全段 error 5 + 100kHz 挂死」，当时误判为「ADC 段干扰」。**在本框架里换脚位或重新初始化 I2C，必须先 `Wire.end()` 再 `Wire.begin()`，缺一不可。**
>
> **已排除（不要再查）**：地址跳线（0x08–0x77 全段扫过，覆盖 A0/A1/A2 全部 8 种组合）、总线速度（100k/400k 一致）、SDA/SCL 被外部拉死、SDA/SCL 互相短接、模块被按住复位（RST 接 GPIO8，驱动低再释放无效，`as found=HIGH`）、上拉缺失、ESP32 I2C 外设坏（空总线对照）、HARDWARE.md §6.1 同型故障（那是 GPIO1/2 全段 error 5）。完整排查史见 `status-oled3/.doc/i2c-absent-investigation-2026-10-17.md`（过程产物，已 gitignore）。
>
> **`status-oled3` 顺带证明了单屏直连拓扑也可用**：`config::ACTIVE_SCREENS` 一个常量切拓扑（1 = 无复用器、一块面板直连 GPIO4/5、三逻辑屏共用；3 = 复用器在链路、每屏一通道一面板）。当前 bench 为 3。

A0 = pin22、A1 = pin23、A2 = pin18，各自直连 VCC 或 GND；RESET = pin24，不用时经上拉接 VCC。

### 4.6 三块屏的通道分配

| 通道 | 器件 | 说明 |
|---|---|---|
| **0** | SSD1306（角色屏） | 保持出厂地址 **0x3C** |
| **1** | SSD1306（数值屏） | 同上 |
| **2** | SSD1306（日志屏） | 同上 |
| 3–7 | 预留 | 0x70 已占住，未来加器件不撞地址 |

> 三块屏**全部保持 0x3C 出厂地址** —— 因为每条通道上只有一个 0x3C 器件，
> 地址冲突在**通道层面**被隔离。这正是 PCA9548A 存在的意义，**不需要动烙铁改地址**。

### 4.7 驱动接口（已实现，2026-10-17）

以下接口**已在 `status-oled3` 实现并实测通过**（`status-oled3/src/pca9548a.h` / `.cpp`）。`pet-oled3` 直接复用，**不再另抄一份签名**：

| 接口 | 约定 |
|---|---|
| `bool pca9548a::begin()` | 初始化 I2C、设超时、探测复用器（0x70 无 ACK 则返回 false） |
| `bool pca9548a::ready()` | 复用器是否在线；离线时上游访问被短路，不产生 I2C 流量 |
| `bool pca9548a::isDirect()` | 是否单屏直连拓扑（无复用器，`config::ACTIVE_SCREENS <= 1`） |
| `bool pca9548a::select(uint8_t channel)` | `channel` 取 0–7，**只选通这一路**，其余全部断开；越界返回 false；重选当前通道是 no-op |
| `void pca9548a::deselectAll()` | 全部断开（**无返回值**：断开是"尽力而为"的动作） |
| `uint8_t pca9548a::currentChannel()` | 诊断用：当前选通的通道，8 表示无 |
| `uint8_t pca9548a::lastError()` | 诊断用：上次上游 I2C 事务的结果码（0 = ok，5 = 超时） |

驱动内部维护通道缓存：`select(n)` 若 `n` 等于当前通道，直接返回、不产生 I2C 事务。设计上**刻意不暴露** `pca9548a_write_mask(uint8_t mask)`，避免调用方同时选通多路触发上拉并联。

选通的寄存器写入必须覆盖 [§4.2](#42-控制寄存器的三个实现细节) 第 2 条：一次完整的 `beginTransmission(0x70) → write(1 << channel) → endTransmission()`，等 STOP 之后通道才真正生效。

> 📌 **2026-10-21 订正**：本节原标题为「规划中的驱动接口（尚未实现）」，接口名为 `pca9548a_select()` / `pca9548a_deselect_all()` 自由函数。实际实现把它们放进 `pca9548a` 命名空间，且 `deselectAll()` 返回 `void`。`pet-oled3` 的 [ADR-0002](pet-oled3/docs/adr/0002-single-channel-sequential-refresh.md) 与 [ADR-0006](pet-oled3/docs/adr/0006-hardware-interface-aligned-with-status-oled3.md) 已按实现签名订正。

### 4.8 已验证（2026-10-17）

- [x] 扫描 0x70–0x77，确认 PCA9548A **实际地址** —— 已完成（2026-10-17 v7 探针）：`SDA=GPIO5 SCL=GPIO4` 时 **`0x70 -> ACK`**，地址确认为 0x70（A0/A1/A2 全接地）。此前全段 0 ACK 是因为线序交叉，不是地址错
- [x] **空总线对照（2026-10-17 v6）** —— GPIO10/GPIO11 本项目无接线，作空总线对照：`idle` 读 LOW/LOW、全段 `nack_addr=112 timeout=0`、完整跑完 112 个事务。⟹ ESP32 I2C 外设与固件配置正常，模块总线的故障不能归因于板子或内核
- [~] 万用表量各通道 SDA/SCL 对 VCC 阻值，确认模块上拉**挂在上游还是八路全挂** —— 未用万用表做（用户无万用表）；已用 ADC 侧面证明上拉接着带电的轨（SDA/SCL 均 3152mV，RESET 拉低时不变）。上游/八路的区分对驱动无影响：`pca9548a::select()` 一次只选一路（ADR-0001），不依赖上拉归属。**2026-10-21 补充**：`pet-oled3` 用脚本把这件事量化结案了——等效上拉下拉电阻 1567–3333 Ω，恒高于 955 Ω 判据（低电平 V_low ≤ 0.4 V），且结论与上拉归属无关；推导见 [`pet-oled3/docs/策划案.md`](pet-oled3/docs/策划案.md) §2.3.1。**本项从此不必再买万用表。**
- [x] 逐通道探测 0x3C，确认三块屏都能被找到 —— 已完成（2026-10-17）：ch0/ch1/ch2 **三块屏全部 online**，boot report 逐行确认
- [x] **三块屏显示不同内容**（同内容无法区分是否真的切换成功）—— 已完成（2026-10-17）：三屏各显各的内容，且**字无上下镜像**（`0xC8` COM 扫描反向正确，`fontDirProbe()` 自检通过）
- [x] 记录 `endTransmission()` 是否返回 5（超时）—— 已完成：**不返回 5**，全段 `timeout=0`。（v4 曾报假的 error 5，根因是探针缺 `Wire.end()`，见 §4.5 教训）
- [x] 确认上游 SDA / SCL 脚位在本机可用 —— **口径订正（2026-10-17）**：实测 **SDA=GPIO5 / SCL=GPIO4**（与初版策划案的 GPIO8/9 及固件最初的 4/5 都不同）。GPIO4/5 可用性由 §2.3 既有实测确认；GPIO8 改接 PCA9548A RST
- [x] 验证「选通在一次完整事务的 STOP 之后才生效」（见 [§4.2](#42-控制寄存器的三个实现细节)）—— 已完成（2026-10-17，反证法）：三块屏同时在线且各显各的内容。若选通/断开时序错，三块 0x3C 会互相抢占同一通道而表现为「内容错乱 / 只有一块亮」，实测无此现象

> ✅ **§4.8 七项全部完成（2026-10-17）。**
> 根因与教训见 §4.5 的结案块；实现签名见 `status-oled3/src/pca9548a.h`。

> 📌 **2026-10-21 复现确认（`pet-oled3` M0）**：`pet-oled3` 用**自己那批模块**把这七项又跑了一遍，结论与 `status-oled3` **完全一致**：
> - PCA9548A **0x70 应答**（A0/A1/A2 接地）；
> - **ch0 / ch1 / ch2 三个下游通道的 `endTransmission()` 全程返回 0（ACK），无一次返回 5**；
> - 三块屏各显各的内容（用户目视确认），**三块屏同型号同尺寸**（`pet-oled3` M0 目视结案）；
> - **4 种按钮手势**（短按 / 长按 × 按钮 A / B）全部触发，一次按压一次事件；
> - **蜂鸣器驱动档现场读回 = 3**（`gpio_get_drive_capability()`），与 §1.2 的驱动能力注记一致。
>
> M0 固件：`pet-oled3/src/main.cpp`；串口原始记录：`pet-oled3/.doc/m0-serial-20261021.txt`。
>
> 📌 复现时用到的**新铁律**（源自 `status-oled3` 的 36 小时教训，现已写进 `pet-oled3/src/pca9548a.cpp` 注释）：**换脚位或重新初始化 I2C 必须先 `Wire.end()` 再 `Wire.begin()`**——ESP32 Arduino 框架的 `Wire.cpp` 让第二次 `begin()` 变成 no-op（永远不 `initPins()`），不这样做会得到"改了脚位却还是旧脚在说话"的假测量。
>
> 📌 **本节七项已由两个项目各自实测，仓库内不再有「PCA9548A 通路待验证」的事项。** `pet-oled3` 的 [ADR-0006](pet-oled3/docs/adr/0006-hardware-interface-aligned-with-status-oled3.md) 已按实现签名订正。
>
> 📌 **2026-10-21 补充：GPIO 21 蜂鸣器的驱动强度在两个项目里都已落地。** `status-oled3` 的 `buzzer::begin()` 原来只做 `pinMode()`（**不设驱动强度**，见 §1.2 尾注），2026-10-21 已补上 `gpio_set_drive_capability(GPIO_NUM_21, GPIO_DRIVE_CAP_3)`（**在 `pinMode()` 之前**），boot report 也改为用 `gpio_get_drive_capability()` **把驱动档读回来**打印 `buzzer pin : GPIO21 drive cap=3 (want 3) readback ok`。
>
> 📌 **2026-10-22 上板复测通过**：`status-oled3` 烧录 `9827529` 后实测读到 **`drive cap=3 (want 3) readback ok`**（寄存器读回值与请求值一致），同时 `pca9548a 0x70 : online`、三屏 ch0/ch1/ch2 全部 online、WiFi 与轮询正常。**两个项目的驱动强度都已在硬件上验证**——`pet-oled3` 2026-10-21 M0 读回 = 3，`status-oled3` 2026-10-22 读回 = 3。原始记录见 `status-oled3/.doc/m1b-boot-20261022.txt`（过程产物，已 gitignore）。

> 📌 **2026-10-21 订正**：本节标题已从「待本机验证（bringup 必做）」改为「已验证（2026-10-17）」；原第 575 行的待办（"完成后回写本节"）**已执行**——[§4.7](#47-驱动接口已实现2026-10-17) 的规划接口已替换为实现签名。
> `pet-oled3` 的 M0 **已于 2026-10-21 完成复现**（结论一致，见上方复现确认块），不必重查地址、线序、上拉、模块好坏。见 [`pet-oled3/docs/adr/0006-hardware-interface-aligned-with-status-oled3.md`](pet-oled3/docs/adr/0006-hardware-interface-aligned-with-status-oled3.md)。

---

## 5. 温湿度传感器：DHT11（带灯模块，实测可用）

### 5.1 规格参数

| 项目 | 参数 |
|---|---|
| 型号 | **DHT11**（带指示灯模块板） |
| 测量范围 | 温度 **0–50 °C**（±2 °C）；湿度 **20–90 %RH**（±5 %RH） |
| 分辨率 | 温度 1 °C；湿度 1 %RH（**整数输出**，不是小数） |
| 采样率上限 | **1 Hz**（即最快 1 秒一次） |
| 接口 | 单总线（1-Wire 类私有协议），**开漏 + 上拉** |
| 数据脚 | **GPIO 7**（本机用户接线） |
| 供电 | 3.3 V（模块板 3.3–5 V 均可，本机接 3V3） |
| 上拉电阻 | **模块自带**（4.7k~10k），无需外接；裸传感器才要补 |
| 指示灯 | 模块自带的电源/指示灯，**恒亮，不表示读到数据** |

> 上表的 0–50 °C / 20–90 %RH 是**驱动接受范围**，即精度保证范围，不是器件物理极限。
> 驱动内常量 `DHT11_TEMP_MIN` / `DHT11_TEMP_MAX` / `DHT11_HUM_MIN` / `DHT11_HUM_MAX`
> 就取这四个值，超出即判 `DHT_ERR_RANGE`。

### 5.2 自写时序驱动（零第三方依赖）

`dht11-oled` 的 DHT11 时序为**本仓库自写**（`src/dht11.h`，仅用 Arduino core API），
`platformio.ini` 无 `lib_deps`。

> **已弃用：Adafruit `DHT sensor library`。** 本机实机踩出两个硬约束，是该库被弃用的原因：
> 1. **最小读取间隔 2000 ms 硬编码在库里**，间隔不够时 `read()` 不碰硬件、
>    直接返回上一次的结果——看起来在读，其实是旧值。且该库没有"一次拿到温度和湿度两个值"的接口，
>    背靠背调 `readHumidity()` + `readTemperature()` 会让第二次读到垃圾，实机症状是湿度正常、温度恒显 `--`。
> 2. **传感器不在线时库会关中断自旋**（`expectPulse()`），
>    累计超时足以触发 `Interrupt wdt timeout on CPU1` 反复重启。
>    详见 [`dht11-oled/README.md`](dht11-oled/README.md) 的「已知边界」一节。
>
> 带灯模块的灯由模块 VCC 供电，只要通电就亮，**与程序是否读到数据无关**。

#### 接口

| 符号 | 签名 / 说明 |
|---|---|
| `dht11Read` | `inline uint8_t dht11Read(uint8_t pin, DhtReading& out)` —— 一次传输解出 5 字节 40 位；**不做节流**，节流是调用方职责 |
| `dhtDataLineLooksAlive` | `inline bool dhtDataLineLooksAlive(uint8_t pin, bool* externallyPulled)` —— 先 `INPUT_PULLUP` 读一次、再纯 `INPUT` 读一次，外部读到 HIGH 才证明上拉存在 |
| `formatDhtCsv` | `inline void formatDhtCsv(uint32_t sec, const DhtReading& r, uint32_t failCount, char* out, size_t outLen)` —— 输出 `t=%lus,hum=%s,temp=%s,ok=%u,fail=%lu`，无效量写 `--` 不写 0 |
| `dhtStatusText` | `inline const char* dhtStatusText(uint8_t status)` —— 返回 `OK` / `NO RESPONSE` / `TIMEOUT` / `BAD CHECKSUM` / `OUT OF RANGE` / `UNKNOWN` |

#### 数据结构

```cpp
enum DhtStatus : uint8_t {
  DHT_OK = 0,
  DHT_ERR_NO_RESPONSE,
  DHT_ERR_TIMEOUT,
  DHT_ERR_CHECKSUM,
  DHT_ERR_RANGE
};

struct DhtReading {
  float temperature = 0.0f;
  float humidity = 0.0f;
  bool tempValid = false;
  bool humValid = false;
  uint8_t status = DHT_OK;
  uint32_t atMs = 0;
};
```

#### 时序与位判决

| 阶段 | 实现 | 手册要求 |
|---|---|---|
| 起始信号 | `digitalWrite(pin, LOW); delay(20)` | ≥ 18 ms |
| 等待应答低 | 循环上限 `cyclesPerUs * 100` | 80 µs |
| 等待应答高结束 | 同上 | 80 µs |
| 收 40 位 | `noInterrupts()` 期间逐位测宽，每位低/高各有循环上限 | 每位 50 µs 低 + 26–28 µs(0) / 70 µs(1) 高 |
| 结束 | 50 µs 低电平后释放总线 | — |

位判决关键行：

```cpp
if (highCycles > lowCycles) { data[i / 8] |= (1u << (7 - (i % 8))); }
```

比较**本位**高低电平的实测宽度，**不能用名义 50 µs 阈值**——`digitalRead` 循环开销会放大阈值，
导致 40 位全判 0。

校验和：`data[0] + data[1] + data[2] + data[3] == data[4]`。
解码：`humidity = data[0]`、`temperature = data[2]`（`data[1]` / `data[3]` 为小数位，恒 0）。

### 5.3 接线（DHT11）

| DHT11 模块排针 | ESP32-S3 | 说明 |
|---|---|---|
| **DATA**（有时标 S / OUT） | **GPIO 7** | 用户指定；模块自带上拉 |
| VCC（+） | 3V3 | |
| GND（-） | GND | |

> 引脚编号写在 [`dht11-oled/src/main.cpp`](dht11-oled/src/main.cpp) 顶部的 `PIN_DHT_DATA`。
> DHT11 与 OLED 用的是两组互不相干的引脚，可以同时接。

### 5.4 采样节拍

| 常量 | 值 | 说明 |
|---|---|---|
| `TICK_MS` | `1000` | 主循环节拍 |
| `SAMPLE_MS` | `1000` | 采样间隔（1 Hz 上限内） |
| `RETRY_MS` | `2000` | 失败后的重试间隔 |
| `DHT_WARMUP_MS` | `1500` | 上电预热，期间不判失败 |
| `OLED_RETRY_MS` | `5000` | 屏掉线后的重试间隔 |

失败重试节拍：

```cpp
hasReading ? SAMPLE_MS : ((now < DHT_WARMUP_MS) ? UINT32_MAX : RETRY_MS)
```

---

## 6. 硬件踩坑记录

### 6.1 GPIO 1 / GPIO 2 上 I2C 完全不通（实测）

2026-10-14 首轮调试时，OLED 接 **GPIO1(SCL) / GPIO2(SDA)**，表现为：

| 检查项 | 实测 |
|---|---|
| 0x3C 探测 | 无应答 |
| `i2c-scan` 全段 0x08–0x77 | 每个地址 `endTransmission` 返回 **5**（**超时**） |
| `bus-probe` GPIO1/GPIO2 | `pullup=HIGH float=HIGH`（多轮），即**高阻态、外部无器件驱动或上拉** |

换上 **GPIO4 / GPIO5** 后立刻通了。

> **重要区别**：`endTransmission()` 返回 **2** = 「该地址无应答」（器件不在这个地址），
> 返回 **5** = 「超时」（总线根本没法完成事务）。
> 看到 5 就不要再纠结地址了，直接查物理层接线。

### 6.2 下游器件不在线时不要刷新（实测）

`flush()` 发 8 页 × 2 次 I2C 事务，屏不在线时每次 `endTransmission()` 都阻塞到 50ms 超时，
一轮刷新近 **800ms** —— 足以把 1 秒的节拍带偏。

因此 `flush()` 在探测失败后直接返回，**计时精度与屏幕在线状态完全解耦**。
实测：屏缺席时仍为 **45 tick / 45 秒 = 1.0000 tick 每秒**，无漂移。

> 所有驱动统一 `Wire.setTimeOut(50)`（总线卡死时不无限等待），
> 这个 50 ms 就是上面 800 ms 的来源：8 页 × 2 次事务 × 50 ms。

---

## 7. 工具链版本（本机实测）

| 组件 | 版本 |
|---|---|
| PlatformIO Core | **6.2.0** |
| Python | 3.10.4 |
| 操作系统 | Windows 10 |
| PlatformIO 安装目录 | `C:\Users\EmoBS\.platformio` |
| `platform` espressif32 | **7.1.3** |
| `framework-arduinoespressif32` | **4.20017.260907+sha.dcc1105b** |
| `toolchain-xtensa-esp32s3` | 8.4.0+2021r2-patch5 |
| `tool-esptoolpy` | 2.41100.260830（esptool.py v4.11.0） |

---

## 8. 第三方库依赖

| 项目 | 第三方依赖 | 说明 |
|---|---|---|
| `uptime-oled` | **无** | 只用 Arduino core 自带的 `Wire` |
| `clock-oled` | **无** | OLED 复用 `uptime-oled` 的裸驱；联网只用 core 自带的 `WiFi`（内含 SNTP 客户端），未引入 NTPClient / ESP32Time |
| `dht11-oled` | **无** | DHT11 时序与 SSD1306 驱动均自写，只用 core API |
| `lcd1602-uptime` | **无** | 只用 Arduino core 自带的 `Wire` |
| `blink3` | `adafruit/Adafruit NeoPixel@^1.15.5`（实际拉取 **1.15.5**） | **仅** `ENABLE_ONBOARD_RGB=1` 板载 WS2812 模式需要；外接三灯模式零依赖 |
| `pin-probe` | **无** | 只用 core API |

### 自写驱动清单

OLED 驱动为自写裸驱（`uptime-oled/src/ssd1306_i2c.cpp` + `.h` + `font5x7.h`，约 400 行）：
自带 1KB 显存缓冲、5×7 点阵字库（95 个 ASCII 字形）、2 倍放大绘制、分块传输。
`clock-oled` 版（`clock-oled/src/ssd1306_i2c.cpp` + `.h`，约 435 行）另加
16×16 点阵中文字库、任意像素行定位与通用贴图 `drawBits()`。

1602 驱动同样是自写裸驱（`lcd1602-uptime/src/lcd1602_i2c.cpp` + `.h`，约 335 行）：
HD44780 4 位模式初始化序列、PCF8574 影子寄存器、两个 nibble 合并进一次 I2C 事务。
1602 内置字库，**不需要外挂点阵字模**。

DHT11 时序驱动为自写（`dht11-oled/src/dht11.h`），接口见 [§5.2](#52-自写时序驱动零第三方依赖)。

### Adafruit NeoPixel（`blink3` 板载灯模式）

| API | 说明 |
|---|---|
| `Adafruit_NeoPixel(uint16_t pixels, uint16_t pin, neoPixelType type = NEO_GRB + NEO_KHZ800)` | 构造：灯珠数、数据脚、时序类型 |
| `bool begin(void)` | 初始化 |
| `void setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b)` | 设某颗灯的颜色 |
| `void setBrightness(uint8_t)` | 全局亮度 |
| `static uint32_t Color(uint8_t r, uint8_t g, uint8_t b)` | 打包成 32 位色值 |

`NEO_GRB` 表示传输顺序是 **G, R, B**（不是 R, G, B）。许可证 BSD-3-Clause。

---

## 9. 资源占用

> **口径说明**：RAM = `.dram0.data + .dram0.bss`（ELF 段合计）；Flash = `firmware.bin` 镜像总大小（含 image header、分区表与校验），故比 ELF 段合计略大。下表数字沿用构建时记录值。

### `uptime-oled`

| 项目 | 占用 |
|---|---|
| RAM | 20,212 字节 / 327,680（**6.2%**） |
| Flash | 282,973 字节 / 3,342,336（**8.5%**） |
| 编译告警 | 0（开启 `-Wall -Wextra`） |

### `lcd1602-uptime`

| 项目 | 占用 |
|---|---|
| RAM | 19,188 字节 / 327,680（**5.9%**） |
| Flash | 282,469 字节 / 3,342,336（**8.5%**） |
| 编译告警 | 0（开启 `-Wall -Wextra`） |

### `dht11-oled`

| 项目 | 占用 |
|---|---|
| RAM | 20,268 字节 / 327,680（**6.2%**） |
| Flash | 286,333 字节 / 3,342,336（**8.6%**） |
| 编译告警 | 0（开启 `-Wall -Wextra`） |

### `clock-oled`

| 项目 | 占用 |
|---|---|
| RAM | 46,144 字节 / 327,680（**14.1%**） |
| Flash | 720,125 字节 / 3,342,336（**21.5%**） |
| 编译告警 | 0（开启 `-Wall -Wextra`） |

> RAM / Flash 明显高于 `uptime-oled`，全部来自 `libWiFi` 协议栈
> （联网对时的必然代价），与 OLED 或时钟逻辑无关。

### `status-oled`

| 项目 | 占用 |
|---|---|
| RAM | 20,392 字节 / 327,680（**6.2%**） |
| Flash | 291,293 字节 / 3,342,336（**8.7%**） |
| 编译告警 | 0（开启 `-Wall -Wextra`） |

> 与 `uptime-oled` 基本持平，差额来自四页渲染、按钮状态机、总线扫描字符串
> 与 `FLASH` 行的分区查询。
>
> **已上板**（2026-10-15，COM4）：1 秒节拍实测准确（75 秒无漂移），
> 按键翻页正常，OLED 在 `0x3C` 在线，
> `RTC_NOINIT_ATTR` 计数跨硬复位保留（`boot#15` → `boot#17`）。
> 运行时读数 `HEAP 357/382K`、`CPU 240MHz`、
> `psram 8386295 bytes`（补 `-DBOARD_HAS_PSRAM` 之后，见 §1.3.1）。
> 注意 Flash 占用会随代码微调变动，以最新一次构建为准。
>
> **V1.1**（2026-10-16，已上板）在 V1.0 基础上给三页各加一行，
> 四页统一为 6 行数据：`TEMP`（芯片温度，P1）、`STACK`（loopTask 已用/总栈，P3）、
> `PSRAM`（已用/总量，P4）。RAM 增量 +48 字节、Flash +3,452 字节，
> 主要来自温度传感器驱动（`libdriver.a`）的首次链接。
> 运行时实测 `TEMP 36.8C`、`STACK 26628/32768B`、`PSRAM 8386051/8386295B`。
>
> **`STACK` 的单位是字节**：`uxTaskGetStackHighWaterMark()` 在 ESP-IDF 里
> 返回字节而非原版 FreeRTOS 的 words（`task.h` 注释即如此声明，本仓库
> 另以 `tasks.c.obj` 反汇编坐实——该函数逐字节比对 `0xA5` 填充值）。
> 栈总量 = `CONFIG_ARDUINO_LOOP_STACK_SIZE`（8192）× 4 = 32768 字节。
> 已用约 81%，余量约 6K，是 `loop()` 深层调用链（`snprintf` / `printf`）的正常水位。

### `status-oled/tools/i2c-scan`

| 项目 | 占用 |
|---|---|
| RAM | 19,164 字节 / 327,680（**5.8%**） |
| Flash | 280,909 字节 / 3,342,336（**8.4%**） |
| 编译告警 | 0（未开 `-Wall -Wextra`，与既有 `i2c-scan` 副本一致） |

---

## 10. 诊断工具

仓库共 8 个诊断工具工程（5 类工具，`i2c-scan` 现有三份副本），
每个自带 `platformio.ini`，可独立烧录：

| 工具 | 位置 | 用途 |
|---|---|---|
| `i2c-scan` | `uptime-oled/tools/i2c-scan`、`lcd1602-uptime/tools/i2c-scan`、`status-oled/tools/i2c-scan` | 扫描 `0x08–0x77` |
| `bus-probe` | `uptime-oled/tools/bus-probe`、`dht11-oled/tools/bus-probe` | 读 SCL/SDA 静态电平 |
| `link-probe` | `lcd1602-uptime/tools/link-probe` | 1602 总线上拉判读 |
| `power-probe` | `lcd1602-uptime/tools/power-probe` | 供电链路检查 |
| `dht-probe` | `dht11-oled/tools/dht-probe` | DHT11 数据线上拉判读 |

共性参数：`PIN_I2C_SCL=4` / `PIN_I2C_SDA=5`（`dht-probe` 用 `PIN_DHT_DATA=7`）、
`Serial.begin(115200)`（`blink3` / `pin-probe` 提为常量 `SERIAL_BAUD=115200`）、
`Wire.setTimeOut(50)`。

`i2c-scan` 的扫描速率用 **100 kHz** 而不是 400 kHz，注释说明扫描阶段优先容错；
`i2c-scan` 每轮之间停 2ms/3000ms，`bus-probe` / `link-probe` / `dht-probe` 每 3000 ms 复测一次。

> `status-oled` 的 P4 总线页自带一次 0x08–0x77 扫描（启动时一次，之后每 10 秒静默重扫），
> 所以它的 `i2c-scan` 副本是"怀疑 P4 读数不对时"的第二意见，不是日常必跑的工具。
`i2c-scan` 只把 `err != 0 && err != 2` 的结果打成 `[ERR]` 行 —— 这正是判读「无应答」与「超时」的分界。

### `endTransmission()` 错误码

| 返回 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 数据过长 |
| 2 | 地址无应答（**器件不在这个地址**） |
| 3 | 数据无应答 |
| 4 | 其它 |
| 5 | 超时（**总线根本没法完成事务**） |

---

## 11. 显示屏：ESP32-S3-RLCD-4.2 全反射屏（官方资料已核实，待 bringup）

### 11.0 本节的证据等级声明

本节参数**不是本机实测**——来自**微雪官方资料的离线归档**（`.doc/ESP32-S3-RLCD/`，已被 `.gitignore` 排除、不入库），
经官方文档站 <https://docs.waveshare.net/ESP32-S3-RLCD-4.2/> 核对。

按本文件开头的三分类，本节整体标注为 **规格已核实**，凡带「⚠️ 待 bringup 验证」字样的条目表示**尚未在本机跑通**。
本节不做实测的原因是：本项目（`rlcd-workbench`）目前**只有方案、没有固件**，M0 之前没有可测对象。

> 📌 引用规则：本项目文档引用本节时用 `§11.x` 定位（章节锚点）。**官方归档不进库**，所以本节是可复核的唯一落点。

### 11.1 板级参数与显示几何

| 项目 | 参数 | 依据 |
|---|---|---|
| 型号 | **Waveshare ESP32-S3-RLCD-4.2** | 官方产品页 |
| 主控 | ESP32-S3-WROOM-1-N16R8（240 MHz 双核 LX7，16 MB Flash + 8 MB OCT PSRAM） | 官方 BSP |
| 屏幕 | 4.2 寸**全反射**（Reflective LCD）黑白屏，**1bpp** | 官方规格 |
| 驱动 IC | **ST7305**（SPI 接口） | 官方 BSP / 例程 |
| 物理尺寸 | 300 × 400 | 官方文档 |
| **驱动尺寸** | **400 宽 × 300 高（横屏）** | 官方 BSP factory `main.cpp`、官方 u8g2 例程、小智固件 `config.h` 三方一致 |
| 背光 | **无**。靠环境光成像，**暗处不可见** | 官方规格 |
| 颜色 | `ColorBlack = 0` / `ColorWhite = 0xff` | 官方 `09_LVGL_V9_Test/display_bsp.h:12-14` |
| 帧缓冲 | 全缓冲 **15200 字节**（304 字节/行 × 50 行） | 官方 u8g2 例程 `ST7305_BUFFER_ROW_BYTES` |
| SPI 时钟 | 官方例程 **24 MHz**（IDF 例程 20 MHz，官方 factory 10 MHz） | 官方例程 |

> ⚠️ **横屏这件事容易搞反**：官方文档写「300×400」是**物理**尺寸，驱动时按 **400 宽 × 300 高**。
> u8g2 官方只提供竖屏 300×400 的 ST7305 构造器，横屏要靠 `U8G2_R1` 换算 + 自定义 `DRAW_TILE`，
> 详见 `rlcd-workbench` 的 `ADR-0001`（`docs/` 暂不入库）。

### 11.2 按键

| 键 | 引脚 | 有效电平 | 备注 |
|---|---|---|---|
| **BOOT** | **GPIO0** | 低有效 + 内部上拉 | 官方 `multi_button` 支持单击 / 双击 / 长按。**按住 BOOT 重新上电会进下载模式**，故固件须丢弃上电 1 秒内的 BOOT 事件 |
| **KEY** | **GPIO18** | 低有效 + 上拉（R9/R17 10 kΩ） | 自定义功能键，5 ms 定时扫描 |
| **PWR** | **无 GPIO** | — | 纯电源开关：经 R45(1 kΩ) 驱动 Q5=AO3400，与 Q3/Q4=AO3401(PMOS)、D1=MBR230LSFT1G 构成 VSYS 开关。**不可读电平，不能当普通 IO** |

> ⚠️ 原理图上 GPIO17 处还标了一个 "2KEY"，但官方速查表把 GPIO17 记为**排母 GP17 空闲扩展**。**以速查表为准**，GPIO17 视为空闲。

### 11.3 电池、电源与指示灯

| 项目 | 参数 |
|---|---|
| 电池采样脚 | **GPIO4**（ADC1_CH3，12 bit，12 dB 衰减） |
| 分压网络 | VBAT → **R21 200 kΩ 1%** → 分压点 → **R23 100 kΩ 1%** → GND；C21/C33 100 nF 并联滤波。**分压比 = 1/3** |
| 官方电压还原 | `vol = 0.001 * tage * 3`（`tage` 为分压点校准毫伏） |
| 官方电量换算 | `< 3.0 V → 0%`；`> 4.12 V → 100%`；否则 `(vol - 3.0) / 1.12 * 100` |
| 充电管理 | **ETA6098**（U7） |
| **CHG 灯** | LED2，由 ETA6098 的 **STAT** 引脚直接驱动（经 R51 27 kΩ 到 GND）。**没有 GPIO 可读** |
| **WRN 灯** | LED1，接 BAT1+ 一端、经 R31 1.2 kΩ 接反接检测电路（Q5/R41）。**没有 GPIO 可读** |
| 电源链 | TPS63020 buck + RT9193 LDO |

> 📌 **软件无法判断「正在充电」**。官方小智固件的 `GetBatteryLevel` 把 `charging` 写死为 `false`。
> 因此界面只能显示电量百分比，**不能画充电图标**。
>
> 📌 CHG 灯**灭 = 充满**（不是常亮），WRN 灯**常亮 = 电池反接**。

### 11.4 触摸：控制器型号官方未公布（⚠️ 待 bringup 验证）

| 项目 | 事实 |
|---|---|
| 触摸控制器位置 | **在屏模组一侧**，通过 `TP_SDA` / `TP_SCL` 挂 I2C |
| 板上独立触摸 IC | **没有**（TP3 / TP4 仅测试点） |
| 中断脚 | **TP_INT = GPIO7**（输入） |
| 复位脚 | **TP_RESET = GPIO42**（输出） |
| **具体型号** | **官方未公布**。SensorLib 里备了 `TouchDrvCST226` / `CST816` / `CST92xx` / `GT9895` / `GT911` 五种驱动，但无法确定本屏模组用哪一种 |
| 触摸 FPC 脚序 | **不发布**（原理图 PDF 文本层的引脚编号有错位与噪声） |

> ⚠️ **触摸不能作为方案基石**。`rlcd-workbench` 的输入面只用 KEY + BOOT 两键（见其 ADR-0003）。

### 11.5 引脚总表

官方接口表在四个独立来源中逐项一致（官方速查表 §2、BSP `board_cfg.h`、ESPHome YAML、本表）。

| GPIO | 归属 | 备注 |
|---|---|---|
| **GPIO0** | BOOT 键 | 低有效 + 内部上拉；下载模式 |
| GPIO1 / 2 / 3 | 排母 GP1 / GP2 / GP3 | 空闲扩展 |
| **GPIO4** | **SYS BAT_ADC** | 电池电压采样，ADC1_CH3 |
| **GPIO5** | **LCD RS（DC）** | 数据 / 命令选择 |
| GPIO6 | LCD TE | 撕裂 / 帧同步 |
| GPIO7 | LCD TP INT | 触摸中断输入 |
| GPIO8 | ES8311 I2S DSDIN | 音频数据输入（ADC 侧） |
| GPIO9 | ES8311 I2S SCLK（BCLK） | |
| GPIO10 | ES8311 I2S ASDOUT | 编解码侧输出 |
| **GPIO11** | **LCD SCL** | 屏幕 SPI 时钟 |
| **GPIO12** | **LCD SDA** | 屏幕 SPI 数据 |
| **GPIO13** | **I2C SDA** | 触摸 / RTC / 排母扩展共用同一条数据线 |
| **GPIO14** | **I2C SCL** | 同上 |
| GPIO15 | RTC INT | |
| GPIO16 | ES8311 I2S MCLK | |
| GPIO17 | 排母 GP17 | 空闲扩展（见 §11.2 注） |
| **GPIO18** | **SYS KEY** | 自定义功能键 |
| GPIO19 / 20 | USB' N / P | 原生 USB，勿占 |
| GPIO21 | SD MOSI | |
| GPIO38 / 39 | SD SCK / MISO | |
| **GPIO40** | **LCD CS** | |
| **GPIO41** | **LCD RESET** | |
| GPIO42 | LCD TP RESET | |
| GPIO43 / 44 | U0TXD / U0RXD | UART0 |
| GPIO45 | ES8311 I2S LRCK（WS） | |
| **GPIO46** | **ES8311 PA CTRL** | NS4150B 功放使能 / 静音 |

**排母 2×8（原理图读出）**：左列 pin1–8 = VCC3V3, GND, GPIO0, GPIO1, GPIO2, GPIO3, GPIO17, GPIO18；
右列 pin9–16 = VBUS, GND, USB'_N(GPIO19), USB'_P(GPIO20), U0TXD(GPIO43), U0RXD(GPIO44), ESP32_SDA(GPIO13), ESP32_SCL(GPIO14)。

### 11.6 I2C 总线与器件地址

全板**一条 I2C**（I2C_NUM_0），**SDA = GPIO13 / SCL = GPIO14**，板载 5.1 kΩ 上拉（R20 / R22）+ 100 Ω 串阻（R25 / R26 / R27）。

| 地址 | 器件 | 说明 |
|---|---|---|
| **0x18** | ES8311 | 音频 codec（输出 + ADC） |
| **0x40** | ES7210 | 音频 ADC（双麦） |
| **0x51** | PCF85063 | RTC，**自带充电电池座**，断电保时间 |
| **0x70** | SHTC3 | 高精度温湿度 |

**四个地址互不冲突。** RTC_SDA 与 ESP32_SDA 是同一网络。

> ⚠️ 官方例程的 `I2cMasterBus I2cbus(14, 13, 0)` **参数序是 SCL 在前、SDA 在后**，与 Arduino `Wire.begin(sda, scl)` 相反。抄代码不要「顺手改成常规顺序」。

### 11.7 显示初始化与刷新模式

**ST7305 初始化序列有两套**（官方 BSP factory 版 vs 官方例程版），差异在 **4 组 gamma 寄存器**：

| 寄存器 | u8g2 官方 `u8x8_d_st7305.c` | 官方例程 / BSP factory / LVGL 例程（**本板用这套**） |
|---|---|---|
| C1 | `0x3C, 0x3E, 0x3C, 0x3C` | **`0x69` ×4** |
| C4 | `0x5A, 0x5C, 0x5A, 0x5A` | **`0x4B` ×4** |
| D8 | `0xA6, 0xE9` | **`0x80, 0xE9`** |
| B2 | `0x12` | **`0x02`** |
| C0 | `0x12, 0x0A` | `0x11, 0x04` |
| 反显 | `0x20`（关）+ `BB(0x4F)` | **`0x21`（开）** |

> 📌 **画面发灰 / 反差不对，几乎都是这 4 组 gamma 抄错了。**

**功耗与刷新档位**：

| 命令 | 含义 | 行为 |
|---|---|---|
| `0x38` | HPM | 面板进入高功耗自刷新档，改动立刻可见 |
| `0x39` | LPM | 面板进入低功耗自刷新档，**写入后要等约 1 秒才显示** |
| `0x28` | Display Off | 全黑（面板电路仍供电） |

**两档的实际帧率由 `0xB2`（FRCTRL 的 LFRA）决定**（数据手册 §8.2.3，p.76）：官方 u8g2 例程 `0xB2 = 0x02` → **LPM 1 Hz**；官方 BSP factory `0xB2 = 0x05` → **LPM 8 Hz**。

> ⚠️ **官方离线归档里没有任何 `0x39` 的先例**——例程、BSP、u8g2 驱动都只发 `0x38`，官方 u8g2 甚至把 LPM **注释掉**（`u8x8_d_st7305.c:789-790`）。数据手册 §7.11 的 HPM↔LPM 切换序列**是图片**，文本层里只有 §7.10 Sleep-In/Out 的 100/300/100 ms，**没有切换延时数值**。
>
> 📌 **官方速查表把 `0x38` 标成「RAMWR（写显存）」是错的**——`0x38` = HPM，RAMWR 是 `0x2C`。该表也没有 HPM/LPM 行，别拿它当依据。

**部分写入的粒度**：ST7305 的行列地址窗口以 **12 像素列块 × 4 行** 为最小单位（列地址**递减**，基准 `0x3C`；行窗口 `0x2B` + `{y_pos*4, y_pos*4+3}`）。

**对比度命令**：`0x81` + 10 位值（`u8x8_d_st7305.c:864-871`，拆成 `arg<<2` 与 `arg>>6` 两个参数字节）。

> ⚠️ 官方 u8g2 驱动的 `U8X8_MSG_DISPLAY_SET_POWER_SAVE` 分支复用了「122×250」的省电序列，
> 但内容实际就是 ST7305 的 `0x29` / `0x28` 显示开关——**这个「看起来像 bug 的地方」无害**。

### 11.8 音频链路（本期语音功能延期，见 `rlcd-workbench` ADR-0011）

| 项目 | 参数 |
|---|---|
| I2S | **只有一组，ES8311 与 ES7210 共用**：MCLK=16 / BCLK=9 / WS=45 / DIN=10 / DOUT=8 |
| 功放 | **NS4150B**（U12，Class-D 单声道），CTRL=**GPIO46**，`pa_gain=6`、`use_mclk=1` |
| 扬声器 | **MX1.25 2PIN 外接接口**（原理图标 PH2.0 2P），随板附送 **8 Ω 2 W** 喇叭。**板上没有板载喇叭** |
| 麦克风 | 两颗模拟麦座（MIC1 / MIC2）接 ES7210；MIC3 / MIC4 仅有网名无独立座体 |
| 采样率 | 官方 BSP **16 kHz**；小智固件 **24 kHz** |
| 格式 | MSB 32 bit slot、STEREO、16 bit |
| 驱动库 | `esp_codec_dev`（官方归档 `07_Audio_Test/src/ExternLib/esp_codec_dev/`）。**PlatformIO registry 上 404**，需作本地库引入 |
| ES7210 麦选择 | 官方两处不一致：Waveshare `codec_init.c` 用 `MIC1\|MIC3`（TDM 时加 `MIC2\|MIC4`）；小智固件用 MIC1–4 全开 + `AUDIO_INPUT_REFERENCE=true` |

### 11.9 官方 AI 固件与第三方路径

| 路径 | 状态 |
|---|---|
| **小智 AI 固件 v2.5.2** | 官方适配列表确认本板 = `v2.5.2_waveshare-esp32-s3-rlcd-4.2`（`merged-binary.bin` 11,289,121 B）。**走 ESP-IDF 编译，不能与自研固件共存、不能一键切换**；默认连小智官方服务端（文档称理论上可改连自有服务端，需相应配置） |
| OpenClaw | 官方教程 `docs.waveshare.net/MimiClaw` |
| ESP-Claw | 官方教程 `docs.waveshare.net/ESPClaw`，需 ≥8 MB Flash + PSRAM |
| ESPHome | 官方给有传感器仪表盘与扬声器铃声测试两份 YAML |
| ESP-ADF | 仅出现在 AIChats 仓库的 `doubao/esp-adf-example` 子模块，**不是本板主路径** |

### 11.10 SD 卡

| 项目 | 参数 |
|---|---|
| 模式 | **1-bit** |
| 引脚 | CLK = GPIO38 / CMD = GPIO21 / D0 = GPIO39（MISO = GPIO39） |
| 文件系统 | FAT32 |

### 11.11 官方例程清单（Arduino 侧）

`01_WIFI_AP` / `02_WIFI_STA` / `03_ADC_Test` / `04_I2C_PCF85063` / `05_I2C_SHTC3` / `06_SD_Card` /
`07_Audio_Test` / `09_LVGL_V9_Test` / `10_U8G2_Test`。

**统一要求**：开发板选 ESP32S3 Dev Module、Flash 16MB、**Partition Scheme 16M Flash (3MB APP / 9.9MB FATFS)**、PSRAM 选 OPI PSRAM、USB CDC On Boot Enabled。

**版本基线**：Arduino-ESP32 ≥ 3.3.0（官方验证 3.3.12）、**U8g2 v2.36.18**、LVGL v9.3.0（Arduino 侧；IDF 侧 v8.3.11）、SensorLib v0.3.1、ESP-IDF ≥ 5.5.0（官方验证 5.5.5）。

---

## 12. 相关文档

### 数据手册原始出处

本节参数均从厂商一手文档核出，不依赖二手转述。原始 PDF 抓取件与文字层提取存放在
`.tmp-research/`（**已被 `.gitignore` 排除**，不入库）：

| 资料 | 出处 | 提取件 |
|---|---|---|
| ESP32-S3 数据手册 v2.2 | [espressif.com](https://www.espressif.com/documentation/esp32-s3_datasheet_en.pdf) | `.tmp-research/ESP32S3-FACTS.md` |
| TCA9548A 数据手册 `SCPS207H` | [ti.com](https://www.ti.com/lit/ds/symlink/tca9548a.pdf) | `.tmp-research/TCA9548A-FACTS.md` |

> 提取件里的引脚表、地址表、控制寄存器原文，是本节 §1、§4 参数的依据；需要复核时看这两个文件。

### 仓库内文档

- 项目说明与屏幕布局：[`uptime-oled/README.md`](uptime-oled/README.md)
- 联网时钟（NTP 对时 + 漂移说明）：[`clock-oled/README.md`](clock-oled/README.md)
- DHT11 温湿度监测（含弃用 Adafruit DHT 库的原因）：[`dht11-oled/README.md`](dht11-oled/README.md)
- 1602 项目（含 5V 电平转换专章）：[`lcd1602-uptime/README.md`](lcd1602-uptime/README.md)
- 板载 WS2812 引脚判定：[`pin-probe/README.md`](pin-probe/README.md)
- 养成游戏策划案（三屏 + PCA9548A 方案）：[`pet-oled3/docs/策划案.md`](pet-oled3/docs/策划案.md)
- 养成游戏的硬件接口决策（**与 `status-oled3` 同一套接线**）：[`pet-oled3/docs/adr/0006-hardware-interface-aligned-with-status-oled3.md`](pet-oled3/docs/adr/0006-hardware-interface-aligned-with-status-oled3.md) / [两键输入决策](pet-oled3/docs/adr/0007-two-buttons-only-input.md)
- 养成游戏的数值参数与交互面定稿：[数值平衡](pet-oled3/docs/adr/0008-numeric-balance-first-pass-parameters.md) / [两键交互面](pet-oled3/docs/adr/0009-two-button-interaction-coverage.md) / [蜂鸣器驱动强度](pet-oled3/docs/adr/0010-buzzer-direct-drive-needs-strongest-drive-cap.md)
- 桌面状态显示器（三屏 + 按钮 + 蜂鸣器，**M0 / M1a / M1b 已上板实测通过**）：[`status-oled3/README.md`](status-oled3/README.md)
- 状态显示器的决策记录（三屏分工、聚合端点、引脚分配等）：[`status-oled3/docs/adr/`](status-oled3/docs/adr/)
- **RLCD 单屏多面板工作台**（4.2 寸全反射黑白屏 + 两键 + 温湿度 / 电量 / RTC + 服务器健康 + 待办，**方案阶段，无固件**）：[`rlcd-workbench/README.md`](rlcd-workbench/README.md)
- RLCD 工作台的术语表（面板布局、手势矩阵、里程碑与断言的术语定义）：[`rlcd-workbench/GLOSSARY.md`](rlcd-workbench/GLOSSARY.md)
- RLCD 工作台的策划案与 16 篇决策记录（显示栈选型、输入面、待办契约、语音延期等）：`rlcd-workbench/docs/` —— **暂不入库**，见本地工作区
- 关键决策记录（PCA9548A 选型、单通道轮询、开机时间推进等）：[`pet-oled3/docs/adr/`](pet-oled3/docs/adr/)
- 术语表（统一命名）：[`pet-oled3/CONTEXT.md`](pet-oled3/CONTEXT.md)
- 仓库总览：[`README.md`](README.md)

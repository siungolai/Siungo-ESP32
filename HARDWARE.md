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
| GPIO 4 / GPIO 5 | I2C SCL / SDA（各项目实测） | 本机跑通的 I2C 脚位，见 [§2.3](#23-接线ssd1306-oled) |
| GPIO 7 | **按项目分角色，同一时间只插一个**：`dht11-oled` 的 DHT11 数据脚 / `status-oled` 的翻页按钮 | 两者不能同时插——DHT11 模块自带 4.7k~10k 上拉，会与按钮要用的内部上拉并联，见 [§5.3](#53-接线dht11) |
| GPIO 26–32 | SPI0/1，封装内 Flash/PSRAM 已占 | 规格已核实，不推荐他用 |
| GPIO 33–37 | 8 线 SPI 高位数据线 | 本板 OPI PSRAM 已占用 |
| GPIO 38 | 规划中：编码器按键 | 待 bringup 验证 |
| GPIO 43 / GPIO 44 | UART0（Universal Asynchronous Receiver-Transmitter，通用异步收发器） | 规格已核实 |
| GPIO 39–42 | JTAG（Joint Test Action Group） | 规格已核实 |
| GPIO 48 | 板载 WS2812 数据脚 | 规格已核实 + 本机实测 |

> ESP32-S3 的 GPIO 分三档驱动能力：默认驱动强度 GPIO17/18 = 10mA、GPIO19/20 = 40mA、其余 20mA；累计 IO 输出电流上限 1500mA。数据手册未给出每脚绝对最大电流——常见引用的 40mA 是 `PAD_DRIVER=3` 档的典型源电流，不是绝对最大值。

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

## 4. I2C 多路复用器：PCA9548A（8 通道）—— 规格已核实，待 bringup 验证

> **状态：已购成品模块，尚未接线验证。** 本节的"已核实"是**规格事实**（来自 TI 官方数据手册），
> 不是"本机实测可用"。上游/下游上拉归属见 [§4.3](#43-上拉电阻归属)，
> 上电后第一件事是显式选通，见 [§4.2](#42-控制寄存器的三个实现细节) 的提示框。
> bringup 验证清单见 [§4.8](#48-待本机验证bringup-必做)。

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
| **SDA** | **GPIO 8** | 建议值，待 bringup 实测确认 |
| **SCL** | **GPIO 9** | 同上 |
| VCC | 3V3 | 模块支持 1.65–5.5 V |
| GND | GND | |
| A0 / A1 / A2 | GND（跳线） | 目标地址 **0x70**，**以实际扫描结果为准**。地址表：LLL=0x70 / LLH=0x71 / LHL=0x72 / LHH=0x73 / HLL=0x74 / HLH=0x75 / HHL=0x76 / HHH=0x77 |
| RESET | 3V3 或悬空 | active-low；多数模块已内部上拉。bringup 阶段先悬空 |

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

### 4.7 规划中的驱动接口（尚未实现）

以下接口来自 [`pet-oled3/docs/策划案.md`](pet-oled3/docs/策划案.md) §5.2 的设计稿，**代码尚未实现**，仅记录设计约定：

| 接口 | 约定 |
|---|---|
| `bool pca9548a_select(uint8_t channel)` | `channel` 取 0–7；只选通这一路，其余全部断开 |
| `bool pca9548a_deselect_all()` | 全部断开 |
| `bool Display::begin(uint8_t channel)` / `void Display::flush()` | 每块屏绑定一个通道，`flush()` 前自动选通、用完断开 |

驱动内部维护通道缓存：`select(n)` 若 `n` 等于当前通道，直接返回、不产生 I2C 事务。设计上**刻意不暴露** `pca9548a_write_mask(uint8_t mask)`，避免调用方同时选通多路触发上拉并联。

选通的寄存器写入必须覆盖 [§4.2](#42-控制寄存器的三个实现细节) 第 2 条：一次完整的 `beginTransmission(0x70) → write(1 << channel) → endTransmission()`，等 STOP 之后通道才真正生效。

### 4.8 待本机验证（bringup 必做）

- [ ] 扫描 0x70–0x77，确认 PCA9548A **实际地址**（跳线可能焊死，不一定 0x70）
- [ ] 万用表量各通道 SDA/SCL 对 VCC 阻值，确认模块上拉**挂在上游还是八路全挂**
- [ ] 逐通道探测 0x3C，确认三块屏都能被找到
- [ ] **三块屏显示不同内容**（同内容无法区分是否真的切换成功）
- [ ] 记录 `endTransmission()` 是否返回 5（超时）
- [ ] 确认 GPIO 8 / GPIO 9 作上游 SDA / SCL 在本机可用
- [ ] 验证「选通在一次完整事务的 STOP 之后才生效」（见 [§4.2](#42-控制寄存器的三个实现细节)）

> 完成后回写本节：把「规格已核实」改为「实测可用」，并把 [§4.7](#47-规划中的驱动接口尚未实现) 的规划接口替换为实现签名。
> 项目背景与完整接线表见 [`pet-oled3/docs/策划案.md`](pet-oled3/docs/策划案.md) 的 M0 里程碑。

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
| RAM | 20,344 字节 / 327,680（**6.2%**） |
| Flash | 287,841 字节 / 3,342,336（**8.6%**） |
| 编译告警 | 0（开启 `-Wall -Wextra`） |

> 与 `uptime-oled` 基本持平（Flash 多 4,868 字节），差额来自四页渲染、
> 按钮状态机、总线扫描字符串与 `FLASH` 行的分区查询。
>
> **已上板**（2026-10-15，COM4）：1 秒节拍实测准确（75 秒无漂移），
> 按键翻页正常，OLED 在 `0x3C` 在线，
> `RTC_NOINIT_ATTR` 计数跨硬复位保留（`boot#15` → `boot#17`）。
> 运行时读数 `HEAP 358/382K`、`CPU 240MHz`、
> `psram 8386295 bytes`（补 `-DBOARD_HAS_PSRAM` 之后，见 §1.3.1）。
> 注意 Flash 占用会随代码微调变动，以最新一次构建为准。

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

## 11. 相关文档

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
- 桌面状态显示器（三屏 + 按钮 + 蜂鸣器，策划中）：[`status-oled3/README.md`](status-oled3/README.md)
- 状态显示器的决策记录（三屏分工、聚合端点、引脚分配等）：[`status-oled3/docs/adr/`](status-oled3/docs/adr/)
- 关键决策记录（PCA9548A 选型、单通道轮询、开机时间推进等）：[`pet-oled3/docs/adr/`](pet-oled3/docs/adr/)
- 术语表（统一命名）：[`pet-oled3/CONTEXT.md`](pet-oled3/CONTEXT.md)
- 仓库总览：[`README.md`](README.md)

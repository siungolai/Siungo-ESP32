# 10. 蜂鸣器供电：直驱可行，但必须提到最强驱动档

日期：2026-10-21

## 状态

已接受

## 背景

[`../策划案.md`](../策划案.md) §7 第 12 项「蜂鸣器直驱电流」自 2026-10-21 起挂在待验证清单上，理由是「有源蜂鸣器典型 20–30 mA，ESP32-S3 单脚默认 20 mA」，验证方法写的是「听音量 + 摸脚温」。

这条待验证项的问题是：**它把一个可以查规格书定案的问题，写成了必须上手试的问题。**

接线事实（`status-oled3` bench 实测，`status-oled3/README.md` 记录）：

```
GPIO 21  ──→  蜂鸣器正极（长脚）
GND     ──→  蜂鸣器负极
```

**GPIO 21 是源端（source current），走 `I_OH` 通道**——蜂鸣器把电流从 GPIO 拉回地。这一点很重要：之前有人按「GPIO 灌电流」的方向算过，结论是错的（汇流与源流的上限不同）。

## 取证：ESP32-S3 datasheet v2.2

| 数据 | 值 | 出处 |
|---|---|---|
| 各脚默认驱动强度 | GPIO17/18 = 10 mA；GPIO19/20 = 40 mA；**其余 = 20 mA** | datasheet v2.2 §2.3.4 注 5（"The default drive strengths for each pin are as follows"） |
| **GPIO 21 属于哪一档** | **"All other pins" = 20 mA 默认档** | 同上（GPIO 21 不在 17/18/19/20 三个例外里） |
| `I_OH` 高电平源电流 | **40 mA**（VDD = 3.3 V，V_OH ≥ 2.64 V，**PAD_DRIVER = 3**） | datasheet v2.2 IO 电气特性表 |
| `I_OL` 低电平汇电流 | 28 mA（VDD = 3.3 V，V_OL ≤ 0.495 V，PAD_DRIVER = 3） | 同上 |
| 累计 IO 输出电流 | **1500 mA**（全部 IO 合计） | datasheet v2.2 Absolute Maximum Ratings 表 |
| 每脚绝对最大电流 | **datasheet 未给出** | —— |

> 📌 **40 mA 不是「每脚绝对最大电流」**，它是 `PAD_DRIVER = 3` 档下 `V_OH ≥ 2.64 V` 时的**典型源电流**。数据手册没有给每脚的绝对最大电流上限——本仓库 [`../../../HARDWARE.md`](../../../HARDWARE.md) §1.2 尾注已记下这一点，不要把 40 mA 当成「可以长期流 40 mA」的许可。

## 关键发现：默认 20 mA 档不够，但提升驱动强度是**一行代码**

有源蜂鸣器典型工作电流 **20–30 mA**。对照上面的表：

| 情况 | 源电流能力 | 够不够 |
|---|---|---|
| **GPIO 21 默认档（20 mA）** | 20 mA | ⚠️ **临界**——勉强驱动 20 mA 型号，30 mA 型号会欠压（`V_OH` 掉到 2.64 V 以下，蜂鸣器音量下降甚至不响） |
| **GPIO 21 提到 `PAD_DRIVER = 3`** | **40 mA** | ✅ **有余量** |

**而「默认 20 mA」是 Arduino 核心帮我们设的，不是硬件限制。**

`gpio_set_drive_capability()` 的可用性核查结果：

| 检查项 | 结论 |
|---|---|
| esp-idf 里有没有这个 API | ✅ 有——`esp_err_t gpio_set_drive_capability(gpio_num_t gpio_num, gpio_drive_cap_t strength);`（`driver/gpio.h`） |
| 枚举有没有 `GPIO_DRIVE_CAP_3` | ✅ 有——`GPIO_DRIVE_CAP_0=weak / _1=stronger / _2=medium / _3=strongest`，`GPIO_DRIVE_CAP_DEFAULT = 2`（`hal/gpio_types.h`） |
| Arduino 核心的 `pinMode()` 会设驱动强度吗 | ❌ **不会**——`__pinMode()` 只调 `gpio_config()`（模式/上拉/中断类型），**完全不碰 `FUN_DRV`** |
| 需要额外 include 吗 | ⚠️ **需要显式 `#include <driver/gpio.h>`**——但 `Arduino.h` 的 include 链（`Arduino.h` → `esp32-hal.h` → `esp32-hal-dac.h` → `driver/gpio.h`）**实际已经把它带进来了**，Arduino 工程里可以直接用 |
| Arduino 核心自己用不用 | ✅ 用——`esp32-hal-tinyusb.c:76-77` 就是 `gpio_set_drive_capability(USBPHY_DM_NUM, GPIO_DRIVE_CAP_3)` |

## 决策

**蜂鸣器直驱 GPIO 21，但固件必须把该脚的驱动强度提到 `GPIO_DRIVE_CAP_3`。**

```cpp
// 蜂鸣器驱动初始化处（buzzer::begin()）
#include <driver/gpio.h>

void begin() {
    // 顺序：先设驱动强度，再把脚配成输出。
    // 有源蜂鸣器典型 20-30 mA；GPIO 21 默认驱动档只有 20 mA（datasheet v2.2 §2.3.4 注 5），
    // 提到 PAD_DRIVER=3 才有 40 mA 源电流（IO 电气特性表 I_OH）。
    gpio_set_drive_capability(static_cast<gpio_num_t>(config::PIN_BUZZER), GPIO_DRIVE_CAP_3);
    pinMode(config::PIN_BUZZER, OUTPUT);
}
```

> 📌 **顺序说明**：本 ADR 初版示例把 `pinMode()` 写在前面，**M0 实现改为先设驱动强度**（`pet-oled3/src/buzzer.cpp`）。理由：`pinMode()` 会把脚配成输出，此时若负载已经在拉电流，芯片可能已经在默认档下工作；先设强度再配输出，才能保证从第一拍起就是强驱动。两种顺序在实测中都能工作，但**先设后配**没有中间态。

### 后果

**正面**

- **第 12 项结案，不需要买万用表、也不需要靠摸脚温**。判定从「听音量 + 摸脚温」变成一行可验证的代码。
- **有 2× 的余量**（40 mA vs 典型 30 mA），不同批次的有源蜂鸣器电流差异不会被卡住。
- **零额外成本**——不占脚位、不加三极管、不改硬件。

**负面 / 需承担的成本**

1. **多一行对 IDF 内部 API 的依赖**。`gpio_set_drive_capability()` 不是 Arduino API，是 esp-idf API。它在当前框架版本（`framework-arduinoespressif32` 4.20017.260907）可用，但**框架升级时需要重新确认**。
2. **驱动强度提高会让该脚的边沿更陡**，理论上对 EMI 略有影响。本项目蜂鸣器是短促的通断节奏（≤ 400 ms），实际影响可忽略。
3. **仍然没有余量给「加三极管」的方案**——但那是备选，不是本决策。若实测音量确实不够（说明实际电流 > 40 mA 或供电压降过大），再上 NPN 三极管，**脚位不变**。

### 兜底方案（未启用，留档）

若实测发现音小或脚烫：加一只 **NPN 三极管**（GPIO 21 驱动基极，蜂鸣器串在集电极/发射极之间）——**脚位不变，仍是 GPIO 21**。这是 `status-oled3` 同款蜂鸣器的已知风险项。

## 备注

- **本决策不改变脚位**。[ADR-0006](0006-hardware-interface-aligned-with-status-oled3.md) 与 [ADR-0007](0007-two-buttons-only-input.md) 的硬件结论全部成立。
- **本项已从 [`../策划案.md`](../策划案.md) §7 结案**（第 12 项，2026-10-21）。**试玩验证没有取消**——「声音够不够大、好不好听」是主观项，已移入 M4 的听感打磨；本 ADR 定的是**电气余量**。
- **本决策不改变蜂鸣器的「有源型」结论**——仍然只能通断编码、不能出音阶（见 `status-oled3` 的 [ADR-0003](../../../status-oled3/docs/adr/0003-active-buzzer-on-off-encoding.md)）。
- **`status-oled3` 已同步落地**（2026-10-21，晚于本 ADR 首次落盘）：`status-oled3/src/buzzer.cpp` 的 `begin()` 现在**同样先调 `gpio_set_drive_capability()` 再 `pinMode()`**，`status-oled3/src/main.cpp` 的 boot report **同样用 `gpio_get_drive_capability()` 读回驱动档**并打印 `buzzer pin : GPIO21 drive cap=3 (want 3) readback ok`。两个项目现在是同一套做法，**不再是「照抄 vs 不照抄」的分叉**。

## 2026-10-21 上板验证（M0）

本 ADR 的代码要求已在 M0 固件里落地并**上板实测**：

| 检查项 | 结果 |
|---|---|
| 编译 | ✅ `gpio_set_drive_capability()` 在 Arduino 工程里可直接编译链接（`#include <driver/gpio.h>` 即可） |
| 落点 | `pet-oled3/src/buzzer.cpp` 的 `begin()`——**在 `pinMode()` 之前**调用 |
| **读回验证** | ✅ **驱动档现场读回 = 3**（`gpio_get_drive_capability()` 读寄存器），boot report 打印 `buzzer pin : GPIO21 drive cap=3 (want 3) readback ok` |
| 听感 | ✅ **5 种节奏用户确认全部可分辨**：click(20ms) / action(80ms) / alert×3 / levelup(400ms) / unhappy×2 |
| 脚温/音量 | 无异常（未加 NPN 三极管，兜底方案未启用） |

> 📌 **"代码里写了"不等于"生效了"**——所以 boot report 用 `gpio_get_drive_capability()` **把寄存器读回来**再打印，而不是只打印一个常量。这是本项目验证驱动强度的固定做法。
>
> 📌 **`status-oled3` 已于 2026-10-21 补齐**（本 ADR 首次落盘时它还是"只写不读"）：`buzzer::begin()` 现在先设驱动强度，boot report 也读回。**代码已改、已编译通过（RAM 16.3% / Flash 26.9%），但 `status-oled3` 的这版固件尚未上板复测**——它读回时应当看到 `drive cap=3`，这一点留待下次烧录时确认。

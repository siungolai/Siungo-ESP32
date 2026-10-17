# 10. 手写极简 JSON 取值器，不引 ArduinoJson

日期：2026-10-16

## 状态

已接受

## 背景

M1 设备侧要解析两个来源的 JSON：主站的 `GET /api/health`（[ADR-0009](0009-phase-in-with-existing-health-endpoint.md)）与自建的 `/status/snapshot.json`（[ADR-0008](0008-server-side-snapshot-via-cron.md)）。

需要回答的问题：**JSON 解析用库还是手写**。

已查明的事实：

- `status-oled3` 的 `platformio.ini` 与 `clock-oled` 完全同套，**没有 `lib_deps`**。整个仓库唯一的 `lib_deps` 是 `blink3` 的 Adafruit NeoPixel。本项目到 M0 为止是零依赖的裸驱风格。
- 两个端点的 JSON 都是**扁平结构**，无嵌套对象、无数组（指标快照的子项可以是扁平键，不需要层级）。
- `framework-arduinoespressif32` 自带 `WiFi` / `WiFiClientSecure` / `HTTPClient`，M1 不需要为网络加依赖。
- ESP32-S3 的资源基线：M0 固件 RAM 6.8%（22356/327680）/ Flash 8.6%（286541/3342336）；`clock-oled` 叠加 WiFi 栈后是 RAM 14.1% / Flash 21.5%（[`../../HARDWARE.md`](../../HARDWARE.md) §9）。

## 考虑的方案

### 方案 A：手写「按 key 取字符串值」的单文件工具 —— 采纳

新增 `src/jsonfield.h`（约 40 行），接口只有一个：

```cpp
// Returns the value of "key" as a NUL-terminated string, or "" when the key
// is absent, the value is not a string, or the JSON is malformed.
// out must have room for at most cap bytes including the terminator.
const char* jsonField(const char* json, const char* key,
                      char* out, size_t cap);
```

**采纳理由**：

1. **需求面恰好等于它的能力面**：8~15 个扁平字符串键，只需要「取出来」，不需要遍历、不需要改、不需要序列化。为一个操作引入一个通用 JSON 库是把问题做大。
2. **零新依赖**：保持本项目裸驱风格，`lib_deps` 继续为空。少一个依赖就少一个版本兼容与供应链面。
3. **内存与 Flash 代价已知且极小**：几十字节常量字符串 vs ArduinoJson 的 `DynamicJsonDocument` 池（其文档建议按最坏情况预留，容易把几百字节到几 KB 的堆吃掉）。
4. **失败模式可控**：函数只有「取到」与「没取到」两种结果，调用方对两种都有明确处理；不存在库的解析错误码体系要映射。
5. **两个端点结构相同**：`/api/health` 与 `snapshot.json` 都是扁平字符串键，同一份实现复用，不需要第二套解析路径。

### 方案 B：引入 ArduinoJson

**否决理由**：为 8 个扁平字符串键引入一个通用 JSON 库，收益（遍历、序列化、嵌套）在本项目**零调用方**，代价却包括一个新的 `lib_deps`、一个文档约定、一份堆占用，以及「升级库要重新验证固件」的长期义务。若将来真的需要嵌套或数组，届时再引也不迟——那时需求是真实的。

### 方案 C：引入正则或 sscanf 拼字符串

**否决理由**：可读性更差且边界情况更多，并不比方案 A 简单。

## 决策

手写 `jsonField()`，并**显式处理以下边界情况**（这是本决策的实质内容，比「不引库」更重要）：

1. **key 不存在** → 返回空串，调用方把空串当作「该字段本次不可用」，不得当作 `"0"` 或 `"ok"`。
2. **值是数字 / `true` / `false` / `null` 而非字符串** → 返回空串。指标快照里 CPU 使用率天然是数字，所以 `jsonField()` 之外再提供一个 `bool jsonNumber(const char* json, const char* key, double* out)`，数字与字符串两条路径分开，避免把 `37.5` 解析成字符串后再 `atof` 的隐式链。
3. **value 内含转义引号**（`\"`）→ 取值时要跳过反斜杠转义，不能在一个 `"` 上提前截断。当前两个端点都没有这种情况，但这是防御性要求，防止将来某字段出现引号时静默截断。
4. **响应被截断**（`HTTPClient` 实收字节少于 `Content-Length`）→ 按实收内容解析；截断正好落在某个 value 中间时该 value 取不到，表现为空串，由调用方按「字段不可用」处理。
5. **`"dev"` / `"unknown"` 与空串的区别**：`handler/health.go:36-50` 在 `Commit` / `Built` 未注入时给的是 `"dev"` / `"unknown"`，**不是空串**。设备侧显示时要把这两个值当作「未注入」而不是「某个真实的 commit 号」，串口打印要能区分。
6. **`Status` 恒为 `"ok"`**（ADR-0009 已记录）→ 代码注释里钉住「不得用该字段判断服务器健康」，真正的判据是「HTTP 200 且 JSON 可解析」。

缓冲区由调用方提供（栈上或静态数组），工具函数不分配内存——这与 SSD1306 驱动「构造不碰总线、framebuffer 静态分配」的既有风格一致。

## 后果

### 正面

- `lib_deps` 保持为空，依赖面不变。
- 堆占用可预测：只多调用方的几个缓冲区。
- 解析逻辑 40 行，评审时可以逐行读完。

### 负面 / 需承担的成本

1. **不支持嵌套与数组**：若将来某端点要返回对象数组（例如多个邮箱各自的状态），`jsonField()` 不够用，届时需要扩展或改引库。当前两个端点都不需要，属可预期的迭代点。
2. **手写解析器要自己保证不被畸形输入搞崩**：上表 6 条边界就是为此列的，实现时必须逐条对应写测试或串口验证。
3. **不是通用 JSON 库**：不能拿去解析任意 JSON，这个定位要在文件头注释写清，避免后人误用。

## 备注

- 本决策只覆盖设备侧解析。服务器侧生成 JSON 的是 bash + python3，不涉及本决策。
- 若 M1b 的 `snapshot.json` 需要嵌套结构（例如把每个服务的探活结果做成数组），应先改服务器侧输出为扁平键，而不是改设备侧解析器——扁平化服务器的成本低于在固件里维护解析器。

// rlcd-workbench / 硬件常量集中处
//
// 所有引脚与几何尺寸只在这里定义一次。换板、换屏只改这个文件，
// 其他文件不出现裸数字。
//
// 来源：HARDWARE.md §11（官方资料离线归档核实，非本机实测）。
// 串口日志只用 ASCII，这条是仓库约定。

#pragma once

#include <stdint.h>

namespace cfg {

// ---- 显示屏（ST7305，4.2 寸全反射 RLCD）----
// 物理 300x400，实际按 400x300 横屏驱动。u8g2 的 ST7305 驱动是竖屏定义，
// 必须显式传 U8G2_R1 才交换成长边在 X 轴（详见 docs/adr/0001）。
constexpr int kLcdSck = 11;
constexpr int kLcdMosi = 12;
constexpr int kLcdDc = 5;
constexpr int kLcdCs = 40;
constexpr int kLcdRst = 41;
constexpr int kLcdTe = 6;  // 撕裂同步，本项目不用

constexpr int kLcdWidth = 400;   // 逻辑宽（横屏）
constexpr int kLcdHeight = 300;  // 逻辑高（横屏）

// u8g2 的 tile 几何：每 tile 8x8 像素
constexpr int kTileWidth = 38;   // 38*8 = 304 列（>300，末尾补空）
constexpr int kTileHeight = 50;  // 50*8 = 400 行
constexpr int kBufferRowBytes = kTileWidth * 8;  // 304
constexpr int kBufferBytes = kBufferRowBytes * kTileHeight;  // 15200

// SPI 时钟。官方例程用 24 MHz；GPIO 矩阵路径上限约 40 MHz，
// ST7305 写时序要求 >= 30 ns（约 33 MHz），24 MHz 留足余量。
constexpr uint32_t kSpiClockHz = 24000000;

// ---- 按键 ----
constexpr int kKeyBoot = 0;   // 低有效，上电 1 秒内的事件要丢弃（进下载模式）
constexpr int kKeyUser = 18;  // 低有效，内部上拉

// ---- I2C 总线（一条总线挂四颗从设备，地址互不冲突）----
// 注意：与仓库里旧 OLED 工程的 SDA=5 / SCL=4 完全不同 —— 那两颗脚在这块板上
// 是屏幕的 DC 与 TE。详见 HARDWARE.md §11.6。
constexpr int kI2cSda = 13;
constexpr int kI2cScl = 14;
constexpr uint32_t kI2cClockHz = 400000;

constexpr uint8_t kAddrShtc3 = 0x70;   // 温湿度
constexpr uint8_t kAddrRtc = 0x51;     // PCF85063 时钟（M2 第 5 步才用）
constexpr uint8_t kAddrEs8311 = 0x18;  // 音频 codec（v0.1.5 起用于提示音，ADR-0022）
constexpr uint8_t kAddrEs7210 = 0x40;  // 音频 ADC（仍不用：录音仍延期，ADR-0011）

// ---- I2S 与功放（v0.1.5，docs/adr/0022）----
// 与 ES8311 同一组 I2S。ES8311 与 ES7210 共用这组脚，本次只用输出方向。
// 引脚来自官方 board_cfg.txt 的 S3_RLCD_4_2 块，与 HARDWARE.md §11.8 逐项一致。
constexpr int kI2sMclk = 16;
constexpr int kI2sBclk = 9;
constexpr int kI2sWs = 45;
constexpr int kI2sDin = 10;   // 录音方向，本次不用
constexpr int kI2sDout = 8;   // 播放方向
// NS4150B 功放使能。高有效；但**不要自己 toggle** —— esp_codec_dev 的
// disable_when_closed 默认 true，open/close 时驱动会成对开关它（ADR-0022 事实盘点）。
constexpr int kPaCtrl = 46;

// ---- SHTC3 温度补偿 ----
// 官方 BSP 的换算式是 175*raw/65536 - 45 - 4，比数据手册多减 4 °C
// （i2c_equipment.cpp:98，常量 SHTC3_PETP_VOL = 4）。这不是手册要求，
// 是官方例程的取向：先照抄以便对齐官方读数，同时把未补偿值一起打出来。
// 与手持表对不上时，先看这两个数差多少（docs/策划案.md §7 第 9 项）。
constexpr float kShtc3TempOffsetC = -4.0f;

// ---- 采样周期 ----
constexpr uint32_t kSensorPollMs = 60000;  // 温湿度 60 秒一次

// ---- 自发热诊断（做完就删）----
// 开机后先停掉一切重绘，静置 kThermalProbeMs，每 10 秒记录一次板温与芯片温度。
// 目的是把「读数 35 °C」拆成两个来源：SoC 自身热 + 我们自己刷新造成的热。
// 静置期间温度还在涨 -> 是芯片持续发热的余温；掉下来 -> 是重绘贡献的。
constexpr bool kThermalProbe = false;
constexpr uint32_t kThermalProbeMs = 180000;
constexpr uint32_t kThermalProbePeriodMs = 10000;

// ---- 电池采样 ----
// VBAT -- R21 200K --+-- R23 100K -- GND，采样点接 GPIO4（ADC1_CH3）。
// 分压比 100/(200+100) = 1/3，所以还原时乘 3。
constexpr int kBatAdc = 4;
constexpr float kBatteryDivider = 3.0f;
constexpr uint32_t kBatteryPollMs = 60000;

// 电压 -> 电量百分比：四点三段折线（v0.1.2 起，见 ADR-0018）。
//
// 官方例程在 3.00–4.12 V 之间画一条直线。这条直线在屏幕上会把一次真实放电
// 画成「先猛掉、中段几乎不动、最后又猛掉」——因为锂电中段电压几乎是平的，
// 而电量还在掉。折线把这件事倒过来：中段每伏对应更多百分点，底部每伏对应更少。
//
// 四个锚点**没有一个是实测的**，全是工程近似。真要做准只有一条路：对本板做
// 一次完整放电记录（ADR-0018 记了这条路）。锚点少的好处是替换时清楚该换哪几个数。
constexpr float kSocKnotV[] = {3.00f, 3.60f, 3.90f, 4.12f};
constexpr int kSocKnotPct[] = {0, 25, 70, 100};
constexpr int kSocKnotCount = static_cast<int>(sizeof(kSocKnotV) / sizeof(kSocKnotV[0]));

// 显示死区（v0.1.2）：|新读数 - 当前显示值| 达到阈值才更新屏幕上的数字。
//
// 阈值按百分比定，不按毫伏定 —— 换成折线之后，同样是 2%：在中段只值约 13 mV
// （3.90–3.60 V 段，45 个百分点挤在 300 mV 里），到底部却值约 48 mV
// （3.60–3.00 V 段，25 个百分点摊在 600 mV 上）。按毫伏设阈值，两个区间的
// 实际效果会差近四倍。
// 低电量段收紧到 1%：那时候「还能撑多久」比「数字稳不稳」重要，晚一拍才显示
// 5% 会让人以为还够用，然后设备熄了。
constexpr int kBatDeadbandPct = 2;
constexpr int kBatDeadbandLowPct = 1;
constexpr int kBatDeadbandLowMax = 10;  // 当前显示值 <= 此值时改用低电量死区

// ---- WiFi ----
// 凭据写在 gitignored 的 src/secrets.h，不在这里。
constexpr uint32_t kWifiTickMs = 100;              // 状态机轮询间隔
constexpr uint32_t kWifiAttemptTimeoutMs = 15000;  // 单次连接尝试多久算超时
constexpr uint32_t kWifiBackoffMinMs = 5000;       // 第一次重试等 5 s
constexpr uint32_t kWifiBackoffMaxMs = 60000;      // 退避上限 60 s（5/10/20/40/60）

// bringup 专用：非 0 时在开机 N 毫秒处主动断开一次，用来验证
// Trying -> Waiting -> Trying 这条重连链。正常固件保持 0。
// 为什么要留这个开关：拔路由器是最真实的验证，但不可能每次都去拔；
// 软件断开走的是同一段状态机代码，能把「掉线后被重连逻辑捞回来」这条路径跑通。
constexpr uint32_t kWifiDropTestMs = 0;

// ---- 时间 ----
// 时区名取自 src/tz.cpp 的内置表（newlib 没有 zoneinfo，见该文件头注释）。
// 名字不在表里会回落到 UTC 并置 invalid，不会静默当成 UTC。
constexpr const char *kTimeZoneName = "Asia/Shanghai";

// NTP 只负责给 UTC，本地化由 tz 表算，所以传 0 偏移。
constexpr const char *kNtpServer1 = "ntp.aliyun.com";
constexpr const char *kNtpServer2 = "ntp1.aliyun.com";
constexpr const char *kNtpServer3 = "cn.pool.ntp.org";

// 校时周期：开机一次 + 之后每 24 小时一次。
// 不是「只跑一次就再也不管」—— RTC 的日累积误差会一直涨。
constexpr uint32_t kResyncIntervalMs = 24u * 60u * 60u * 1000u;
constexpr uint32_t kNtpWaitTimeoutMs = 20000;      // 发起 SNTP 后最多等这么久
constexpr int64_t kTimeSanityEpoch = 1700000000;   // 2023-11-15，低于它说明系统时间还没设

// ---- 服务器快照 ----
// 契约冻结在 status-oled3/docs/adr/0012；本项目议定扩到 22 字段（加 svc_select /
// svc_wb / svc_ma12，见 ADR-0014）。设备端按字段名取值，不建偏移表。
// 真实主机名是**资产**，不该跟着代码进公开仓库：六个探针里有一个是第三方主机
// 加非标端口，写进来等于替别人广播「这个地址开放 11948」。所以这里只留占位符，
// 真值放 gitignored 的 src/secrets.h。
// 没有 secrets.h 也能编译（走占位符）—— clone 下来想先看屏幕点亮，不该被凭据卡住。
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef RLCD_DATA_HOST
#define RLCD_DATA_HOST "example.com"
#endif
#ifndef RLCD_PROBE_WEB
#define RLCD_PROBE_WEB "www.example.com"
#endif
#ifndef RLCD_PROBE_SELECT
#define RLCD_PROBE_SELECT "select.example.com"
#endif
#ifndef RLCD_PROBE_WB
#define RLCD_PROBE_WB "wb.example.com"
#endif
#ifndef RLCD_PROBE_MA12
#define RLCD_PROBE_MA12 "your-tcp-host.example:0000"
#endif

constexpr const char *kDataHost = RLCD_DATA_HOST;
constexpr const char *kSnapshotPath = "/status/snapshot.json";
constexpr const char *kMonitorKeyHeader = "X-Monitor-Key";

constexpr uint32_t kSnapshotPollMs = 15000;   // 轮询间隔
constexpr uint32_t kSnapshotStaleMs = 60000;  // 超过这么久没更新就算「停更」
// 失败退避：15 -> 30 -> 60 -> 120 s 封顶。服务器挂了的时候不该每 15 秒去敲
// 一次门 —— 既费电，又把日志刷成一片「fail」。
constexpr uint32_t kSnapshotBackoffMaxMs = 120000;
constexpr uint32_t kHttpTimeoutMs = 8000;
constexpr uint32_t kHttpResponseMax = 1024;   // 与 status-oled3 一致；超了就是契约坏了

// ---- 待办（M8，ADR-0020 取代 ADR-0006）----
// 数据源换成网站的设备摘要端点：只读、服务端已经把「只取未完成 + 到期窗口
// [本周一−14 天, 本周日] + 逾期置顶排序 + 上限 6 条」算好，设备只负责画。
//
// 5 分钟一次：服务端 SQLite 是单连接（SetMaxOpenConns(1)），30 秒一次只会
// 排在自己后面；文档的 30 秒是按插电常驻牌写的，这块板用电池。
// 停更 30 分钟：5 分钟一次的话，错过一轮就报停更等于把「偶尔抖一下」变成
// 屏上的事故；6 轮才算真停了。
constexpr const char *kTodoPath = "/api/todo-device";
constexpr const char *kTodoKeyHeader = "X-Todo-Device-Key";
constexpr uint32_t kTodoPollMs = 300000;
constexpr uint32_t kTodoStaleMs = 1800000;
// 失败退避 30 s 起、300 s 封顶：失败时反而比正常周期更密，是为了「网络刚恢复
// 就尽快捞回来」——正常的 5 分钟周期会让恢复晚一个周期。
constexpr uint32_t kTodoBackoffMinMs = 30000;
constexpr uint32_t kTodoBackoffMaxMs = 300000;
// 认证/部署类错误（401/403/404/405）判「人工得管」，但不停止轮询：
// 服务端把 key 配好、或把端点部署上去之后，设备应该自己恢复，而不是等重烧。
constexpr uint32_t kTodoSlowProbeMs = 1800000;
constexpr uint32_t kTodoConnectTimeoutMs = 3000;
// 2048：最坏响应约 1.27 KB（6 条中文标题 + 行程块），1024 会截断成非法 JSON，
// 看起来像服务端坏了。与快照的 1024 分家，那条路径的契约没变。
constexpr uint32_t kTodoResponseMax = 2048;

// ---- 时序 ----
constexpr uint32_t kBootKeyIgnoreMs = 1000;  // 上电后丢弃 BOOT 事件的窗口
constexpr uint32_t kHpmSettleMs = 100;       // 切 HPM 后等画面稳定再写（自定值，待实测校正）

// ---- 空闲省电 ----
// 无按键、无数据变化超过这个时长就切 LPM（0x39），有交互立刻切回 HPM（0x38）。
constexpr uint32_t kIdleToLpmMs = 30000;  // docs/adr/0005
// 对比度 5 档。两条路径改同一个值、写同一份 NVS：面板上 BOOT 长按的热键
// 与关于页设置版面里的「BOOT 长按 = 切换选中行选项」（docs/adr/0017）。
// 编码见 RlcdDisplay::setContrast。
constexpr uint8_t kContrastLevels[] = {40, 50, 60, 70, 80};
constexpr uint8_t kContrastCount = sizeof(kContrastLevels) / sizeof(kContrastLevels[0]);
constexpr uint8_t kContrastDefaultIdx = 2;  // 第 3 档 = 60，M0 实测可见变化
constexpr uint16_t kContrastHintMs = 2000;  // 角落「对比度 3/5」显示时长

// ---- 声音（v0.1.5，docs/adr/0022）----
// 本板没有蜂鸣器（README「这块板值得知道的几件事」）。出声只有一条路：
// ES8311 -> NS4150B（CTRL=GPIO46）-> MX1.25 外接喇叭。
//
// 三项设置都是两档，编码统一为 0 = 开、1 = 关。默认全部为「开」——
// 这是用户 2026-10-27 的决定，代价是一上电就进入「每小时响一声」，
// 由夜间静音窗兜底，不至于半夜开机就响。
constexpr uint8_t kSoundCount = 2;
constexpr uint8_t kSwitchOn = 0;
constexpr uint8_t kSwitchOff = 1;

// 夜间静音窗：开工时刻与收工时刻（本地小时）。
// 这是**全系统唯一的时段判断点** —— 整点报时那一项的代码里不存在任何
// 小时数常量（ADR-0022 决策 §0）。加新的声音类型时自动继承这个窗。
constexpr uint8_t kQuietStartHour = 21;
constexpr uint8_t kQuietEndHour = 8;

// 提示音参数。两声「嘀嘀」共 340 ms：150 ms 响 + 40 ms 静 + 150 ms 响。
// 采样率 16 kHz 与官方 BSP 一致（24 kHz 是小智协议的需求，本次不涉及）。
//
// ⚠️ **为什么是 150 ms 而不是 60 ms —— m01063 上板实测的教训**：
// 原来两声各 60 ms（全长 150 ms），在设置版面里连按五次长按切档，
// **只有第一声听得见，后四声全哑**。而同样五次 playChime() 放在 setup()
// 里连播却全响，串口每次都是 say=1（esp_codec_dev_write 返回成功）。
// 把试听换成 300 ms 长音后连响五次全响 —— 结论是**短音被吃掉了**，不是链路坏。
//
// 机理：每次出声都要 open → write → delay → close 一整趟。150 ms 的数据在
// 16 kHz 双声道 16 bit 下只有 4800 B，而 I2S DMA 缓冲 + kPaSettleMs 建立的
// 固定开销吃掉了开头一段；写入被排到 close 之后才真正出到功放，
// 而 close() 会立刻断功放（disable_when_closed）。长音长到覆盖掉固定开销
// 就听得见，短音就被整段吞没。
//
// 150 ms 是权衡结果：够长到稳定出声，又短到不像闹钟。
constexpr uint32_t kChimeSampleRate = 16000;
constexpr uint32_t kChimeToneMs = 150;
constexpr uint32_t kChimeGapMs = 40;
constexpr uint32_t kChimeFlushMs = 60;    // 关流前等数据放完，见 closeStream()
constexpr uint16_t kChimeHz = 2700;      // 8 Ω 2 W 喇叭上足够响，且不与语音重叠
constexpr uint8_t kChimeAmplitude = 60;   // 占满量程的百分比，留余量防削顶

// 输出音量五档（v0.1.5 后续，docs/adr/0023）。
//
// 量纲是 esp_codec_dev 的 0..100，内部换算成 ES8311 的 DAC_REG32 档位。
//
// **默认档 85 不是随便挑的**：v0.1.5 上板时 85 是唯一被实测证明「听得见」的
// 值（自检长鸣与整点报时探针都响在它上面）。默认落在这一档，意味着老设备
// 升级后音量不会莫名变响或变哑；其余四档从它往下排，最上面留 95 让用户能往上试。
// 小声端只到 40 —— 宁可最小声不够小，也不要「调到最大还是听不清」。
//
// 写值时机：**每次 esp_codec_dev_open() 之后都必须写一遍**，这不是可选的。
// es8311.c:243 的 suspend() 会把 DAC_REG32 清成 0x00（约 -95.5 dB），而
// esp_codec_dev_open() 每次都先 suspend 再 start、start 不恢复它。
// 不显式设音量 = 每次开流都是静音，且 write() 依然返回成功。
constexpr int kVolumeLevels[] = {40, 60, 75, 85, 95};
constexpr uint8_t kVolumeCount = sizeof(kVolumeLevels) / sizeof(kVolumeLevels[0]);
constexpr uint8_t kVolumeDefaultIdx = 3;  // = 85，见上

// bringup 自检：非 false 时开机先长鸣 1 秒。
// 存在理由与 kThermalProbe 相同 —— 声音链路不走官方 codec_board，
// 拼错 if 层结构的症状是「无声」而不是「报错」，需要一个确定的发声源来证伪。
//
// v0.1.5 上板已通过：第一版写这套 codec 配置时「一切正常却不出声」，
// 根因是自己多加的 mclk_div=256 / no_dac_ref=true 与漏设 hw_gain，
// 对齐官方 codec_init.c 的字段集后出声。留这个开关方便下次回归。
constexpr bool kSoundSelfTest = false;
constexpr uint32_t kSoundSelfTestMs = 1000;

// ---- 设置注册表（v0.1.1，docs/adr/0017）----
// Preferences 的 namespace 名。只放「一项一个 key」的小配置，不放大数据。
constexpr const char *kSettingsNamespace = "rlcd";
// 生效钩子：settings.cpp 不认显示层，由 main.cpp 在 setup() 里挂上
// RlcdDisplay::setContrast。nullptr 时设置模块只改值不生效（自检可跑）。
// 放在 namespace 外面：函数指针要能在 main.cpp 里被取地址赋值，
// 而成员钩子会让 settings.cpp 反向依赖显示层。
// 只声明不定义 —— 定义在 settings.cpp。放定义会撞
// 「multiple definition of cfg_g_lcd_setContrast」：每个 include config.h 的
// 翻译单元都会各自拿到一份。
}  // namespace cfg

extern void (*cfg_g_lcd_setContrast)(uint8_t);

// ---- 声音三项的生效钩子（v0.1.5，docs/adr/0022 决策 §4）----
//
// 与 cfg_g_lcd_setContrast 同手法、反方向：对比度是「设置层推给显示层」，
// 声音是「设置层记状态，主循环来取」。三个 setter 只写模块内的静态标志，
// 不出声；真正的响由 main.cpp 在 loop 里调用 sound::playChime()。
//
// 为什么不直接在 apply 里调 sound::playChime()：
//   settings 这一层刻意不认识显示层，也不该认识音频层（settings.cpp 头注释）。
//   而且 loadAll() 会逐项 apply —— 上电时叫一声不是用户按的，是固件自己叫的。
//
// 定义在 settings.cpp（与 cfg_g_lcd_setContrast 同理，放头文件会 multiple definition）。
void cfg_g_sound_setMaster(bool on);
void cfg_g_sound_setChime(bool on);
void cfg_g_sound_setQuiet(bool on);

// 音量是唯一一个「有值」而不是「开关」的声音项（docs/adr/0023）。
// 同样只记状态不出声：切档时的试听由 main.cpp 的 previewSoundSetting() 驱动。
void cfg_g_sound_setVolume(uint8_t idx);

// 供 main.cpp 读取的判定树入参（ADR-0022 决策 §0 的四层判定要用的前三项）。
// 第四层「到点了」由 main.cpp 自己从 workclock 取。
bool cfg_g_sound_master();
bool cfg_g_sound_chime();
bool cfg_g_sound_quiet();

// 当前音量档位索引（0..kVolumeCount-1）。sound.cpp 每次 open 之后按它取
// kVolumeLevels[idx] 写进 DAC。越界时 settings 层已回落到默认档，
// 这里再兜一层，宁可响错响度也不能开流时写一个野值。
uint8_t cfg_g_sound_volume();

namespace cfg {

// ---- 界面几何（全部来自 docs/策划案.md §4.2 / §4.3 / §4.4 的实测字宽）----
constexpr int kHeaderH = 24;
constexpr int kFooterH = 22;
constexpr int kContentTop = kHeaderH + 1;                 // 25（页首下面一条分隔线）
constexpr int kContentBottom = kLcdHeight - kFooterH - 1;  // 277
constexpr int kSplitX = 200;                               // 两栏等宽，各 200 px
constexpr int kMarginL = 4;
constexpr int kMarginR = kLcdWidth - 4;  // 396

// ---- 日历（三行滚动窗口，docs/adr/0016）----
constexpr int kCalTop = 49;        // 第一行格子顶
constexpr int kCalRowH = 20;       // 格高
constexpr int kCalRowPitch = 21;   // 行距
constexpr int kCalCellW = 26;      // 格宽
constexpr int kCalColPitch = 27;   // 列距
constexpr int kCalRows = 3;
constexpr int kCalLeft = 204;      // 右栏 x

// ---- 待办（6 槽位）----
constexpr int kTodoSlots = 6;
constexpr int kTodoTop = 126;
constexpr int kTodoPitch = 25;

}  // namespace cfg

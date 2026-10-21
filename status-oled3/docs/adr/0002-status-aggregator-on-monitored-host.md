# 2. 聚合端点跑在被监控的服务器上，设备侧零凭据

日期：2026-10-15

## 状态

已接受

## 背景

服务器状态与邮箱状态都在这块表的外面。ESP32-S3 有 WiFi，但"谁去取数据"有两条根本不同的路：**设备自己直取**（阿里云 OpenAPI + IMAP），或**由服务器侧的一个端点先聚合成一个 JSON、设备只轮询它**。

用户已明确两点约束：**不使用阿里云的 API**（因此免掉 AccessKey 与请求签名），以及**设备直连服务器获取服务器信息**。

## 考虑的方案

### 方案 A：服务器本机采集 + 单一聚合端点，设备 HTTPS 轮询 —— 采纳

被监控的那台阿里云主机上跑一个端点（下称**聚合端点**），做两件事：

1. 读**本机**指标：CPU 占用、内存、磁盘、网络流量、负载、以及服务探活（网站 / SSH 是否可达）
2. 逐个邮箱查**未读数**（IMAP 或各邮箱服务商提供的轻量接口）

输出一个 JSON。ESP32 每 N 秒 GET 一次，`status-oled3` 侧**不保存任何服务端凭据**（AccessKey、IMAP 授权码、API Token 一概不进固件）。

> 「零凭据」的边界要说清：**WiFi 自身的 SSID/密码是例外**。那是连网的前提，不是服务端凭据，沿用 `clock-oled/src/secrets.h`（仓库根 `.gitignore` 已覆盖）的既有先例。本决策排除的是「为了拿服务器/邮箱数据而需要的凭据」，不是「为了上网而需要的凭据」。

**采纳理由**：

1. **免掉阿里云 OpenAPI 的整套机制**：CloudMonitor 需要 AccessKeyId/AccessKeySecret、HMAC-SHA1 签名、按实例 ID 查指标——对一个只显示四个数字的屏幕是严重过度设计，且密钥要烧进 ESP32 flash，泄露面从一台服务器扩大到一块随身设备
2. **"服务器自身状态"用本机 `/proc` 读最准最快**，不受云 API 的采集粒度（通常 1 分钟）与配额限制；本机脚本读 `/proc/stat`、`/proc/meminfo`、`df` 是零依赖的
3. **邮箱凭据只存在于服务器一处**：用户现有后端已有 SMTP 凭据管理先例——`W:\Github\Siungo-Website\Code\server` 的告警通道，凭据落在 `DataDir/email.json`（`authUser` / `authPassword`，原子写 0600，读取时掩码为 `********`）。集中在一处比分散到固件里更安全，轮换授权码也不用重新烧录。**新增的 IMAP 授权码不得沿用 `.doc/deploy-secrets.txt` 那种明文模式**（该文件已在盘上存有 `MONITOR_KEY` 与 `ADMIN_PASSWORD`），只走 0600 配置文件，见 [ADR-0008](0008-server-side-snapshot-via-cron.md)
4. **设备侧实现量最小**：一个 `HTTPClient` + JSON 解析，没有 TLS 双向认证、没有签名、没有 OAuth 刷新
5. **"服务器挂了"本身变成可观测事件**：数据停更即掉线，屏上显示最后更新时间（见 [ADR-0004](0004-fixed-server-polling-and-staleness.md)），这比任何探针都更贴近用户真实关心的那句话——"我的服务器还活着吗"

### 方案 B：设备直连阿里云 CloudMonitor API

**否决理由**：用户明确排除；且需要 AccessKey + HMAC 签名 + 实例 ID 映射三件事，ESP32 侧要实现签名算法并保管长期密钥，与"屏幕只显示四个数字"的信息量严重不匹配。

### 方案 C：第三方 uptime 探针（UptimeRobot 等）兜底

**不否决，但仅作可选补充**：外网探针能回答"从公网看得到吗"，却给不了 CPU/内存/磁盘，且要引入第二个外部依赖与它的 API key。主通道用方案 A；若将来需要"从外网验证"，再评估是否加一个探针页面。

### 方案 D：设备直连 IMAP 拉取各邮箱

**否决理由**：每个邮箱的授权码都要写进固件（进 git 风险、进 flash 泄露面）；TLS 会话的内存代价叠加在已有的 WiFi 栈之上（`HARDWARE.md` §9 实测 WiFi 栈已占 RAM 14.1% / Flash 21.5%）；IMAP 的 IDLE/NOOP 保活逻辑在 MCU 上写起来远比轮询一个 JSON 麻烦。

## 决策

**服务器侧采集、设备侧只轮询一个 JSON 端点。** 聚合端点读取本机指标与各邮箱未读数，ESP32 通过 HTTPS 定时 GET；设备不持有任何服务端凭据。

聚合端点的**部署形态**最初于 [ADR-0005](0005-aggregator-as-standalone-service-with-shared-secret.md) 决定为「独立小服务 + 独立 systemd unit + nginx 反代」，**该方案已于 2026-10-16 被用户否决**。当前形态见 [ADR-0008](0008-server-side-snapshot-via-cron.md)：cron 每分钟生成静态 JSON + nginx 直接伺服，零常驻进程、零编译。设备侧先接主站现成的 `/api/health` 打通链路（[ADR-0009](0009-phase-in-with-existing-health-endpoint.md)）。

## 后果

### 正面

- 设备侧零凭据、零签名、零 OAuth
- 邮箱授权码集中在服务器一处，轮换不需要重新烧录固件
- 指标来源是本机 `/proc`，精度与实时性优于云 API 的 1 分钟粒度

### 负面 / 需承担的成本

1. **多了一个必须维护的服务端组件**：端点本身挂掉或语法错误，三块屏里有两块变哑巴
2. **服务器真宕机时，邮箱信息一起消失**——因为邮箱状态也由这台服务器代查。这是本方案的结构性代价：**邮箱数据与服务器数据共命运**。缓解办法只有把邮箱查询挪到另一台常驻机（回到"聚合器放别处"的路线），当前不接受这个复杂度
3. **停更判定成为必需功能**：必须显示"最后更新时间"，且要定义多久不更新算掉线（见 [ADR-0004](0004-fixed-server-polling-and-staleness.md)）
4. **设备需要知道服务器地址**：域名写进固件（见 [ADR-0006](0006-aggregator-url-pinning.md)），换服务器要改固件

## 备注

- 本决策与 `pet-oled3` 的 [ADR-0004](../../../pet-oled3/docs/adr/0004-uptime-only-time-progression.md)（不联网）不冲突：那条决策的适用范围是掌上宠物单机玩法，其备注已声明"仅对 pet-oled3 生效"。本项目需要外网数据，按 [`../../../HARDWARE.md`](../../../HARDWARE.md) §9 的实测代价重新评估即可
- 若未来要把聚合端点挪去第二台机器，本决策的方向不变，只换设备轮询的地址（[ADR-0006](0006-aggregator-url-pinning.md) 的一个常量）——这是刻意保留的退路
- **"设备不持有任何服务端凭据"**的准确边界：不持有**服务端侧**的凭据（IMAP 授权码、阿里云 AccessKey、数据库口令等）。WiFi 自身的 SSID/密码与轮询用的共享密钥（`X-Monitor-Key`，M1b 起）是设备侧凭据，先例见 `clock-oled/src/secrets.h`；共享密钥的分发与存放见 [ADR-0008](0008-server-side-snapshot-via-cron.md) 与 [ADR-0006](0006-aggregator-url-pinning.md)
- 停更判定用设备本地单调时钟而非服务器 `timestamp`，见 [ADR-0004](0004-fixed-server-polling-and-staleness.md) 的修订
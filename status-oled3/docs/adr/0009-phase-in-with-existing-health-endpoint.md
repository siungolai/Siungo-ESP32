# 9. M1 第一步先接主站现成的 GET /api/health

日期：2026-10-16

## 状态

已接受

## 背景

[ADR-0008](0008-server-side-snapshot-via-cron.md) 定下了服务器侧最终形态（cron + 静态 JSON），但那条路要写脚本、加 nginx location、配 cron，一次性到位的话，设备侧固件要等到服务器全部就绪才能开始验证。

需要回答的问题：**设备侧的 HTTPS 链路、JSON 解析、停更判定、蜂鸣器提示，能不能在服务器零改动的前提下先跑通**。

已查明的事实：

- 主站 `GET /api/health` **公开无认证**（`W:\Github\Siungo-Website\Code\server\routes.go`，README.md:159 列为公开端点），nginx 的 `/api/` 已在反代到 `127.0.0.1:8080`，设备请求它**不需要任何服务器侧改动**。
- 响应契约（`W:\Github\Siungo-Website\Code\server\handler\health.go:36-50`）：`HealthStatus` 的 json tag 为 `status` / `service` / `uptime` / `version` / `commit` / `built` / `siteName` / `siteUrl` / `timestamp`。`Status` 恒为 `"ok"`；`Uptime` 是进程级 `time.Since(startTime)`；`Commit` / `Built` 由 `build.sh -ldflags -X` 注入，未注入时是 `"dev"` / `"unknown"`（不是空串）。
- 它**不含** CPU / 内存 / 磁盘 / 负载任何字段。先接它只能得到「服务活着 + 运行时长 + 版本身份 + 服务器时钟」。

## 考虑的方案

### 方案 A：先接 `/api/health`，再切到 `/status/snapshot.json` —— 采纳

设备侧代码先对着 `/api/health` 写，跑通后把 URL 常量改成 `/status/snapshot.json`，其余逻辑（HTTPS、JSON 取值、停更判定、提示音）完全不动。

**采纳理由**：

1. **服务器侧零改动即可验证设备侧全部关键路径**：HTTPS 握手、根证书校验、HTTP 状态码处理、JSON 取值、15s 节奏、60s 停更判定、WiFi 断开重连、蜂鸣器三种触发——这些恰恰是设备侧风险最集中的部分，且与数据内容无关。
2. **两种方案下设备侧代码相同**：这是本决策的核心论据。设备侧只做「HTTPS GET + 按 key 取值 + 记 `lastSuccessMs`」，它拿到的 JSON 有几个字段、字段叫什么，不影响这三步的结构。先接哪个端点是**配置**不是**架构**。
3. **`/api/health` 本身就是服务探活的答案**：设备侧第一版就能显示「主站后端活着 + 运行了多久 + 哪个 commit」，这对桌面状态显示器是有意义的真实信息，不是假数据。
4. **失败可定位**：如果连一个已在生产跑了数月的公开端点都拿不到，问题必然在设备侧（TLS / DNS / WiFi），不必怀疑服务器。

### 方案 B：等服务器侧 cron + nginx 全部就绪再动设备侧

**否决理由**：把两条独立的验证路径串成了一条。服务器侧要写脚本、改 nginx、配 cron、验证告警链路；设备侧要调 TLS、内存、节奏。任何一边卡住另一边就完全停摆，而它们本可以并行。

### 方案 C：设备侧先用 `http://` 明文 + IP 避开 TLS

**否决理由**：ADR-0006 已否决「IP + 端口 + `setInsecure()`」，明文更退一步——而且「服务器是否活着」这个答案会因为中间人插入而失真，与停更判定的目的直接矛盾。

## 决策

M1 分两步走，设备侧固件只有**一个常量**的差别：

| 阶段 | 端点 | 服务器侧改动 | 设备侧显示 |
|---|---|---|---|
| M1a | `https://www.siungo.top/api/health` | 无 | ESP32 屏：RSSI / IP / 堆内存；服务器屏：`up`（uptime）/ `commit` / 停更态；邮箱屏：`M1 pending` |
| M1b | `https://www.siungo.top/status/snapshot.json` | cron 脚本 + nginx location + crontab | 服务器屏 2 页（CPU+内存+负载 / 磁盘+网络+服务探活）；邮箱屏：未读总数 |

- 两个 URL 都在 `src/config.h`，切换只改常量；`X-Monitor-Key` 请求头只在 M1b 需要（`/api/health` 公开，带头无害但没必要），所以请求头也做成配置项。
- M1a 阶段**不引入 NTP**：停更判定用设备本地 `millis()` 单调时钟，见 [ADR-0004](0004-fixed-server-polling-and-staleness.md)。`health.json` 里的 `timestamp` 只在串口打印，用于发现服务器时钟偏差。

## 后果

### 正面

- 设备侧的全部网络风险可以在**今天**就开始收敛，不必等服务器脚本。
- `/api/health` 已在生产跑了数月，作为第一个端点比自建端点更可靠。
- M1a 到 M1b 的切换是一次常量修改，不需要重构。

### 负面 / 需承担的成本

1. **M1a 的服务器屏信息量有限**：只有 uptime 与 commit，没有 CPU / 内存 / 磁盘。这段时间里服务器屏的价值低于最终形态——但它显示的是真实数据，不是占位符。
2. **`Status` 恒为 `"ok"`**：该字段在进程活着时永远是 `ok`，因此它**不能**作为「服务器健康」的判据。设备侧判「服务器活着」的依据只能是「HTTP 200 + JSON 可解析」，这一点必须在代码注释里钉住，否则后人会误读该字段。
3. **服务器进程活着 ≠ 网站可用**：`/api/health` 只证明 Go 进程在跑。nginx 挂掉时设备照样停更（连不上），但 nginx 挂而 Go 活着时设备显示正常——这个盲区由服务器侧的 `snapshot.json` 里的服务探活字段（M1b）补上。

## 备注

- `/api/health` 的响应是 8 个固定 tag 的扁平 JSON，这直接支撑了 [ADR-0010](0010-hand-written-json-value-extractor.md) 的「不引 ArduinoJson」决策。
- 本 ADR 只覆盖设备侧与端点选择，不改 [ADR-0002](0002-status-aggregator-on-monitored-host.md) 的「设备侧不持有服务端凭据」结论——`/api/health` 无认证，M1b 的 `X-Monitor-Key` 属共享密钥而非服务端凭据（见 ADR-0002 的边界说明）。

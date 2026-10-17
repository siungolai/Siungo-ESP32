# 5. 聚合端点做成独立小服务 + 独立 systemd unit + nginx 反代

日期：2026-10-15

## 状态

**已被用户否决，由 [ADR-0008](0008-server-side-snapshot-via-cron.md) 取代**（否决记录见下文「背景」末段与「备注」）

> 本文件保留原文作为决策历史，不代表当前部署形态。当前形态见 ADR-0008：cron 每分钟生成静态 JSON + nginx 直接伺服，零常驻进程、零编译。

## 背景

[ADR-0002](0002-status-aggregator-on-monitored-host.md) 定下了「服务器侧采集、设备侧轮询」，但把**部署形态**列为待决项：并入 `W:\Github\Siungo-Website\Code\server` 现有 Go 后端，还是单开一个服务。

本文件基于对那台机器的实查记录决定。已核实的关键事实：

1. **主站 Go 后端已经是一条独立服务**：systemd unit `upload-server`（User=www-data，WorkingDirectory=/var/www/photography，`ProtectSystem=strict`、`PrivateTmp=yes`、`NoNewPrivileges=yes`），监听 `127.0.0.1:8080`，由 nginx `/api/` 反代。unit 文件**不在仓库里**，只存在于服务器上
2. **「同一台机器上再跑一个 Go 服务」已有先例**：`token-cost` 是一个独立 Go 服务，监听 `127.0.0.1:8089`，源码在独立仓库 `W:\Github\siungo-token-cost`，其 `.gitignore` 明确写着不要把它加回主仓库
3. **主站后端自己有一套重装备的发布流水线**：`deploy.sh` 会 `git status --porcelain -- Code/server` 拒绝脏工作区、`git archive` 打包 scp 到 `/opt/siungo-build/photography`、服务器端先跑 PORT=18099 的临时实例做干跑探针（`/api/photos` 200 + health commit 匹配）才动生产、备份 `/root/backups/upload-server.<ts>.bak` 保留 5 份、`systemctl stop/start`
4. **主站后端依赖 cgo 与本地 webp patch**：`replace github.com/chai2010/webp => ./third_party/chai2010-webp`，`CGO_ENABLED=1`，**在 Windows 上无法交叉编译**（`webp.go:22:9: undefined: webpGetInfo`），只能服务器端 `build.sh` 编译
5. 主站后端的配置是**纯 `os.Getenv`、无 `.env` 加载**，密钥通过 systemd `Environment=` 注入；`MONITOR_KEY` 已有先例（`/api/settings/email/alert` 用它做 `X-Monitor-Key` 常量时间比对）

## 考虑的方案

### 方案 A：独立小服务，独立 unit，nginx 反代，共享密钥鉴权 —— 采纳

`status-aggregator` 作为一个小服务单独编译部署：自己的源码目录、自己的 systemd unit、自己的 nginx location，与主站后端零代码耦合。设备侧带一个共享密钥（见 [ADR-0002](0002-status-aggregator-on-monitored-host.md) 的零凭据边界）。

**采纳理由**：

1. **发布链路解耦，这是最重的一条**：并入主站后端意味着每次改屏幕上的一个字段都要走一遍「脏工作区检查 + git archive + scp + 干跑探针 + 停机重启」的完整流水线，且**在 Windows 上无法本地编译验证**（cgo webp），只能在服务器上 `build.sh` 盲改。独立服务的产物若选纯 Go 标准库（不引 cgo），Windows 上 `GOOS=linux GOARCH=amd64 go build` 直接出二进制，开发回路从「改完 scp 上去看日志」缩短为「本地 build + 服务器 dry-run」
2. **故障域隔离**：主站 unit 挂了不影响屏幕，聚合 unit 挂了不影响网站。主站后端每次发布要 `systemctl stop upload-server`，这段时间屏幕数据会停更——但那是预期行为，不是缺陷；反过来，聚合服务的发布**完全不会**让网站抖一下
3. **先例已验证可行**：`token-cost` 就是同一台机器上这么跑的，`token-cost` 源码甚至有意识地移出了主仓库。这台机器的运维者（用户）已经接受并维护这个模式
4. **权限可以更小**：聚合服务只需要读 `/proc`、`df`、以及 IMAP 外联，不需要碰 `/var/www/photography/data` 里的 SQLite 与图片目录。单独一个 unit 可以用 `ProtectSystem=strict` + 只读绑定，比塞进已有 unit 更窄
5. **密钥注入方式与现状一致**：`MONITOR_KEY` 已经通过 systemd `Environment=` 注入给 Go 进程，新服务照抄这个模式，不需要引入 `.env` 文件或配置文件解析

### 方案 B：并入主站 Go 后端，加一个 `/api/status` 路由

**否决理由**：

1. **发布代价被高估地继承**：如背景第 3、4 条，一次屏幕字段调整要触发整条重流水线，且无法在开发机预编译
2. **`ProtectSystem=strict` 的 unit 要为一个新功能放宽**（IMAP 出网、读 `/proc` 只是读，问题不大，但真正的问题是单元内的代码面变大，回归面从「屏幕」扩大到「网站」）
3. 主站后端有 CSRF 中间件**最外层包裹整个 mux**，且 `csrfExemptPaths` 被 `middleware/session_test.go:306-336` 锁死（未声明的路径会直接测试失败）。给设备端加一个不走 Cookie 的 GET 路由虽然可行，但每次动这个表都要改测试，属于无谓的耦合
4. **主站后端的 body/路由约束与本项目无关但会被迫继承**：1MB JSON 上限、`Protect` 固定位置派发、CORS allowlist 只放行 `siungo.top` 两个域名——这些对一个本地设备轮询端点全是噪音

### 方案 C：并入但走「同 unit 不同进程」的 sidecar

**否决理由**：systemd 不支持「一个 unit 两个 ExecStart 常驻」，要么 `Type=notify` 自己实现，要么拆 unit——拆了 unit 就回到方案 A，不如一开始就按独立服务设计。

## 决策

`status-aggregator` = **独立小服务**：

- 独立源码目录（不放 `Siungo-Website/Code/server`，参考 `token-cost` 独立仓库的先例）
- 独立 systemd unit（`status-aggregator.service`，User=www-data 或更窄，`ProtectSystem=strict`）
- 监听 `127.0.0.1` 上的一个新端口（避开 8080 / 8089 / 18099 干跑段），nginx 加一个 `location` 反代到它，走与 `/api/` 同级的 HTTPS
- 设备侧带共享密钥（`X-Monitor-Key` 形式，与 `/api/settings/email/alert` 的既有做法一致），服务端常量时间比对；**不复用 RemoteAddr 本地豁免**——`handler/email.go:264` 已经明确删掉了那个模式，因为 nginx 反代下 RemoteAddr 恒为 127.0.0.1
- **实现语言选纯 Go 标准库（`net/http` + 读 `/proc` + `crypto/tls` 的 IMAP），不引 cgo**，使 Windows 开发机能直接交叉编译

## 后果

### 正面

- 改屏幕字段 = 改一个独立小仓库 + 一次 dry-run，不碰网站发布链路
- 网站停机重启（`systemctl stop upload-server`）不再导致屏幕停更，两个故障域彻底分开
- unit 权限可以按最小需求收窄
- 与 `token-cost` 同构，运维心智成本低

### 负面 / 需承担的成本

1. **多一个 systemd unit 与 nginx location 要维护**，且这两个文件都不在仓库里（沿用该机器现状），换机器要凭记忆重建——缓解办法是把 unit 与 nginx 片段作为模板文件收进 `status-oled3` 仓库
2. **证书与域名仍是手动续期**：`/etc/nginx/ssl/siungo.crt` 是阿里云控制台手动导出的通配符证书（certbot 从未管理过它，`certbot certificates` 为空），2026-10-13 记录 notAfter 2026-12-22。聚合服务复用同一张证书，续期流程与主站绑定
3. **聚合服务自身没有外部探针**：这台机器上的监控全是「机器自己 cron 自己」，邮箱是唯一通知通道。聚合服务挂掉时，屏幕会停更——而屏幕正是最该知道这件事的东西，但它已经哑了。这是结构性盲区，接受
4. **密钥分发仍有一处泄露面**：`W:\Github\Siungo-Website\.doc\deploy-secrets.txt` 里明文存着 `MONITOR_KEY` 与 `ADMIN_PASSWORD`。新服务**不得**把密钥写进任何提交进仓库的文件，只走服务器侧 systemd `Environment=`（或等效的 0600 配置文件）+ 设备侧 `src/secrets.h`（不进 git，见 [ADR-0002](0002-status-aggregator-on-monitored-host.md) 与 [ADR-0006](0006-aggregator-url-pinning.md)）

## 备注

- **否决记录（2026-10-16）**：用户在讨论后续开发计划时明确否决本方案——原话「我没计划单独放go服务到阿里云上」。否决的实质理由是「常驻进程产分钟级数据 = 重武器打蚊子」：这台机器已有四个 cron + bash + python3 的数据生产任务（`photo-monitor.sh` 每 5 分钟、`sqlite-backup.sh` 每天 04:00、`photo-cleanup.sh` bash 嵌 python3 heredoc、`cert-expiry-check.sh` 每天 08:30），唯一的 Go 常驻服务 `siungo-token-cost` 是特例（需要 SQLite 与 `go:embed` 前端），不构成反证。替代形态见 [ADR-0008](0008-server-side-snapshot-via-cron.md)。
- 端口选择：8080（主站）、8089（token-cost）、18099（干跑段）已占用，本机监听端口取 **127.0.0.1:8090**，M1 部署时确认。设备侧的引脚口径另见 [ADR-0007](0007-gpio-assignment-buttons-and-buzzer.md)（**注意：GPIO 10/11/12 是初版规划，2026-10-16 实测接线为 GPIO 6 / 7 / 21，ADR-0007 已订正**）
- nginx 侧若只加 `location /api/status/` 而不动 server 级 `add_header`，要注意 **`add_header` 是替换语义不是合并**——新 location 若需要安全头，必须把 6 个头全部重写一遍，否则会静默丢掉 HSTS 等头（该教训对 ADR-0008 的 `location /status/` 同样适用）
- 端口与路径前缀（`127.0.0.1:8090` + `/status/`）是初版占位，M1 部署时与 nginx 配置一起定稿；定稿后只需同步 [ADR-0006](0006-aggregator-url-pinning.md) 的一个常量
- 本决策只定部署形态，**不定端点长什么样**（字段、刷新节奏、停更阈值），那些属于 [ADR-0004](0004-fixed-server-polling-and-staleness.md) 与后续实现规格

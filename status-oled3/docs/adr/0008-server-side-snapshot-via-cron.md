# 8. 服务器侧指标快照：cron + 静态 JSON + nginx 直接伺服

日期：2026-10-16

## 状态

已接受（**取代 [ADR-0005](0005-aggregator-as-standalone-service-with-shared-secret.md)**，该方案已被用户否决）

## 背景

[ADR-0002](0002-status-aggregator-on-monitored-host.md) 定下「服务器本机采集 + 单一聚合端点」，并把部署形态列为待决项；[ADR-0005](0005-aggregator-as-standalone-service-with-shared-secret.md) 当时选了「独立 Go 小服务 + 独立 systemd unit + nginx 反代 + 共享密钥」。

用户在讨论后续计划时否决了该形态：**「我没计划单独放go服务到阿里云上」**。否决的理据是这个机器上再跑一个常驻进程，只为产分钟级数据，代价与收益不成比例。

需要重新回答的问题只有一个：**CPU / 内存 / 磁盘 / 负载 / 服务探活 / 邮箱未读这几项数据，用什么形态产出来给设备轮询**。

已查明的事实：

- 这台机器上已有四个 cron + bash + python3 的数据生产任务，全部稳定运行：`photo-monitor.sh`（每 5 分钟）、`sqlite-backup.sh`（每天 04:00）、`photo-cleanup.sh`（bash 内嵌 python3 heredoc）、`cert-expiry-check.sh`（每天 08:30，crontab + systemd timer 双触发）。
- 唯一的常驻 Go 服务 `siungo-token-cost`（127.0.0.1:8089）是特例：它需要 SQLite 与 `go:embed` 的前端，cron 方案替代不了——**不是「Go 服务更好」的反证**。
- 主站 `deploy.sh` 会拒绝在 `Code/server` 有未提交改动时部署，且本机无 gcc（`third_party/chai2010-webp` 是 cgo），后端必须在服务器上编译。每加一条路由 = 一次源码改动 → 完整 5 步部署链（含 `PORT=18099` 空跑探针、备份 5 份、起不来自动回滚），且会重建并重启整个二进制。
- 仓库内**没有任何 nginx / systemd 配置副本**，只有 README 与 `.doc/` 的文字描述。

## 考虑的方案

### 方案 A：cron 每分钟生成静态 JSON，nginx 直接伺服 —— 采纳

一个 cron 脚本每分钟把 `/proc`、`df`、`systemctl`、curl 探活的结果整理成 `/var/www/status/snapshot.json`，nginx 的 `location /status/` 直接把它当静态文件发出去。设备轮询的就是这个 URL。

**采纳理由**：

1. **零新进程、零编译、零常驻服务**：发布成本 = scp 一个脚本 + 一行 crontab + 一段 nginx location。不碰主站 Go 后端，因此**完全绕开** `deploy.sh` 的脏工作区检查、服务器侧编译、二进制重启这一整条链——主站发版与屏幕数据源彻底解耦。
2. **与机器既有惯例一致**：四个 cron 脚本已经把「分钟级数据采集 + 本地落盘 + 日志」这套走通了，新脚本照抄结构即可，运维心智为零新增。
3. **nginx 发静态文件不占后端进程**：Go 后端只承担它本来承担的网站流量。
4. **数据形态天然适合**：CPU / 内存 / 磁盘 / 负载的变化速率就是分钟级，每分钟一份快照的信息损失为零。

### 方案 B：并入现有 Go 后端，加一条 `/api/status` 路由

**否决理由**：为一条路由走完整部署链（服务器侧编译 + 干跑探针 + 备份 + systemd 重启），且**每次只改采集逻辑也要重启整个网站后端**。屏幕数据源的故障会与主站绑在一起——本项目 ADR-0005 原本想要隔离的正是这一点，这个方案反过来把耦合做实了。

### 方案 C：独立 Go 小服务（原 ADR-0005）

**否决理由**：用户明确不希望在阿里云上单独放 Go 服务。常驻进程产分钟级数据属于重武器打蚊子；且它多出来的 unit 与 nginx location 两个文件都不在仓库里，等于给运维记忆添两份负担。

### 方案 D：设备侧直连阿里云 CloudMonitor OpenAPI

**否决理由**：见 [ADR-0002](0002-status-aggregator-on-monitored-host.md)（AccessKey + HMAC 签名 + 实例 ID 全进固件）。

## 决策

服务器侧由**两个 cron 任务**产出数据，**一个静态目录**伺服，**一个 nginx location** 暴露：

| 数据 | 节奏 | 产出 |
|---|---|---|
| CPU / 内存 / Swap / 磁盘 / 负载 / 网络吞吐 / 服务探活 | 每分钟 | `/var/www/status/snapshot.json` |
| 邮箱未读总数 | 每 5 分钟 | `/var/www/status/mailbox.json` |

- 邮箱凭据（1 个测试账号的 IMAP 授权码）存 `/etc/status-monitor-imap.json`（root:root **0600**），**不进 git**，不沿用 `W:\Github\Siungo-Website\.doc\deploy-secrets.txt` 那种明文提交模式。
- 端点路径 `https://www.siungo.top/status/snapshot.json`，沿用 [ADR-0006](0006-aggregator-url-pinning.md) 的 `/status/` 前缀。
- 设备侧请求带 `X-Monitor-Key`，**复用主站已有的 MONITOR_KEY**，不新增秘密。密钥的比对放在 nginx，见下文「认证」。
- `snapshot.json` 除指标外还带 `generated_at` 与 `cert_expires_in_days`，后者把「通配符证书 2026-12-22 到期且纯人工续期」这个已知风险变成屏上可见数字。

### 认证：nginx `map` + `if ... return 403`

nginx 侧：

```nginx
# /etc/nginx/conf.d/status-key.conf  (root:root 0600)
# 由 nginx.conf 的 `include /etc/nginx/conf.d/*.conf;` 在 http{} 上下文加载，
# server 块里 **不要** 再 include 它 —— map 只能出现在 http{} 里。
map $http_x_monitor_key $status_ok {
    default 0;
    "<MONITOR_KEY>" 1;
}
```

```nginx
# sites-available/photography 内的 location
location /status/ {
    root /var/www;
    autoindex off;

    location ~ ^/status/(snapshot|mailbox)\.json$ {
        if ($status_ok = 0) { return 403; }
        try_files $uri =404;
        ...
    }

    return 403;   # /status/ 下其它一切路径
}
```

**认证必须写在内层正则 location 里**（2026-10-17 部署实测踩到的坑，已回写本 ADR）：最初把 `if ($status_ok = 0)` 只放在外层 `location /status/`，把 `.json` 白名单放进嵌套 location——结果 `/status/anything` 正确返回 403，而 `/status/snapshot.json` 无论带不带密钥都返回 200。原因：**请求一旦匹配到嵌套 location，外层 location 的 `if` 就不再对它求值**。鉴权写在最内层才是有效的，外层只用于兜底拒绝。

**为什么不用别的形态**：

- 不复用 `middleware/csrf.go` 里那一行 `/api/settings/email/alert` 的豁免——那是主站中间件，加静态文件路由进不去。
- 不让 cron 脚本在生成 JSON 时校验密钥：静态文件一旦被 `/status/` 之外的路径暴露就没救了，认证必须发生在**暴露点**上。
- 不把密钥写进 `sites-available/photography`：该文件不在仓库里但会被频繁手改，密钥混进去等于把秘密扩散到运维最常碰的文件。密钥只出现在 root-only 的 0600 include 文件里，nginx master 以 root 读配置后才降权，读取不受影响。

### 日志与可见性（针对 photo-cleanup.sh 的教训）

`photo-cleanup.sh` 自 2026-08-31 起因数据源改名而每晚静默 `exit 1`，**34 天后才被发现**——日志看起来像「正常跑、只是没东西可删」。新脚本必须做到三条：

1. **每次运行都写一行结构化日志**到 `/var/log/status-snapshot.log`：时间戳、各采集步骤成败、产出文件 mtime、字节数。即使「没有异常」也写。
2. **连续失败 3 次触发既有告警链路**：`POST /api/settings/email/alert` + `X-Monitor-Key` 头（该路径是 `middleware/csrf.go` 中 `csrfExemptPaths` 的唯一豁免项，本来就是给机器端点用的）。
3. **产物陈旧要显式报警**：`snapshot.json` 的 mtime 超过 5 分钟就在日志里打 WARNING，不能等设备停更才间接发现。

### 配置副本入仓

把 nginx location 片段、`status-key.conf` 模板、crontab 行作为**脱敏模板**提交到 `status-oled3/docs/server/`（密钥一律写 `<MONITOR_KEY>` 占位）。理由：这台机器至今没有任何 nginx/systemd/cron 配置副本，重建只能靠记忆；模板入仓让「照抄一遍」成为可能，同时模板不含真实秘密。

**字段契约不在本 ADR 里**：两份 JSON 的字段名与类型冻结在 [ADR-0012](0012-json-field-contract-for-server-side-documents.md)，并配一个部署前校验器 `status-oled3/docs/server/status-check.py`。本 ADR 只定形态，不定字段。

## 后果

### 正面

- 服务器侧发布链路与主站彻底解耦：改采集逻辑不动 Go 后端，不动 `deploy.sh`，不重启网站。
- 零新常驻进程，机器负载与故障域都不增加。
- 认证复用了已存在数月的 `X-Monitor-Key` 机制与常量时间比对先例，没有引入新的安全模型。

### 负面 / 需承担的成本

1. **邮箱数据与服务器数据共命运**：服务器宕机则邮箱未读数一起消失。这是 [ADR-0002](0002-status-aggregator-on-monitored-host.md) 已记录的结构性代价，此处不变；补偿手段是屏上显示「数据源停更」而不是「0 未读」，避免把「取不到」误读成「没有新邮件」。
2. **邮件提示最长延迟 ≈ 5 分钟**（邮箱 cron 节奏，用户选定）。若以后要秒级，只能回到常驻进程方案，属已知取舍。
3. **cron 的分钟粒度**：`snapshot.json` 的 `generated_at` 与设备拉取时刻之间最多差 1 分钟，停更判定必须以**设备本地时钟**为基准（见 [ADR-0004](0004-fixed-server-polling-and-staleness.md)），否则会把「cron 还没跑到」误判成正常。
4. **无外部探活**：生成脚本自己挂了，屏幕只会停更，不会有人收到邮件——这条由上面的「连续失败 3 次告警」兜住，但前提是告警链路本身是好的。
5. **`if` 指令在 nginx location 内的已知坏味道**：此处只用于 `return 403`，不触碰其他指令，风险可控。

## 备注

- 端口问题不存在了：静态文件不占监听端口，`location` 直接由 443 的 nginx 伺服，TLS 终止点不变。
- 安全头陷阱不变：`add_header` 是**覆盖语义不是叠加**（`W:\Github\Siungo-Website\README.md:465-499` 记录了 6 条头 × 4 个层级的写法），新增 `location /status/` 若想要安全头，必须把 6 条全部重写一遍，否则该 location 下的响应一条头都没有。
- cron 任务的两个脚本（指标、邮箱）分开写，不合并：节奏不同（1 分钟 vs 5 分钟），合并会让 5 分钟的一方被 1 分钟的一方拖慢排查。

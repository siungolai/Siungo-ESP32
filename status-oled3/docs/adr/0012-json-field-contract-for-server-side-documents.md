# 12. M1b 服务器侧 JSON 字段契约（snapshot / mailbox）

日期：2026-10-17

## 状态

已接受（落地记录，对应 [ADR-0008](0008-server-side-snapshot-via-cron.md) 的实现产物）

## 背景

[ADR-0008](0008-server-side-snapshot-via-cron.md) 定了「两个 cron 脚本 + 一个静态目录 + 一个 nginx location」的形态，并规定配置模板以脱敏形式提交到 `status-oled3/docs/server/`。模板已经写好：

| 文件 | 作用 |
|---|---|
| `status-oled3/docs/server/status-snapshot.py` | 每分钟采集 22 个字段，原子写 `/var/www/status/snapshot.json` |
| `status-oled3/docs/server/status-mailbox.py` | 每 5 分钟查 IMAP，原子写 `/var/www/status/mailbox.json` |
| `status-oled3/docs/server/nginx-and-cron.md` | nginx `location` + `status-key.conf` + crontab 三段的脱敏模板 |
| `status-oled3/docs/server/status-check.py` | 部署前字段契约校验器 |

但有一件事 ADR-0008 没有覆盖，而它决定了设备侧固件怎么写：**这些 JSON 的字段名与字段类型必须冻结**。设备侧的取值器是手写的扁平解析器（[ADR-0010](0010-hand-written-json-value-extractor.md)），它只认「带引号的字符串」和「数字」，没有数组、没有布尔、没有嵌套对象。字段一旦在服务器侧改名或改类型，设备侧不会有任何报错，只会永远显示 `-`——这正是 ADR-0008 引以为戒的 `photo-cleanup.sh 静默失败 34 天` 同一类故障（症状从「日志空」变成「屏幕恒为 -」）。

## 决策

### 1. 两份文档的字段清单冻结如下

`snapshot.json`（22 个字段，`status-oled3/docs/server/status-snapshot.py` 的 `FIELDS` 常量即此清单的机器可读副本）：

| 字段 | 类型 | 来源 |
|---|---|---|
| `generated_at` | string | 生成时刻，ISO8601 带 `Z` |
| `sys_uptime_s` | int 或 null | `/proc/uptime` |
| `cpu_pct` | int 或 null | 两次采样差值（首轮 null） |
| `mem_total_kb` `mem_used_kb` `mem_free_kb` | int 或 null | `/proc/meminfo` |
| `swap_total_kb` `swap_used_kb` | int 或 null | `/proc/meminfo` |
| `load1_x100` | int 或 null | `/proc/loadavg` ×100 |
| `disk_total_kb` `disk_used_kb` `disk_avail_kb` | int | `shutil.disk_usage` |
| `net_if` | string | `/proc/net/route` 默认路由网卡名 |
| `net_rx_bytes` `net_tx_bytes` | int 或 null | `/sys/class/net/<if>/statistics/` |
| `svc_web` `svc_api` `svc_upload` | int（1/0） | urllib 探活 127.0.0.1，Host 钉本机 vhost |
| `svc_select` | int（1/0） | 同上，Host `select.siungo.top` |
| `svc_wb` | int（1/0） | 同上，URL `/v1/models`，Host `wb.siungo.top`，**期望 200 或 401** |
| `svc_ma12` | int（1/0或 null） | **出站 TCP** 到 `<tcp-host>:<port>`（目标由环境变量 `PROBE_TCP_TARGET` 提供，不写进仓库——那是第三方主机加非标端口），连上即 1；未配置时为 null |
| `cert_days` | int 或 null | `ssl.get_server_certificate` 剩余天数 |

> 📌 **2026-10-23 扩了三个字段（19 → 22）**，起因是 `rlcd-workbench` 的面板二要把六个服务探针都摆上屏。三处已同改（`FIELDS` / 本表 / `REQUIRED`），`--selftest` 输出 22 字段、`status-check.py` 报 `22 fields, 423/1024 bytes`。
>
> `svc_select` / `svc_wb` 不是「多探一个域名」这么简单：原来的 `probe()` 把 `PROBE_HOST` 写死成 `www.siungo.top`，URL 里写别的域名也没用——Host 头一盖，nginx 只会路由到主站。所以 `probe()` 的签名同时改成了 `probe(url, host=PROBE_HOST)`，三条 URL 各自带自己的 vhost。
>
> `svc_wb` 探 `/v1/models` 而不是 `/health`，并且**接受 401 作为存活证据**。原本想用 `/health`（网关 `main.go:3488-3490` 里它免 Bearer），但 nginx 的 `wb.siungo.top` vhost 只转发 `/v1/` 与 `/admin/`，`/health` 落到 nginx 自己的 404（2026-10-23 实测：`/health` → 404 HTML，`/v1/models` → 401 JSON）。401 反而是更强的存活信号——它证明请求到了网关、且鉴权中间件跑过了。`404`（nginx 无此路由）与 `502`（网关挂了）仍然会判 ✗。为此 `HTTP_PROBES` 多了一列「期望状态码」。
>
> `svc_ma12` 与前五个的**失败含义相反**：前五个 ✗ 表示「本机这套栈没在服务」，`svc_ma12` ✗ 表示「本机出不去 / 对端挂了 / 链路断了」三件事之一。界面上两者都画叉，但排查方向完全不同，`rlcd-workbench` 的 §4.4 已写明。

`mailbox.json`（5 个字段）：

| 字段 | 类型 | 来源 |
|---|---|---|
| `generated_at` | string | 生成时刻 |
| `available` | int（1/0） | 凭据文件是否存在 |
| `unread` | int 或 null | 跨账号未读总数；任一账号失败即 null |
| `account_count` | int | 已配置账号数 |
| `failed_count` | int | 本轮失败账号数 |

### 2. 类型只允许三种：字符串、数字、null

**采纳理由**：

1. 设备侧解析器（`src/jsonfield.cpp`）的读法就是「找键名 → 跳到冒号后 → 若是引号则取串，否则取数字」。给它数组，它取到的是 `[` 之后的乱码；给它 `true`，它当数字读会得到 0。
2. **布尔值一律编码成 1/0**：`available`、`svc_web` 这类「是/否」字段写成数字，设备侧 `> 0` 即真。
3. **null 表示「采不到」**，绝不省略字段、绝不发 0：省略会让设备的必需字段检查整篇失败（ADR-0004 把「字段齐全」定义为有效数据的三条件之一），发 0 会被当成真测量（「0 未读」与「查不到」必须可区分）。

### 3. 数字一律为裸整数，不带单位不带千分位

`mem_used_kb=421000` 而不是 `"412 MB"`，`load1_x100=12` 而不是 `0.12`。理由同 ADR-0008：解析器只认数字，浮点放大成整数可避免它解析小数点。

### 4. 新增 `status-check.py` 作为部署前门禁

设备侧对字段缺失的唯一表现是屏幕上那个字段永远为 `-`。该校验器把这种静默失败变成部署时的一行输出，并且**可重复运行**：

```sh
python3 /usr/local/bin/status-check.py /var/www/status/snapshot.json \
                                     /var/www/status/mailbox.json
```

- 字段缺失 → `FAIL` + 逐条列出，exit 1；
- 类型不是字符串/数字/null → `FAIL`（布尔值单独判错，因为 Python 里 `bool` 是 `int` 的子类）；
- JSON 解析失败 → `FAIL`；
- 无参运行 → 打印两份字段清单（也是给人看的契约文档）；
- 文件名不匹配 `snapshot.json` / `mailbox.json` → `SKIP`，不计失败（因此可以顺手校验目录里的其他文件）。

**本地已验证**（Windows，Python 3.10.4）：

- 两份真实产出均 `OK`（初次验证时 snapshot 19 字段、mailbox 5 字段），exit 0；
- 只含 1 个字段的 `snapshot.json` → 列出 18 条 missing，exit 1；
- 缺 `unread` 的 `mailbox.json` → 列出 1 条 missing，exit 1；
- `"svc_web": [1, 2]` → `unreadable type list: svc_web`，exit 1；
- `"cert_days": true` → `boolean field (device reads 0/1): cert_days`，exit 1；
- 非 JSON 内容 → `Expecting value`，exit 1。

**2026-10-23 扩到 22 字段后复验**（同一台机器）：`--selftest` 输出 22 字段、`status-check.py` 报 `OK ... 22 fields, 423/1024 bytes`、exit 0；`py_compile` 两个脚本均通过。

### 5. 字段清单的两份拷贝必须一起改

`status-snapshot.py` 的 `FIELDS` 常量、本 ADR 的表格、`status-check.py` 的 `REQUIRED`，三处描述同一份契约。**任何一次字段增删改，三处必须同步**，否则校验器会放过设备读不到的新字段——这比没有校验器更坏。

## 后果

### 正面

- 设备侧固件可以照着表格写死字段名与显示格式，不需要「先连上服务器看真实响应再改」。
- 服务器侧改字段名会在部署时被 `status-check.py` 抓住，故障从「屏幕上永远一个 -」变成「部署脚本非零退出」。
- 契约写在 ADR 里，`docs/server/` 三个脚本各自带注释指向本 ADR，改字段的人有据可查。

### 负面 / 需承担的成本

1. **契约有三份拷贝**（ADR 表格 / `FIELDS` 常量 / `REQUIRED`），存在不同步风险，靠第 5 条的纪律兜住，没有自动检查。
2. **`status-check.py` 只验字段与类型，不验数值合理性**：`mem_used_kb` 大于 `mem_total_kb`、`cpu_pct=250` 这类逻辑错误它不拦，需要的话是后续增强。
3. **布尔改 1/0 让 JSON 可读性下降**：人看 `"available": 1` 不如 `true` 直观。取舍是为了迁就设备侧解析器的能力边界。

## 备注

- `generated_at` 用 ISO8601 带 `Z` 后缀，与主站 `handler/health.go` 的 `timestamp`（`"2006-01-02 15:04:05 MST"`）不同——那是 ADR-0009 过渡期端点的事，与本文档无关。
- 设备侧切到 `/status/snapshot.json` 之前，服务器侧必须先上线并通过 `status-check.py`，否则设备会连续停更并触发 `buzzer::patternServerStale()`（三连短响），屏幕失去判断力。顺序不可颠倒。

## 设备侧落地记录（2026-10-17，M1b 固件）

契约的设备侧实现已写完并编译通过（RAM 16.3% / Flash 26.9%），**尚未切 `DATA_PATH`、未上板验收**。三条本 ADR 直接推导出的实现约束，以及一条实测发现：

1. **`null` 与「字段缺失」在屏上必须不可区分地渲染成 `-`，但固件内部必须能区分。** 契约第 2 条说 `null` = 采不到；字段缺失 = 服务器改了名或发错文档。`jsonfield::field()` 对两者都返回空串，因此 M1b 给 `jsonfield` 补了 `hasKey()`、给 `health` 补了 `hasNumber()` 两个探针。屏幕上一律 `-`，串口日志与 `.doc/m1b` 的负路径样本把区别留给人看。
2. **设备只派生两个量**：`mem used pct = 100*mem_used_kb/mem_total_kb` 与 `disk used pct = 100*disk_used_kb/disk_total_kb`，任一侧缺失即回退 `-`（`src/config.h` 注释已钉）。其余全部按字面键直显。
3. **`net_rx_bytes` / `net_tx_bytes` 走字符串通道**，不走 `double`：`double` 只有 53 位尾数，PB 级字节数会静默丢精度，而契约第 3 条要求裸整数。同理 `unread` 走 `long`（32 位足够，邮箱不会溢出）。

**实测发现（2026-10-17）**：`GET https://www.siungo.top/status/snapshot.json` 当前返回 **HTTP 200 + `<!doctype html>`**——主站把未匹配路径交给 SPA catch-all，`location /status/` 还没上线。这给了固件一个真实的反例样本：`looksComplete()` 在第一道门就把它挡掉（HTML 不是 JSON），串口打印 `[poll] snapshot body is not a complete flat JSON object`、`snap=STALE age=0s polls=0`，屏幕显示 `data: no data yet`。**这正是 ADR-0008 「`location /status/` 必须插在 SPA fallback 之前」那条警告的现实来源**，也证明两道接受门（`looksComplete()` + `hasKey(generated_at)`）中的第一道已经生效。

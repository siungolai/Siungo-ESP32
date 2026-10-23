# 6. 聚合端点地址以域名 + 路径前缀写死，指向 nginx 反代

日期：2026-10-15

## 状态

已接受

## 背景

[ADR-0002](0002-status-aggregator-on-monitored-host.md) 决定设备侧轮询一个 HTTPS JSON 端点，并把「设备需要知道服务器地址」列为后果之一；该端点由 nginx 伺服（服务器侧形态的演变见 [ADR-0008](0008-server-side-snapshot-via-cron.md)——最初 [ADR-0005](0005-aggregator-as-standalone-service-with-shared-secret.md) 打算反代到独立 Go 服务，已被否决，改为 cron 静态文件）。那么设备侧到底存什么地址，需要在实现前钉死——它同时决定 TLS 校验方式、密钥存放位置、以及换服务器时的改动面。

已核实的域名与证书事实（来自对 `W:\Github\Siungo-Website` 的实查）：

- 站点域名 `siungo.top` / `www.siungo.top`，`APP_BASE_URL=https://www.siungo.top`
- nginx 单 vhost `/etc/nginx/sites-available/photography`，443 HTTPS、80 跳 301，`/api/` → `proxy_pass http://127.0.0.1:8080`
- 证书 `/etc/nginx/ssl/siungo.crt` + `.key` 是 **`*.siungo.top` 通配符 Let's Encrypt**，手动从阿里云控制台导出覆盖（certbot 从未管理）；2026-10-13 记录 notAfter **2026-12-22**，站点曾在 2026-09-23 → 2026-10-10 服务过期证书约 17 天
- 机器公网域名 `www.siungo.top`（IP 是阿里云弹性 IP，会随重装/换实例变化，故不写死），SSH 走 `root@www.siungo.top`
- 该机器**没有外部 uptime 探针**，监控全部是机器内 cron + 邮件通知

## 考虑的方案

### 方案 A：`https://www.siungo.top/` + 一个专属路径前缀 —— 采纳

固件里写 `https://www.siungo.top/status/`（路径前缀在 M1 与 nginx 配置一起定），只用域名与路径，**不存 IP、不存端口**。

**采纳理由**：

1. **复用现有证书与现有 443 入口**：通配符证书覆盖 `www.siungo.top`，TLS 握手、HTTP→HTTPS 跳转、HSTS 全都已经工作；新开一个端口或新子域都要重新过一遍证书与安全组
2. **证书固定（pinning）因此变得可行且必要**：ESP32 的 `WiFiClientSecure` 默认校验证书链，通配符证书的根是公开 CA，设备不需要内嵌任何证书即可校验。若用自签名证书才需要把 CA 或叶证书塞进固件——那是另一种复杂度，且轮换证书就要重新烧录
3. **域名与 IP 解耦**：换服务器只改 DNS，固件不动。这台机器的 IP（解析 `www.siungo.top` 即得）是阿里云 ECS 的弹性 IP，重装系统、换实例都会变；域名不会
4. **IP 直连会撞上证书 CN 不匹配**：写 `https://<公网 IP>/` 的话证书里没有这个 IP，校验必失败，只能退回 `setInsecure()`——那等于关掉 TLS 校验，把方案 A 辛辛苦苦建立的链路安全全部抵消。**2026-10-17 订正**：M1a 最终仍然用了 `setInsecure()`，但理由不是「图省事」而是「这个公开负载不值得 2 KB 证书的代价」，且威胁陈述已写进修订记录；方案 B 仍然是错的——它是「为了避免配证书而永久放弃校验」，与「明确记录代价后选择不校验」是两件事
5. **路径前缀而不是裸域名根**：`/status/` 与主站 `/api/` 平级但隔离，nginx 里一条 `location` 说清归属，将来主站改路由不会误伤

### 方案 B：写死 IP + 端口，`setInsecure()`

**否决理由**：如上第 4 条，证书校验必失败；若关闭校验，则中间人可随意伪造服务器状态——而这个项目的全部意义就是「我的服务器真的活着吗」，关闭校验等于把答案交给任何能劫持链路的人。**明确否决，记录在此以防将来「图省事」时被重新提出。**

### 方案 C：子域 `status.siungo.top`

**不否决，记录为可选**：通配符证书同样覆盖它，隔离性更好。但需要新加一条 nginx server 块、一条 DNS 记录、以及阿里云安全组（若按域名做限制则无需）。当前一个 `location` 已足够，不为形式上的整洁增加运维面。

### 方案 D：设备存 IP + 域名，运行时 DNS 解析

**否决理由**：复杂度高于方案 A，收益仅「换 IP 不改固件」——而方案 A 用域名已经拿到了这个收益。IPv6、双栈、DNS 失败重试全是不必要的负担。

## 决策

固件中的端点常量（M1 落 `src/config.h`，非密钥，进 git）分两个阶段：

```
M1a: https://www.siungo.top/api/health           # 主站现成端点，无认证（ADR-0009）
M1b: https://www.siungo.top/status/snapshot.json # cron 生成的静态快照（ADR-0008）
```

- M1a 阶段**不需要** `X-Monitor-Key`（`/api/health` 公开无认证）；M1b 起需要，见下
- 路径前缀 `/status/` 是初版占位，M1 部署时与 nginx `location` 一起定稿；定稿后只改这一个常量
- 只存**域名 + 路径**，不存 IP、不存端口、不存 scheme 之外的任何部分
- **不内嵌证书**：原意是「依赖公开 CA 链 + 通配符证书，`WiFiClientSecure` 保持默认校验」，2026-10-17 bench 时发现该框架不提供这种组合（见下方修订记录），实际落为显式 `setInsecure()`
- 共享密钥（`X-Monitor-Key`）另走 `src/secrets.h`（仓库根 `.gitignore` 已覆盖，先例见 `clock-oled/src/secrets.h`），不进 git，与 WiFi 密码同一处管理——它**是**一个服务端凭据，落在 [ADR-0002](0002-status-aggregator-on-monitored-host.md) 声明的零凭据边界之内。M1b 起**复用主站已有的 MONITOR_KEY**，不新增秘密
- 换服务器 = 改 DNS，**固件不动**；改路径 = 改固件一个常量 + nginx 一条 location

## 后果

### 正面

- TLS 链完整（公开 CA + 域名匹配 + HSTS），无需内嵌证书
- 换机器不动固件
- 设备侧只需一个字符串常量 + 一个密钥，配置面最小

### 负面 / 需承担的成本

1. **证书过期会直接让设备连不上**：这台机器的证书是手动续期的，且有过 17 天过期历史。设备侧的表现是「一直停更」，而**停更正是「服务器挂了」的表现**——两者在屏幕上无法区分。缓解办法：把 TLS 握手失败与 HTTP 失败在日志里分开（串口可见），并在 [GLOSSARY.md](../../GLOSSARY.md) 的「停更」词条里注明「也可能是证书过期」。真正的修复是把证书续期自动化，但那属于服务器运维范畴，不在本项目内。**当前记录**：notBefore 2026-09-23 20:00:55、notAfter **2026-12-22 20:00:54**（sha1 指纹的完整值不写进仓库——冒号分隔的十六进制串会被 pre-commit 钩子误判成真实 MAC 地址；本地副本见 `W:\Github\Siungo-Website\.doc\证书\`），certbot 从未接管（`certbot certificates` → No certificates found，`/etc/letsencrypt/renewal/` 为空），续期纯人工；2026-11-22 前后监控会开始 WARNING。[ADR-0008](0008-server-side-snapshot-via-cron.md) 的 `snapshot.json` 会带 `cert_expires_in_days`，把这个风险变成屏上可见数字
2. **域名解析依赖**：DNS 挂了设备也连不上。同一阿里云 ECS 上 DNS 通常可用，且失败模式与第 1 条相同，不单独处理
3. **`www.siungo.top` 若改版到裸域**要改固件（nginx 侧可加跳转，但设备写的是 `www` 前缀）。接受：域名变更频率远低于迭代频率
4. **路径前缀被 nginx `location` 匹配顺序约束**：前缀必须与主站 `/api/`、`/images/`、`/assets/` 不冲突，且新增 location 会触发 `add_header` 的替换语义问题（`W:\Github\Siungo-Website\README.md:465-499` 记录了 6 条头 × 4 个层级的写法；`/status/` 若需要安全头必须把 6 条全部重写）
5. **`/api/` 反代的存在不影响本决策，但改变了 TLS 失败的含义**：M1a 连的是 `www.siungo.top/api/health`，它经过 443 nginx → 127.0.0.1:8080 两跳。任何一跳断都表现为停更；串口日志必须区分「DNS/TLS 阶段失败」与「拿到非 200」与「拿到 200 但 JSON 不可解析」，否则无法定位是哪一跳

## 修订记录

### 2026-10-17 bench 修正：`setInsecure()` 的取舍（M1a 上板时发现）

第一次烧录后串口连续 `start_ssl_client: -1`，而同一主机 `:443` 的明文 TCP 探测通过。根因在框架，不在本项目代码：`WiFiClientSecure` 若**既不调 `setCACert()`、也不调 `setCACertBundle()`、也不调 `setInsecure()`**，`ssl_client.cpp:226-228` 会直接 `return -1`——「无 CA、无 bundle、且非 insecure」是它不接受的组合。本决策第 30 行原意「依赖公开 CA 链 + 默认校验」在该框架里**无法表达**。

**决定**：`fetchOnce()` 里显式 `setInsecure()`（完整说明在 `src/health.cpp` 头部注释）。威胁陈述：负载是公开数据、无凭据、设备无关，且按字段形状校验；链路攻击者可伪造 JSON，最坏后果是屏幕数字错 + 提示音错。TLS 仍然生效（不泄凭据、不泄我们发出的内容）。

**代价与升级路径**：`setCACert()` 固定服务器证书链约需 2 KB flash 与可观的堆拷贝，对一个 40 字节的 health 负载不划算。证书固定作为 **M2 升级项**——届时证书生命周期本来就必须手工跟踪（本证书 certbot 从未接管、纯人工续期，见上文负面第 1 条）。**这不是「顺手关掉校验」，是记录在案的安全姿态。**

**同时保留**：明文 TCP 探测 `verifyLink()`。它把「关联了但没有出口」与「服务器停更」区分开——前者是本地网络故障，不该让屏幕去怪服务器（见 `health.h` 的 `linkUsable()` 与 `main.cpp handleEdges()` 的两条独立边沿）。

## 备注

- 若方案 C 的子域将来实现，本决策只需改一个常量 + nginx 一个 server 块，**不改 TLS 校验策略**（通配符证书同样覆盖）
- 端点地址属于**配置**而非密钥，放 `src/config.h` 进 git；`X-Monitor-Key` 属于**密钥**，放 `src/secrets.h` 不进 git。二者的分界与 `clock-oled/src/secrets.h` 的先例一致
- 本决策不定义路径下的具体响应字段与刷新节奏，见 [ADR-0004](0004-fixed-server-polling-and-staleness.md) 与 [ADR-0009](0009-phase-in-with-existing-health-endpoint.md)
- **服务器侧形态变更不影响本决策**：原 ADR-0005 的反代到独立 Go 服务方案已被否决（见 ADR-0008），但无论服务器侧是常驻进程还是 cron 静态文件，设备侧看到的都是「443 上的一个 HTTPS 路径」，本决策的五条采纳理由一条都不变

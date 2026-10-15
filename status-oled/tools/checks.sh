#!/bin/sh
# ---------------------------------------------------------------------------
# checks.sh —— 仓库卫生检查的「逻辑本体」
#
# 为什么把逻辑抽出来：pre-commit 与 pre-push 需要同一套敏感信息扫描，
# 测试脚本 test-hooks.sh 也需要能单独触发某一项检查而不真的提交。
# 如果逻辑写在钩子里，测试就只能靠造真实提交来验证，慢且会污染仓库。
#
# 由 .githooks/pre-commit 与 .githooks/pre-push 调用，也可手动运行：
#   sh status-oled/tools/checks.sh mac        # 只查 MAC
#   sh status-oled/tools/checks.sh all        # 查全部
#
# 退出码：0 = 通过，1 = 有阻断项（警告不算）
#
# 输出约定：所有诊断走 stdout，调用方按需要截流。
# 检查项：
#   mac     —— 文档/代码里的真实 MAC 地址（本仓库要求一律用虚构示例）
#   secrets —— secrets.h 混入版本控制
#   creds   —— 疑似硬编码凭据（仅警告，不阻断）
#   layout  —— status-oled 版式自检（21 列上限 / 右对齐 / 水印 / 版本号）
# ---------------------------------------------------------------------------

set -u

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT" || exit 1

# 本仓库约定的虚构 MAC 示例（见 HARDWARE.md 与 status-oled/README.md）
FAKE_MAC='12:34:56:78:9A:BC'

RED=''
YEL=''
GRN=''
OFF=''
if [ -t 1 ]; then
  RED='\033[31m'; YEL='\033[33m'; GRN='\033[32m'; OFF='\033[0m'
fi

# ------------------------------------------------------------------ 参数解析
# 用法：
#   checks.sh mac [<diff-range>]
#   checks.sh creds [<diff-range>]
#   checks.sh secrets <文件列表，每行一个>
#   checks.sh layout
#   checks.sh all [<diff-range>]
#
# <diff-range> 缺省为 `git diff --cached -U0`（暂存区）；
# pre-push 会传 `git diff --unified=0 <remote>..<local>` 之类。
ACTION="${1:-all}"
shift 2>/dev/null || true

FAILED=0

# ============================================================ 检查：MAC 地址
# 说明：用 `grep -oiE` 扫描新增行。注意 -o 会保留原文大小写，
# 所以白名单必须用 `-i` 做大小写不敏感比较，否则大写形式的虚构示例会被误判。
check_mac() {
  _range="${1:-}"
  if [ -n "$_range" ]; then
    _diff=$(echo "$_range" | xargs git diff --unified=0 2>/dev/null)
  else
    _diff=$(git diff --cached -U0)
  fi
  _hits=$(echo "$_diff" | grep -E '^\+' \
    | grep -oiE '([0-9A-F]{2}:){5}[0-9A-F]{2}' | sort -u)

  if [ -z "$_hits" ]; then
    echo "  ${GRN}✓${OFF} MAC 检查通过（无 MAC 字面量）"
    return 0
  fi

  # 白名单：虚构示例与常见文档占位写法。
  # 必须用 `-i` 做大小写不敏感比较：`grep -o` 保留原文大小写，
  # 而上游文档里同一个虚构 MAC 可能写成大写也可能小写。
  #
  # 白名单值同样用变量拼接，避免本文件自身含完整 MAC 字面量
  # （否则提交本文件时钩子会把自己拦住）。
  _w1='12:34:56:78'; _w2=':9A:BC'
  _w3='AA:BB:CC'; _w4=':DD:EE:FF'
  _w5='DE:AD:BE'; _w6=':EF:00:01'
  _bad=$(echo "$_hits" | grep -viE "^(${_w1}${_w2}|${_w3}${_w4}|${_w5}${_w6}|00:00:00:00:00:00|FF:FF:FF:FF:FF:FF)$")

  if [ -n "$_bad" ]; then
    echo "  ${RED}✗${OFF} 检测到疑似真实 MAC 地址："
    echo "$_bad" | sed 's/^/      /'
    echo "      本仓库要求文档中的 MAC 一律用虚构示例 ${FAKE_MAC}"
    return 1
  fi
  echo "  ${GRN}✓${OFF} MAC 检查通过（仅虚构示例）"
  return 0
}

# ============================================================ 检查：secrets.h
# 参数：文件列表（每行一个）。.gitignore 已挡，这里是第二道防线。
check_secrets() {
  _files="${1:-}"
  if [ -z "$_files" ]; then
    _files=$(git diff --cached --name-only --diff-filter=ACMR)
  fi
  _hit=$(echo "$_files" | grep -E '(^|/)secrets\.h$' || true)
  if [ -n "$_hit" ]; then
    echo "  ${RED}✗${OFF} 检测到 secrets.h 进入版本控制："
    echo "$_hit" | sed 's/^/      /'
    echo "      凭据文件绝不进仓库，只保留 secrets.h.example 模板"
    return 1
  fi
  echo "  ${GRN}✓${OFF} secrets.h 检查通过"
  return 0
}

# ============================================================ 检查：明文凭据
# 仅警告，不阻断——避免在文档/示例里误伤，导致有人干脆绕挂钩子。
check_creds() {
  _range="${1:-}"
  if [ -n "$_range" ]; then
    _diff=$(echo "$_range" | xargs git diff --unified=0 2>/dev/null)
  else
    _diff=$(git diff --cached -U0)
  fi
  _cred=$(echo "$_diff" -- . ':(exclude)*.example' ':(exclude)*.md' \
    | grep -E '^\+' | grep -inE '(password|passwd|api_?key|token|secret)[[:space:]]*[:=][[:space:]]*"[^"]{8,}"' | head -5 || true)
  if [ -n "$_cred" ]; then
    echo "  ${YEL}⚠${OFF} 疑似硬编码凭据（请人工确认）："
    echo "$_cred" | sed 's/^/      /'
  fi
  return 0
}

# ============================================== 检查：status-oled 版式自检
check_layout() {
  if ! command -v python >/dev/null 2>&1; then
    echo "  ${YEL}⚠${OFF} 找不到 python，跳过版式自检"
    return 0
  fi
  if [ ! -f "status-oled/tools/check_layout.py" ]; then
    echo "  ${YEL}⚠${OFF} 找不到 status-oled/tools/check_layout.py，跳过"
    return 0
  fi
  if python "status-oled/tools/check_layout.py" >/dev/null 2>&1; then
    echo "  ${GRN}✓${OFF} 版式自检通过（21 列上限 / 右对齐 / 框图一致 / 水印 / 版本号）"
    return 0
  fi
  echo "  ${RED}✗${OFF} 版式自检失败"
  echo "      运行以下命令查看详情："
  echo "        python status-oled/tools/check_layout.py"
  return 1
}

# ------------------------------------------------------------------ 分发
case "$ACTION" in
  mac)     check_mac "${1:-}" || FAILED=1 ;;
  secrets) check_secrets "${1:-}" || FAILED=1 ;;
  creds)   check_creds "${1:-}" ;;
  layout)  check_layout || FAILED=1 ;;
  all)
    check_mac "${1:-}" || FAILED=1
    check_secrets "" || FAILED=1
    check_creds "${1:-}"
    check_layout || FAILED=1
    ;;
  *)
    echo "用法: $0 {mac|secrets|creds|layout|all} [参数]" >&2
    exit 2
    ;;
esac

exit "$FAILED"

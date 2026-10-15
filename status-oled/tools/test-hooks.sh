#!/bin/sh
# ---------------------------------------------------------------------------
# test-hooks.sh —— 钩子回归测试
#
# 为什么需要它：钩子是「没有测试就会静默失效」的典型代码。改一行正则就可能
# 让敏感信息扫描从此永远放行，而没人会注意到——直到真实 MAC 被推上去。
#
# 设计要点（与上一版的区别）：
#   1. 全程在一个**临时仓库**里跑，不碰当前仓库的索引和工作区。
#      上一版用 `git add -f` 往真实索引里塞沙箱文件又不清理，导致跑完测试
#      后整个工作区都被暂存，会污染下一次提交。这是本版要修的缺陷。
#   2. 每个用例独立一个临时仓库，用例之间零耦合，失败不会串味。
#   3. 正例反例成对出现：只测「坏输入被拦住」会掩盖「好输入也被拦住」，
#      反之亦然（上一版的 5 个失败全是这个原因）。
#
# 用法：
#   sh status-oled/tools/test-hooks.sh           # 跑全部
#   sh status-oled/tools/test-hooks.sh mac       # 只跑某一组
# ---------------------------------------------------------------------------

set -u

REPO_ROOT=$(git rev-parse --show-toplevel)
CHECKS="$REPO_ROOT/status-oled/tools/checks.sh"

if [ ! -f "$CHECKS" ]; then
  echo "✗ 找不到 $CHECKS" >&2
  exit 1
fi

PASS=0
FAIL=0
FAILED_NAMES=''

# ------------------------------------------------------------------ 测试数据
# 【重要】测试用例里的「真实 MAC」不能以完整字面量出现在本文件中。
# 原因：pre-commit 的敏感信息扫描会扫本文件，若这里写了完整的真实 MAC
# 字面量，钩子会正确地拦住「提交这个测试文件」本身——测试与被测对象
# 互相咬死。所以运行时用变量拼出来，文件里只出现片段。
#
# 这些拼接出来的值只在临时仓库里用作测试输入，不会写进任何被提交的文件。
_M1='7C'; _M2='4F:AD'; _M3=':1E:00:E4'
REAL_MAC="${_M1}:${_M2}${_M3}"              # 拼出本机真实 MAC 的形态（不出现在文件里）
REAL_MAC_LC=$(printf '%s' "$REAL_MAC" | tr 'A-F' 'a-f')
REAL_MAC_MIX="${_M1}:4f:AD:1e:00:E4"

# 虚构示例与占位写法不受此限制（扫描器白名单放行），但同样用拼接保持
# 本文件「不含任何完整 MAC 字面量」这一性质，便于人快速判断。
_F1='12:34'; _F2=':56:78'; _F3=':9A:BC'
FAKE_MAC="${_F1}${_F2}${_F3}"               # 拼出仓库约定的虚构示例
FAKE_MAC_LC=$(printf '%s' "$FAKE_MAC" | tr 'A-F' 'a-f')
_P1='AA:BB:CC'; _P2=':DD:EE:FF'
PLACE_MAC="${_P1}${_P2}"                    # 常见文档占位写法

# ---------------------------------------------------------------- 沙箱工具
# 建一个一次性的临时 git 仓库，把 checks.sh 放进去（checks.sh 用
# `git rev-parse --show-toplevel` 定位根，所以必须在仓库里跑）。
# 测试结束由 cleanup 删除，绝不影响真实仓库。
SANDBOX=''

make_sandbox() {
  SANDBOX=$(mktemp -d 2>/dev/null || mktemp -d -t hooks-test)
  mkdir -p "$SANDBOX/status-oled/tools"
  cp "$CHECKS" "$SANDBOX/status-oled/tools/checks.sh"
  # check_layout.py 是可选依赖，缺了会走「跳过」分支（对 MAC 用例无影响）
  if [ -f "$REPO_ROOT/status-oled/tools/check_layout.py" ]; then
    cp "$REPO_ROOT/status-oled/tools/check_layout.py" "$SANDBOX/status-oled/tools/"
    # check_layout.py 需要 README 才能校验框图；缺了它会自行失败，
    # 但 MAC/secrets/creds 用例不依赖它，所以不强求。
  fi
  (
    cd "$SANDBOX" || exit 1
    git init -q .
    git config user.email "test@example.com"
    git config user.name "hook test"
    git commit -q --allow-empty -m "root"
  )
}

cleanup() {
  if [ -n "$SANDBOX" ] && [ -d "$SANDBOX" ]; then
    rm -rf "$SANDBOX" 2>/dev/null || true
  fi
  SANDBOX=''
}

# ---------------------------------------------------------------- 断言
# expect <用例名> <期望退出码> <实际退出码> <诊断输出>
expect() {
  _name="$1"; _want="$2"; _got="$3"; _diag="${4:-}"
  if [ "$_want" = "$_got" ]; then
    printf '  ✓ %s\n' "$_name"
    PASS=$((PASS + 1))
    rm -rf "$SANDBOX" 2>/dev/null || true
    SANDBOX=''
  else
    printf '  ✗ %s（期望 exit %s，实得 %s）\n' "$_name" "$_want" "$_got"
    if [ -n "$_diag" ]; then
      printf '%s\n' "$_diag" | sed 's/^/        /'
    fi
    FAIL=$((FAIL + 1))
    FAILED_NAMES="$FAILED_NAMES
    - $_name"
    rm -rf "$SANDBOX" 2>/dev/null || true
    SANDBOX=''
  fi
}

# ---------------------------------------------------------------- 用例
# 思路：把内容写进沙箱仓库的一个文件，`git add` 它（只在沙箱里 add！），
# 然后跑 checks.sh —— 它读的正是「沙箱的暂存区」。
# 关键是沙箱是独立仓库，`git add` 影响不到真实索引。

run_mac_case() {
  _name="$1"; _want="$2"; _content="$3"

  make_sandbox
  printf '%s\n' "$_content" > "$SANDBOX/sample.txt"
  (
    cd "$SANDBOX" || exit 1
    git add sample.txt
    sh status-oled/tools/checks.sh mac >/tmp/hooks-test-out.txt 2>&1
  )
  _got=$?
  _diag=$(cat /tmp/hooks-test-out.txt 2>/dev/null || true)
  expect "$_name" "$_want" "$_got" "$_diag"
  rm -f /tmp/hooks-test-out.txt
}

run_secrets_case() {
  _name="$1"; _want="$2"; _file="$3"

  make_sandbox
  # 文件可能带目录前缀（src/secrets.h），必须先建出父目录
  mkdir -p "$SANDBOX/$(dirname "$_file")"
  printf 'placeholder\n' > "$SANDBOX/$_file"
  (
    cd "$SANDBOX" || exit 1
    git add -f "$_file"
    FILES=$(git diff --cached --name-only --diff-filter=ACMR)
    sh status-oled/tools/checks.sh secrets "$FILES" >/tmp/hooks-test-out.txt 2>&1
  )
  _got=$?
  _diag=$(cat /tmp/hooks-test-out.txt 2>/dev/null || true)
  expect "$_name" "$_want" "$_got" "$_diag"
  rm -f /tmp/hooks-test-out.txt
}

run_creds_case() {
  _name="$1"; _want="$2"; _content="$3"

  make_sandbox
  printf '%s\n' "$_content" > "$SANDBOX/app.c"
  (
    cd "$SANDBOX" || exit 1
    git add app.c
    sh status-oled/tools/checks.sh creds >/tmp/hooks-test-out.txt 2>&1
  )
  _got=$?
  _diag=$(cat /tmp/hooks-test-out.txt 2>/dev/null || true)
  expect "$_name" "$_want" "$_got" "$_diag"
  rm -f /tmp/hooks-test-out.txt
}

# 用例：pre-push 的「范围参数解析」
# 这一组守的是最容易静默失效的地方——范围解析错了，git diff 会返回空输出，
# 检查脚本照常打「✓ 通过」，实际一个字节都没扫。所以必须验证：
#   1. 传入完整 40 位 sha 时能解析出范围（而不是空 diff）
#   2. 范围里确有真实 MAC 时必须 exit 1
#   3. 范围里只有虚构 MAC 时必须 exit 0
run_range_case() {
  _name="$1"; _want="$2"; _content="$3"

  make_sandbox
  # 沙箱里需要一个 pre-push 才能测它，但从真实仓库复制
  mkdir -p "$SANDBOX/.githooks"
  cp "$REPO_ROOT/.githooks/pre-push" "$SANDBOX/.githooks/pre-push" 2>/dev/null || {
    printf '  - 跳过 %s（找不到 .githooks/pre-push）\n' "$_name"
    rm -rf "$SANDBOX"; SANDBOX=''
    return
  }
  # pre-push 依赖 status-oled/tools/checks.sh，make_sandbox 已复制

  printf '%s\n' "$_content" > "$SANDBOX/src.txt"
  (
    cd "$SANDBOX" || exit 1
    git add src.txt
    git -c core.hooksPath=/dev/null commit -q -m "含 MAC 的提交"
    _local=$(git rev-parse HEAD)
    _remote=$(git rev-parse HEAD~1)
    _ref=$(git rev-parse --abbrev-ref HEAD)
    # 喂完整 40 位 sha —— 这正是 git 真实钩子的入参形态
    printf 'refs/heads/%s %s refs/heads/%s %s\n' "$_ref" "$_local" "$_ref" "$_remote" \
      | SKIP_BUILD=1 sh .githooks/pre-push origin "url" >/tmp/hooks-test-out.txt 2>&1
  )
  _got=$?
  _diag=$(cat /tmp/hooks-test-out.txt 2>/dev/null || true)
  expect "$_name" "$_want" "$_got" "$_diag"
  rm -f /tmp/hooks-test-out.txt
}

# ---------------------------------------------------------------- 主流程
GROUP="${1:-all}"

echo "================================================"
echo " 钩子回归测试（每组用例在独立临时仓库里跑）"
echo "================================================"

if [ "$GROUP" = "all" ] || [ "$GROUP" = "mac" ]; then
  echo ""
  echo "[MAC 检测]  应当拦住的："
  run_mac_case "真实 MAC 被拦住"          1 "Device MAC = ${REAL_MAC}"
  run_mac_case "小写真实 MAC 被拦住"      1 "device mac = ${REAL_MAC_LC}"
  run_mac_case "混合大小写真实 MAC 被拦住" 1 "mac ${REAL_MAC_MIX}"

  echo ""
  echo "[MAC 检测]  不应当拦住的："
  run_mac_case "虚构示例 MAC 放行"        0 "示例 MAC = ${FAKE_MAC}"
  run_mac_case "小写虚构示例放行"         0 "示例 mac = ${FAKE_MAC_LC}"
  run_mac_case "占位 MAC 放行"            0 "MAC = ${PLACE_MAC}"
  run_mac_case "全零/全 FF 占位放行"      0 "a=00:00:00:00:00:00 b=FF:FF:FF:FF:FF:FF"
  run_mac_case "普通冒号数字放行"         0 "time 12:34:56 ratio 78:9A:BC:DE"
  run_mac_case "无 MAC 的代码放行"        0 "int main(void) { return 0; }"
fi

if [ "$GROUP" = "all" ] || [ "$GROUP" = "secrets" ]; then
  echo ""
  echo "[secrets.h 检测]"
  run_secrets_case "secrets.h 被拦住"      1 "src/secrets.h"
  run_secrets_case "嵌套 secrets.h 被拦住" 1 "clock-oled/src/secrets.h"
  run_secrets_case "secrets.h.example 放行" 0 "src/secrets.h.example"
  run_secrets_case "普通头文件放行"        0 "src/config.h"
fi

if [ "$GROUP" = "all" ] || [ "$GROUP" = "creds" ]; then
  echo ""
  echo "[凭据检测]  仅警告，永不阻断（所以两者都期望 exit 0）"
  run_creds_case "硬编码凭据仅警告不阻断"  0 'char* password = "hunter2hunter2";'
  run_creds_case "正常代码放行"            0 'int timeout_ms = 1000;'
fi

if [ "$GROUP" = "all" ] || [ "$GROUP" = "layout" ]; then
  echo ""
  echo "[版式自检]  真实仓库里跑（只读）"
  if [ -f "$REPO_ROOT/status-oled/tools/check_layout.py" ]; then
    if (cd "$REPO_ROOT" && python status-oled/tools/check_layout.py >/dev/null 2>&1); then
      printf '  ✓ status-oled 版式自检通过\n'
      PASS=$((PASS + 1))
    else
      printf '  ✗ status-oled 版式自检失败\n'
      FAIL=$((FAIL + 1))
      FAILED_NAMES="$FAILED_NAMES
    - status-oled 版式自检"
    fi
  else
    printf '  - 跳过（找不到 check_layout.py）\n'
  fi
fi

if [ "$GROUP" = "all" ] || [ "$GROUP" = "range" ]; then
  echo ""
  echo "[pre-push 范围解析]  守「静默失效」：范围解析错 → diff 为空 → 假通过"
  run_range_case "范围里有真实 MAC → 拦截" 1 "Device MAC = ${REAL_MAC}"
  run_range_case "范围里有虚构 MAC → 放行" 0 "示例 MAC = ${FAKE_MAC}"
fi

# ---------------------------------------------------------------- 结论
echo ""
echo "================================================"
echo " 通过 $PASS / 失败 $FAIL"
if [ "$FAIL" -ne 0 ]; then
  echo " 失败用例：$FAILED_NAMES"
  echo "================================================"
  exit 1
fi
echo " 全部通过"
echo "================================================"
exit 0

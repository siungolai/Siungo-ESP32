// rlcd-workbench / 待办响应的切片器实现（M8，ADR-0020）

#include "todo_parse.h"

#include <stdlib.h>
#include <string.h>

namespace todojson {
namespace {

constexpr size_t kKeyCap = 24;

// 找到 "key" 后面那个值的起点。找不到返回 nullptr。
// 要求键名完整（前后都有引号）且冒号紧跟其后 —— 与 jsonfield 的口径一致：
// "schemaVersionX" 不算 "schemaVersion"。
const char *findValue(const char *json, const char *key) {
  if (json == nullptr || key == nullptr) return nullptr;
  char quoted[kKeyCap];
  const size_t n = strlen(key);
  if (n + 3 > sizeof(quoted)) return nullptr;
  quoted[0] = '"';
  memcpy(quoted + 1, key, n);
  quoted[n + 1] = '"';
  quoted[n + 2] = '\0';

  const char *p = json;
  while ((p = strstr(p, quoted)) != nullptr) {
    p += n + 2;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != ':') continue;  // 是某个字符串的内容，不是键
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
  }
  return nullptr;
}

// 从 v（指向 open 字符）开始，把配对的一段（含首尾括号）拷进 out。
// 字符串里的括号与转义不参与配对计数。
bool copyBalanced(const char *v, char open, char close, char *out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  if (v == nullptr || *v != open) return false;

  int depth = 0;
  bool inStr = false;
  bool esc = false;
  size_t n = 0;
  for (const char *p = v; *p != '\0'; ++p) {
    const char c = *p;
    if (inStr) {
      if (esc) {
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        inStr = false;
      }
    } else if (c == '"') {
      inStr = true;
    } else if (c == open) {
      ++depth;
    } else if (c == close) {
      --depth;
      if (depth == 0) {
        // 收口：这一整段要装得下，否则算失败（半截 JSON 比没有更坏）。
        if (n + 2 > cap) return false;
        out[n++] = c;
        out[n] = '\0';
        return true;
      }
    }
    if (n + 1 >= cap) return false;
    out[n++] = c;
  }
  out[0] = '\0';
  return false;
}

}  // namespace

bool object(const char *json, const char *key, char *out, size_t cap) {
  const char *v = findValue(json, key);
  return copyBalanced(v, '{', '}', out, cap);
}

bool array(const char *json, const char *key, char *out, size_t cap) {
  const char *v = findValue(json, key);
  return copyBalanced(v, '[', ']', out, cap);
}

bool nextObject(const char *arr, size_t *pos, char *out, size_t cap) {
  if (arr == nullptr || pos == nullptr || out == nullptr || cap == 0) return false;
  size_t i = *pos;
  // array() 返回的切片是带外层方括号的，所以 '[' 也要当分隔符跳过 ——
  // 漏了这一条，第一个对象永远找不到，items 会静默变成 0 条。
  while (arr[i] == ' ' || arr[i] == ',' || arr[i] == '[' || arr[i] == '\r' ||
         arr[i] == '\n' || arr[i] == '\t') {
    ++i;
  }
  if (arr[i] != '{') return false;

  // 复用 copyBalanced：从当前位置切一个对象出来，再把 pos 推到它后面。
  char *tmp = out;
  if (!copyBalanced(arr + i, '{', '}', tmp, cap)) return false;
  const size_t used = strlen(tmp);
  *pos = i + used;
  return true;
}

bool nextInt(const char *arr, size_t *pos, long *out) {
  if (arr == nullptr || pos == nullptr || out == nullptr) return false;
  size_t i = *pos;
  while (arr[i] == ' ' || arr[i] == ',' || arr[i] == '[' || arr[i] == '\r' ||
         arr[i] == '\n' || arr[i] == '\t') {
    ++i;
  }
  if (arr[i] == ']' || arr[i] == '\0') return false;
  char *end = nullptr;
  const long v = strtol(arr + i, &end, 10);
  if (end == arr + i) return false;
  *pos = (size_t)(end - arr);
  *out = v;
  return true;
}

}  // namespace todojson

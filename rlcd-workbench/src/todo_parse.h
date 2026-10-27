// rlcd-workbench / 待办响应里「切一块出来」的最小切片器（M8，ADR-0020）
//
// jsonfield.h 只认扁平对象里的字符串与数字，而 /api/todo-device 的响应是
// 三层：顶层标量 + stats 对象 + items 对象数组 + （可选）trips 对象。
//
// 这里不写通用 JSON 解析器，只做一件事：把某个键对应的对象/数组原文切出来，
// 交给 jsonfield 去读里面的扁平字段。切片器不认识类型、不分配内存、不建树。
//
// 为什么不是「改 jsonfield 支持嵌套」：jsonfield 的注释写着「要嵌套就先让
// 服务端压平」——那是 2026-10 的结论，代价是整个扫描器要重写成递归下降。
// 40 行切片器 + 复用现有扫描器，等价且不动已经过 A3 验收的那份代码。
//
// 已知边界：切片器假设输入是合法 JSON（服务端 Go 的 encoding/json 保证）。
// 它只负责「别被字符串里的括号和转义骗到」，不负责报语法错误。

#pragma once

#include <stddef.h>

namespace todojson {

// 把 key 对应的**对象**原文（含花括号）切进 out。没有这个键、或值不是对象，
// 返回 false 且 out 为空串。cap 不够装整段时也返回 false —— 半截 JSON 比没有更坏。
bool object(const char *json, const char *key, char *out, size_t cap);

// 同上，切的是**数组**（含方括号）。
bool array(const char *json, const char *key, char *out, size_t cap);

// arr 形如 [{...},{...}]。pos 由调用方持有（初值 0），每成功取到一个对象就
// 前进到它后面；数组读完返回 false。
bool nextObject(const char *arr, size_t *pos, char *out, size_t cap);

// arr 形如 [22,24,27]。逐个取整数；读到 ']' 返回 false。
bool nextInt(const char *arr, size_t *pos, long *out);

}  // namespace todojson

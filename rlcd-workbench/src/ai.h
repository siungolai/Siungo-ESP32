// rlcd-workbench / AI 通路（本期不实现）
//
// 这个文件里只有接口与状态枚举，**没有一行实现**。这是设计决定，不是进度落后：
// AI 对话面板本期连版面设计都不做（见 docs/策划案.md §4.5），也就没有哪个界面
// 元素会去消费这些函数。写它的唯一目的，是让"将来要接的时候边界在哪"有一个能被
// 编译器检查的说法，而不是几个月后从聊天记录里翻。
//
// 真要动手时的顺序（现在一步都还没走）：
//   1. 服务器侧先得有一个能收音频帧的端点 —— 当前 workbuddy-gateway 只有
//      OpenAI 兼容的 HTTP/SSE，没有 ASR、没有 TTS、没有 WebSocket；
//   2. 设备侧把 ES8311 / ES7210 用 esp_codec_dev 接上（依赖 IDF 5 的 i2c_master，
//      平台已就位，见 docs/策划案.md §9「明确不做」）；
//   3. 最后才是这里的状态机，以及面板三的版面。
//
// 顺序反过来做会得到一个能录音、能说话、但没人在另一头听的设备。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ai {

// 与文档里那张状态图同构。Disabled 不是错误态，是"没有配置通路"——
// 屏上要能区分"没接"和"接不上"。
enum class State : uint8_t { Idle = 0, Connecting, Listening, Speaking, Disabled };

void begin();
void tick();
State state();
const char *stateName();

// 上行一帧 PCM。返回 false 表示这一帧没送出去（未连接 / 队列满）。
bool sendAudioFrame(const int16_t *pcm, size_t samples);

// 下行文本回调。final=false 是增量，true 表示这一轮说完。
// 页尾那个"当前页 / 总页数"要等这里接上才有东西可数。
using TextHandler = void (*)(const char *text, bool final);
void setTextHandler(TextHandler handler);

// 页尾页码的来源。本期恒为 0 / 0，页尾因此不显示页码。
int conversationPages();
int conversationPage();

}  // namespace ai
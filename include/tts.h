// ============================================================
// tts.h  云端语音合成：把一句话变成耳机里的声音
//
// 和 ai.h 那几个函数一样是同步阻塞的，只能在语音线程里调。
// 放进录音线程会把采样卡住。
// ============================================================

#ifndef TTS_H
#define TTS_H

#include "httpclient.h"

// 合成走 DashScope 原生接口，不是 compatible-mode。
// 这套网关上 compatible-mode 的 /audio/speech 是 404，
// 只有原生这条返回音频链接
#define AI_TTS_URL \
	"https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
#define AI_MODEL_TTS "qwen3-tts-flash"

// 进程启动时调一次：把放音通路切到耳机
int tts_init(void);

// 念一句话。成功 0，失败 -1 并打印原因
int tts_say(const struct ai_config *cfg, const char *text);

#endif
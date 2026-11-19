// ============================================================
// ai.h  云端能力：语音识别、看图回答、纯文本问答
//
// 三个函数都走 OpenAI 兼容的 /chat/completions，区别只在 content 的构造：
//   纯文本   content 是字符串
//   看图     content 是数组，含 image_url（JPEG 转 base64 data URI）
//   听音频   content 是数组，含 input_audio（wav 转 base64）
//
// 全部同步阻塞。调用者负责放在独立线程里，别堵住录音线程。
// ============================================================

#ifndef AI_H
#define AI_H

#include "httpclient.h"
#include <stddef.h>

// 各任务的默认模型，可以被 cfg.model 覆盖
//
// 语音识别用 omni 而不是专门的 asr 模型：
// qwen-audio-3.0-asr-flash 走 chat/completions 时会返回
// UNSUPPORTED_FORMAT（format is empty），它要的是另一套接口；
// omni 系列原生支持 input_audio，实测可用
#define AI_MODEL_VLM  "qwen3-vl-plus"
#define AI_MODEL_ASR  "qwen3-omni-flash"
#define AI_MODEL_TEXT "qwen3-vl-plus"

// 纯文本问答
int ai_ask_text(const struct ai_config *cfg, const char *model,
		const char *question, char *answer, size_t answer_size);

// 看图回答：jpeg 是内存里的原始 JPEG 字节
int ai_ask_image(const struct ai_config *cfg, const char *model,
		 const char *question,
		 const unsigned char *jpeg, size_t jpeg_len,
		 char *answer, size_t answer_size);

// 语音识别：pcm 是 16 位单声道 16000Hz 的采样，函数内部拼 wav 头。
// 走 chat/completions 的 input_audio（部分平台支持）
int ai_ask_audio(const struct ai_config *cfg, const char *model,
		 const short *pcm, size_t frames,
		 char *answer, size_t answer_size);

// 语音识别（文件上传版）：把整个 wav 走 multipart 传到 /audio/transcriptions。
// 阿里云不认 chat/completions 里的音频，这条路才是通用的
int ai_transcribe(const struct ai_config *cfg, const char *model,
		  const unsigned char *wav, size_t wav_len,
		  char *answer, size_t answer_size);

// base64 编码，返回 malloc 出来的字符串，调用者 free
// 单独暴露出来是因为看图那条路要把 45KB 的 JPEG 编码成 60KB 文本，
// 内存占用要看得见，不能藏在某处悄悄分配
char *ai_base64(const unsigned char *data, size_t len);

#endif
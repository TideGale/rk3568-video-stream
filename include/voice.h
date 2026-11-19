// ============================================================
// voice.h  语音交互：把录音段落变成文字，再变成动作或回答
//
// 数据流：
//   audio 线程检测到人声 → voice_feed 投递 → voice 线程
//     → ASR 转文字 → 判断意图
//        ├─ 含"拍"    → 抓最新帧存成文件，回答"已拍一张"
//        └─ 其他      → 抓最新帧送视觉模型，把答案念给用户
//
// voice 线程自己一条，不跟录音线程抢时间。识别慢的时候录音照常进行。
// ============================================================

#ifndef VOICE_H
#define VOICE_H

#include "httpclient.h"
#include <stddef.h>

// 拍照落盘目录
#define VOICE_SHOT_DIR "/root"

// 初始化：传入已加载好的密钥配置
int  voice_init(const struct ai_config *cfg);
void voice_deinit(void);

// pthread 入口，arg 传 volatile int * 的停止标志
void *voice_thread(void *arg);

// 录音线程回调里调这个，只做拷贝入队，立刻返回
void voice_feed(const short *pcm, size_t frames);

// 给 /status 用：取最近一次的提问和回答
void voice_last(char *ask, size_t ask_size, char *answer, size_t answer_size);

#endif
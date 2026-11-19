// ============================================================
// audio.h  音频采集的对外接口
//
// 两种用法：
//   1. 单次录音：audio_record，测设备时用
//   2. 常开线程：audio_init + audio_thread + audio_deinit，项目里用这个
// ============================================================

#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>

// 录音设备与落盘目录
// 板载 codec 只支持 2 到 8 声道，所以必须用 plughw 让插件层转成单声道
#define AUDIO_DEV "plughw:0,0"
#define AUDIO_DIR "/root/audio_rec"

// 录音结果，给调用者判断要不要送去识别
struct audio_result
{
	int    peak;            // 归一化前的峰值，0 表示整段是静音
	double rms;             // 归一化前的均方根，用来判断信噪比
	double gain;            // 实际用的归一化系数
	size_t samples;         // 采到的采样点数
};

// 单次录音，测设备时用
int audio_record(const char *dev, int seconds, const char *path,
		 struct audio_result *res);

// 常开录音线程：先 init 一次，再 create 线程，退出前 deinit
int  audio_init(const char *dev, const char *dir);
void audio_deinit(void);

// pthread 入口，arg 传 volatile int * 的停止标志
void *audio_thread(void *arg);

// 有人声时的回调，参数是归一化之后的一段 PCM。
// 回调里必须很快返回（只做拷贝和投递），不能在里面发网络请求，
// 否则录音线程被阻塞，后面的段就串了。
typedef void (*audio_voice_cb)(const short *pcm, size_t frames);
void audio_set_voice_cb(audio_voice_cb cb);

#endif
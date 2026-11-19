// ============================================================
// audio.c  ALSA 音频采集
//
// 采样率 16000、单声道、16 位小端，这是语音识别最通用的格式。
// 硬件只支持 2 到 8 声道，所以设备名必须用 plughw，
// 靠 ALSA 的插件层把单声道转成多声道再送进 codec。
// ============================================================

#include "audio.h"
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#define AUD_RATE     16000              // 采样率，语音识别的标准值
#define AUD_CHANNELS 1                  // 逻辑声道数，硬件那边由 plughw 转
#define AUD_BITS     16
#define AUD_PERIOD   1600               // 一周期 1600 帧 = 100 毫秒
#define AUD_GAIN_MAX 16.0               // 归一化系数上限，防止把底噪放大成噪声
#define AUD_TARGET   19660              // 目标峰值，约满量程的 60%

// 写 44 字节标准 wav 头
static int wav_header(FILE *fp, unsigned int rate, unsigned short ch,
		      unsigned short bits, unsigned int datalen)
{
	unsigned int   byterate   = rate * ch * bits / 8;
	unsigned short blockalign = (unsigned short)(ch * bits / 8);
	unsigned int   riff       = 36 + datalen;
	unsigned int   fmtlen     = 16;
	unsigned short pcm        = 1;      // 1 表示未压缩 PCM

	if (fwrite("RIFF", 1, 4, fp) != 4)       return -1;
	if (fwrite(&riff, 4, 1, fp) != 1)        return -1;
	if (fwrite("WAVEfmt ", 1, 8, fp) != 8)   return -1;
	if (fwrite(&fmtlen, 4, 1, fp) != 1)      return -1;
	if (fwrite(&pcm, 2, 1, fp) != 1)         return -1;
	if (fwrite(&ch, 2, 1, fp) != 1)          return -1;
	if (fwrite(&rate, 4, 1, fp) != 1)        return -1;
	if (fwrite(&byterate, 4, 1, fp) != 1)    return -1;
	if (fwrite(&blockalign, 2, 1, fp) != 1)  return -1;
	if (fwrite(&bits, 2, 1, fp) != 1)        return -1;
	if (fwrite("data", 1, 4, fp) != 4)       return -1;
	if (fwrite(&datalen, 4, 1, fp) != 1)     return -1;

	return 0;
}

int audio_record(const char *dev, int seconds, const char *path,
		 struct audio_result *res)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *params = NULL;
	short *buf = NULL;
	short *all = NULL;
	int err = 0;
	int ret = -1;

	if (res != NULL)
		memset(res, 0, sizeof(*res));

	// 打开采集设备。SND_PCM_STREAM_CAPTURE 表示录音，不是放音
	err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0)
	{
		fprintf(stderr, "打开设备失败: %s\n", snd_strerror(err));
		return -1;
	}

	snd_pcm_hw_params_alloca(&params);

	// hw_params 是「跟驱动商量参数」，一次填不完，分四步
	if ((err = snd_pcm_hw_params_any(pcm, params)) < 0)
		goto done;
	if ((err = snd_pcm_hw_params_set_access(pcm, params,
			SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		goto done;
	if ((err = snd_pcm_hw_params_set_format(pcm, params,
			SND_PCM_FORMAT_S16_LE)) < 0)
		goto done;
	if ((err = snd_pcm_hw_params_set_channels(pcm, params,
			AUD_CHANNELS)) < 0)
		goto done;

	unsigned int rate = AUD_RATE;
	if ((err = snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0)) < 0)
		goto done;

	// 周期设 100 毫秒：太短会让系统调用太频繁，太长会让录音启动有明显延迟
	snd_pcm_uframes_t period = AUD_PERIOD;
	snd_pcm_hw_params_set_period_size_near(pcm, params, &period, 0);

	// 这一步才真正把参数写进驱动
	if ((err = snd_pcm_hw_params(pcm, params)) < 0)
		goto done;

	// 打印驱动实际给了什么，hw_params 是商量不是命令
	printf("[AUD] 设备 %s\n", dev);
	printf("[AUD] 实际采样率 %u, 周期 %lu 帧 (%.0f ms)\n",
	       rate, (unsigned long)period, period * 1000.0 / rate);

	buf = malloc(period * sizeof(short));
	if (buf == NULL)
		goto done;

	size_t total = (size_t)rate * seconds;
	all = malloc(total * sizeof(short));
	if (all == NULL)
		goto done;

	size_t got = 0;
	while (got < total)
	{
		// 一次读一个周期。readi 的返回值是「读到几帧」，不是字节
		snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, period);

		if (n == -EPIPE)
		{
			// 溢出：上层读得太慢，数据被覆盖。清掉重来，不退出
			fprintf(stderr, "溢出，重新同步\n");
			snd_pcm_prepare(pcm);
			continue;
		}
		if (n < 0)
		{
			fprintf(stderr, "读失败: %s\n", snd_strerror((int)n));
			snd_pcm_recover(pcm, (int)n, 1);
			continue;
		}
		if (n == 0)
			continue;

		size_t room = total - got;
		size_t take = (size_t)n < room ? (size_t)n : room;
		memcpy(&all[got], buf, take * sizeof(short));
		got += take;
	}

	// ---------- 归一化 ----------
	// 板载 codec 没有增益控件，只能在软件里补
	int peak = 0;
	double sum = 0.0;

	for (size_t i = 0; i < got; i++)
	{
		int v = all[i] < 0 ? -all[i] : all[i];
		if (v > peak)
			peak = v;
		sum += (double)all[i] * all[i];
	}

	double rms  = got > 0 ? sqrt(sum / got) : 0.0;
	double gain = 1.0;

	if (peak > 0)
	{
		gain = (double)AUD_TARGET / peak;
		if (gain > AUD_GAIN_MAX)
			gain = AUD_GAIN_MAX;    // 全静音时不放大底噪
	}

	for (size_t i = 0; i < got; i++)
	{
		double v = all[i] * gain;
		if (v >  32767.0) v =  32767.0;     // 削波保护
		if (v < -32768.0) v = -32768.0;
		all[i] = (short)(v >= 0 ? v + 0.5 : v - 0.5);
	}

	printf("[AUD] 采到 %zu 帧 (%.2f 秒)\n", got, (double)got / rate);
	printf("[AUD] 峰值 %d, RMS %.1f, 归一化 %.2f 倍\n", peak, rms, gain);

	// ---------- 存 wav ----------
	FILE *fp = fopen(path, "wb");
	if (fp == NULL)
	{
		perror("fopen");
		goto done;
	}

	unsigned int datalen = (unsigned int)(got * sizeof(short));
	if (wav_header(fp, rate, AUD_CHANNELS, AUD_BITS, datalen) < 0)
	{
		fclose(fp);
		goto done;
	}

	if (fwrite(all, 1, datalen, fp) != datalen)
	{
		fclose(fp);
		goto done;
	}

	fclose(fp);
	printf("[AUD] 已写入 %s, %u 字节\n", path, datalen + 44);

	if (res != NULL)
	{
		res->peak    = peak;
		res->rms     = rms;
		res->gain    = gain;
		res->samples = got;
	}

	ret = 0;

done:
	if (ret != 0 && err < 0)
		fprintf(stderr, "失败: %s\n", snd_strerror(err));

	free(buf);
	free(all);
	if (pcm != NULL)
		snd_pcm_close(pcm);

	return ret;
}

// ============================================================
//                 常开录音线程（第 4 步）
// ============================================================

#define AUD_SEG_SEC    3                // 一段 3 秒，语音识别的常用短句长度
// 峰值低于这个数当静音，不归一化也不落盘。
// 实测这块板子的麦克风很灵敏，安静时峰值就有上千（RMS 177 左右），
// 300 会把环境噪声全当成说话，一直往云端发“嗯”。提到 900 之后
// 只有真开口才触发，说话音量的峰值通常在 2000 以上
#define AUD_SPEECH_MIN 900

static char g_audio_dev[64];
static char g_audio_dir[128];
static audio_voice_cb g_voice_cb = NULL;

void audio_set_voice_cb(audio_voice_cb cb)
{
	g_voice_cb = cb;
}

// 打开采集设备并配好参数，线程和单次录音共用
static snd_pcm_t *pcm_open_capture(const char *dev)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *params = NULL;
	unsigned int rate = AUD_RATE;
	snd_pcm_uframes_t period = AUD_PERIOD;
	int err;

	err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0)
	{
		fprintf(stderr, "[AUD] 打开 %s 失败: %s\n", dev, snd_strerror(err));
		return NULL;
	}

	snd_pcm_hw_params_alloca(&params);

	if ((err = snd_pcm_hw_params_any(pcm, params)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_access(pcm, params,
			SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_format(pcm, params,
			SND_PCM_FORMAT_S16_LE)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_channels(pcm, params,
			AUD_CHANNELS)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0)) < 0)
		goto fail;

	snd_pcm_hw_params_set_period_size_near(pcm, params, &period, 0);

	if ((err = snd_pcm_hw_params(pcm, params)) < 0)
		goto fail;

	return pcm;

fail:
	fprintf(stderr, "[AUD] 配置参数失败: %s\n", snd_strerror(err));
	snd_pcm_close(pcm);
	return NULL;
}

// 把一段 PCM 写成 wav，文件名带时间戳，事后能按时间找
static int save_segment(short *pcm, size_t frames)
{
	struct timespec ts;
	struct tm tm;
	char path[256];

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);

	snprintf(path, sizeof(path), "%s/a_%04d%02d%02d_%02d%02d%02d.wav",
		 g_audio_dir,
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);

	FILE *fp = fopen(path, "wb");
	if (fp == NULL)
	{
		perror("[AUD] fopen");
		return -1;
	}

	unsigned int datalen = (unsigned int)(frames * sizeof(short));
	if (wav_header(fp, AUD_RATE, AUD_CHANNELS, AUD_BITS, datalen) < 0)
	{
		fclose(fp);
		return -1;
	}

	if (fwrite(pcm, 1, datalen, fp) != datalen)
	{
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return 0;
}

// 扫峰值和 RMS，不是静音才归一化。返回 0 表示这段没人声
static int normalize(short *pcm, size_t frames, struct audio_result *res)
{
	int peak = 0;
	double sum = 0.0;

	for (size_t i = 0; i < frames; i++)
	{
		int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
		if (v > peak)
			peak = v;
		sum += (double)pcm[i] * pcm[i];
	}

	double rms = frames > 0 ? sqrt(sum / frames) : 0.0;

	res->peak    = peak;
	res->rms     = rms;
	res->gain    = 1.0;
	res->samples = frames;

	// 静音就不放大。否则底噪被乘 16 倍，听起来是沙沙声，识别也会被误触发
	if (peak < AUD_SPEECH_MIN)
		return 0;

	double gain = (double)AUD_TARGET / peak;
	if (gain > AUD_GAIN_MAX)
		gain = AUD_GAIN_MAX;

	for (size_t i = 0; i < frames; i++)
	{
		double v = pcm[i] * gain;
		if (v >  32767.0) v =  32767.0;
		if (v < -32768.0) v = -32768.0;
		pcm[i] = (short)(v >= 0 ? v + 0.5 : v - 0.5);
	}

	res->gain = gain;
	return 1;
}

int audio_init(const char *dev, const char *dir)
{
	snprintf(g_audio_dev, sizeof(g_audio_dev), "%s", dev);
	snprintf(g_audio_dir, sizeof(g_audio_dir), "%s", dir);

	// 目录不存在就建，已存在也当成功
	mkdir(dir, 0755);

	return 0;
}

void audio_deinit(void)
{
	// 线程内部自己关设备，这里没有要释放的全局资源
}

void *audio_thread(void *arg)
{
	volatile int *stop = (volatile int *)arg;

	snd_pcm_t *pcm = pcm_open_capture(g_audio_dev);
	if (pcm == NULL)
		return NULL;

	pthread_setname_np(pthread_self(), "audio");

	size_t seg_frames = (size_t)AUD_RATE * AUD_SEG_SEC;
	short *seg = malloc(seg_frames * sizeof(short));
	short *buf = malloc(AUD_PERIOD * sizeof(short));

	if (seg == NULL || buf == NULL)
	{
		fprintf(stderr, "[AUD] 内存不足\n");
		free(seg);
		free(buf);
		snd_pcm_close(pcm);
		return NULL;
	}

	fprintf(stderr, "[AUD] 录音线程启动，设备 %s，每段 %d 秒，阈值 %d\n",
		g_audio_dev, AUD_SEG_SEC, AUD_SPEECH_MIN);

	unsigned long seg_count = 0;
	unsigned long saved = 0;
	int err_cnt = 0;
	unsigned long silent = 0;

	while (*stop == 0)
	{
		size_t got = 0;

		// 录满一整段再判断。退出信号随时可能来，所以内层也要查
		while (got < seg_frames && *stop == 0)
		{
			snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, AUD_PERIOD);

			if (n == -EPIPE)
			{
				if (err_cnt < 5)
					fprintf(stderr, "[AUD] 采集溢出(EPIPE)，重新同步\n");
				err_cnt++;
				snd_pcm_prepare(pcm);       // 溢出，重新同步后继续
				continue;
			}
			if (n < 0)
			{
				// 前几次全打出来，之后每 100 次打一次。
				// 静默 continue 会让线程空转而看不出任何异常
				if (err_cnt < 5 || err_cnt % 100 == 0)
					fprintf(stderr, "[AUD] 读失败: %s (累计 %d 次)\n",
						snd_strerror((int)n), err_cnt);
				err_cnt++;
				snd_pcm_recover(pcm, (int)n, 1);
				continue;
			}
			if (n == 0)
				continue;

			size_t room = seg_frames - got;
			size_t take = (size_t)n < room ? (size_t)n : room;
			memcpy(&seg[got], buf, take * sizeof(short));
			got += take;
		}

		// 退出时没录满的那一段扔掉，半截数据没有意义
		if (*stop != 0)
			break;

		seg_count++;

		struct audio_result r;
		if (normalize(seg, got, &r) == 0)
		{
			// 静音不落盘，但要能看到"读到了多少"。否则线程空转时无从判断
			silent++;
			if (seg_count <= 3 || seg_count % 20 == 0)
				fprintf(stderr,
					"[AUD] 第 %lu 段静音: 峰值 %d（阈值 %d），累计静音 %lu 段\n",
					seg_count, r.peak, AUD_SPEECH_MIN, silent);
			continue;
		}

		if (save_segment(seg, got) == 0)
		{
			saved++;
			fprintf(stderr,
				"[AUD] 第 %lu 段有人声: 峰值 %d, RMS %.1f, 系数 %.2f, 已存 %lu\n",
				seg_count, r.peak, r.rms, r.gain, saved);
		}

		// 交给语音线程去识别。回调只做拷贝入队，立刻返回，
		// 录音线程不能被网络请求阻塞
		if (g_voice_cb != NULL)
			g_voice_cb(seg, got);
	}

	fprintf(stderr, "[AUD] 录音线程退出，共判 %lu 段, 存了 %lu 段\n",
		seg_count, saved);

	free(seg);
	free(buf);
	snd_pcm_drop(pcm);                      // 丢掉驱动里还没读完的数据
	snd_pcm_close(pcm);

	return NULL;
}

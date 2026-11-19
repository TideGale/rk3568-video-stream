// ============================================================
// voice.c  语音交互：录音 → 识别 → 意图 → 动作或回答
//
// 三个设计点：
//   1. 单槽邮箱。录音线程投递，语音线程取走；投递时如果上一条还没处理，
//      直接覆盖——用户连着说两句，前一句作废，比排队积压合理。
//   2. 关键路径上不 sleep。用条件变量等新数据，有活才醒。
//   3. 所有 AI 调用都在本线程，录音线程只管采样。
// ============================================================

#include "voice.h"
#include "ai.h"
#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

// 邮箱容量：一段最多 3 秒，16000 * 3 = 48000 个采样，留点余量
#define VOICE_PCM_MAX (16000 * 5)

// 意图关键词。用简单的子串匹配，不上模型——
// 指令类的话必须又快又准，模型会犹豫，字符串不会
static const char *KW_SHOT[] = { "拍", "照片", "截图" };
static const char *KW_STATUS[] = { "帧率", "状态", "多少帧", "丢帧", "跑得" };

static struct ai_config g_cfg;
static int g_inited = 0;

// ---- 单槽邮箱 ----
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static short  g_pcm[VOICE_PCM_MAX];
static size_t g_frames = 0;
static int    g_ready = 0;
static unsigned long g_dropped = 0;     // 被覆盖掉的段数，诊断用

// ---- 最近一次问答，给 /status 看 ----
static char g_last_ask[256];
static char g_last_ans[1024];

int voice_init(const struct ai_config *cfg)
{
	if (cfg == NULL)
		return -1;

	g_cfg = *cfg;
	g_inited = 1;

	fprintf(stderr, "[VOICE] 语音模块就绪，识别模型 %s，视觉模型 %s\n",
		AI_MODEL_ASR, AI_MODEL_VLM);

	return 0;
}

void voice_deinit(void)
{
	g_inited = 0;
}

void voice_feed(const short *pcm, size_t frames)
{
	if (pcm == NULL || frames == 0)
		return;

	if (frames > VOICE_PCM_MAX)
		frames = VOICE_PCM_MAX;

	pthread_mutex_lock(&g_lock);

	// 上一段还没被处理完就覆盖。连续说话时只保留最新一句
	if (g_ready)
		g_dropped++;

	memcpy(g_pcm, pcm, frames * sizeof(short));
	g_frames = frames;
	g_ready = 1;

	pthread_cond_signal(&g_cond);
	pthread_mutex_unlock(&g_lock);
}

void voice_last(char *ask, size_t ask_size, char *answer, size_t answer_size)
{
	pthread_mutex_lock(&g_lock);
	if (ask != NULL && ask_size > 0)
		snprintf(ask, ask_size, "%s", g_last_ask);
	if (answer != NULL && answer_size > 0)
		snprintf(answer, answer_size, "%s", g_last_ans);
	pthread_mutex_unlock(&g_lock);
}

static int contains_any(const char *text, const char *const *words, int n)
{
	for (int i = 0; i < n; i++)
	{
		if (strstr(text, words[i]) != NULL)
			return 1;
	}
	return 0;
}

// 抓一帧存成文件，返回 0 成功。文件名带帧序号，不覆盖
static int save_shot(char *path_buf, size_t path_size)
{
	unsigned char *jpeg = NULL;
	size_t len = 0;
	unsigned long seq = 0;

	if (net_get_frame(&jpeg, &len, &seq) != 0)
	{
		fprintf(stderr, "[VOICE] 还没有采到帧，拍不了\n");
		return -1;
	}

	snprintf(path_buf, path_size, VOICE_SHOT_DIR "/shot_%lu.jpg", seq);

	FILE *fp = fopen(path_buf, "wb");
	if (fp == NULL)
	{
		perror("[VOICE] fopen");
		free(jpeg);
		return -1;
	}

	if (fwrite(jpeg, 1, len, fp) != len)
	{
		fclose(fp);
		free(jpeg);
		return -1;
	}

	fclose(fp);
	free(jpeg);

	fprintf(stderr, "[VOICE] 已拍照 %s（%zu 字节，帧号 %lu）\n",
		path_buf, len, seq);

	return 0;
}

// 状态播报：把 /status 的 JSON 转成一句人话。
// 不念 JSON 是因为念出来没人听得懂，只挑三个关键数字
static void build_status_speech(char *out, size_t out_size)
{
	char json[256];
	net_format_status(json, sizeof(json));

	double fps = 0.0;
	unsigned long frames = 0, drop_s = 0, drop_p = 0;
	long rss = 0, uptime = 0;

	// 用 sscanf 而不是上 cJSON：格式是我们自己定的，位置固定，
	// 而且这里不能失败——念数字这种事不该依赖一次 JSON 解析
	sscanf(json,
	       "{\"fps\":%lf,\"frames\":%lu,\"drop_stream\":%lu,"
	       "\"drop_preview\":%lu,\"clients\":%*d,\"uptime\":%ld,\"rss\":%ld}",
	       &fps, &frames, &drop_s, &drop_p, &uptime, &rss);

	snprintf(out, out_size,
		 "现在 %.0f 帧每秒，一共采了 %lu 帧，推流丢了 %lu 帧，内存 %ld KB，"
		 "已经跑了 %ld 分钟",
		 fps, frames, drop_s, rss, uptime / 60);
}


// 识别结果里那些没意义的东西：语气词、单字、纯标点。
// 环境噪声会被识别成“嗯”“啊”，这类不该触发看图
static int is_meaningless(const char *text)
{
	static const char *filler[] = {
		"嗯", "呃", "啊", "哦", "恩", "唉", "哎", "喂",
		"嗯嗯", "啊啊", "呃呃", "那个", "这个", "然后"
	};

	size_t n = strlen(text);

	// 去掉标点和空格后不足 3 个字节（中文一个字 3 字节），基本没内容
	size_t solid = 0;
	for (size_t i = 0; i < n; i++)
	{
		if (text[i] != ' ' && text[i] != '。' && text[i] != '.' &&
		    text[i] != '，' && text[i] != ',' && text[i] != '？' &&
		    text[i] != '?' && text[i] != '!' && text[i] != '！' &&
		    text[i] != '\n' && text[i] != '\r')
			solid++;
	}
	if (solid < 3)
		return 1;

	for (size_t i = 0; i < sizeof(filler) / sizeof(filler[0]); i++)
	{
		if (strcmp(text, filler[i]) == 0)
			return 1;
	}

	return 0;
}

// 一条完整的处理流程：PCM → 文字 → 意图 → 动作
static void handle_segment(const short *pcm, size_t frames)
{
	char text[1024];

	// 第一段：语音转文字
	if (ai_ask_audio(&g_cfg, AI_MODEL_ASR, pcm, frames,
			 text, sizeof(text)) != 0)
	{
		fprintf(stderr, "[VOICE] 识别失败，这段丢掉\n");
		return;
	}

	// 去掉首尾空白和换行，模型偶尔会在回答后面带换行
	size_t n = strlen(text);
	while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r' ||
			 text[n - 1] == ' '))
		text[--n] = '\0';

	if (n == 0)
		return;

	if (is_meaningless(text))
	{
		// 噪声被识别成语气词，静默丢掉，不上云也不回答
		fprintf(stderr, "[VOICE] 忽略无意义识别: %s\n", text);
		return;
	}

	fprintf(stderr, "[VOICE] 听到: %s\n", text);

	pthread_mutex_lock(&g_lock);
	snprintf(g_last_ask, sizeof(g_last_ask), "%s", text);
	pthread_mutex_unlock(&g_lock);

	// 第二段：判断意图
	if (contains_any(text, KW_SHOT, (int)(sizeof(KW_SHOT) / sizeof(KW_SHOT[0]))))
	{
		char path[128];
		const char *ans;

		if (save_shot(path, sizeof(path)) == 0)
			ans = "已拍一张";
		else
			ans = "还没采到画面，拍不了";

		pthread_mutex_lock(&g_lock);
		snprintf(g_last_ans, sizeof(g_last_ans), "%s", ans);
		pthread_mutex_unlock(&g_lock);

		fprintf(stderr, "[VOICE] 回答: %s\n", ans);
		return;
	}

	// 指令类暂时只做拍照和状态播报，念出来留给下一步
	if (contains_any(text, KW_STATUS, (int)(sizeof(KW_STATUS) / sizeof(KW_STATUS[0]))))
	{
		char speech[512];
		build_status_speech(speech, sizeof(speech));

		pthread_mutex_lock(&g_lock);
		snprintf(g_last_ans, sizeof(g_last_ans), "%s", speech);
		pthread_mutex_unlock(&g_lock);

		fprintf(stderr, "[VOICE] 回答: %s\n", speech);
		return;
	}

	// 第三段：既不是指令，就当问题——抓帧送视觉模型
	unsigned char *jpeg = NULL;
	size_t len = 0;

	if (net_get_frame(&jpeg, &len, NULL) != 0)
	{
		const char *ans = "还没采到画面，看不到东西";

		pthread_mutex_lock(&g_lock);
		snprintf(g_last_ans, sizeof(g_last_ans), "%s", ans);
		pthread_mutex_unlock(&g_lock);

		fprintf(stderr, "[VOICE] 回答: %s\n", ans);
		return;
	}

	char answer[1024];

	fprintf(stderr, "[VOICE] 送图给 %s，图片 %zu 字节\n", AI_MODEL_VLM, len);

	// 加一句限制：这些回答后面要念出来，长了一句要念二十秒
	char question[640];
	snprintf(question, sizeof(question),
		 "%s（请用一句不超过三十个字的话回答，只说看到的东西）", text);

	int rc = ai_ask_image(&g_cfg, AI_MODEL_VLM, question, jpeg, len,
			      answer, sizeof(answer));
	free(jpeg);

	if (rc != 0)
	{
		fprintf(stderr, "[VOICE] 看图失败\n");
		return;
	}

	pthread_mutex_lock(&g_lock);
	snprintf(g_last_ans, sizeof(g_last_ans), "%s", answer);
	pthread_mutex_unlock(&g_lock);

	fprintf(stderr, "[VOICE] 回答: %s\n", answer);
}

void *voice_thread(void *arg)
{
	volatile int *stop = (volatile int *)arg;

	if (!g_inited)
	{
		fprintf(stderr, "[VOICE] 没初始化，线程不启动\n");
		return NULL;
	}

	pthread_setname_np(pthread_self(), "voice");

	// 线程自己的缓冲。放在这里而不是全局，因为处理时不想长时间持锁
	static short work[VOICE_PCM_MAX];

	fprintf(stderr, "[VOICE] 语音线程启动\n");

	while (*stop == 0)
	{
		pthread_mutex_lock(&g_lock);

		// 等新数据，最多等 200 毫秒。用超时是为了能及时看到 stop 标志
		while (g_ready == 0 && *stop == 0)
		{
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 200 * 1000 * 1000;
			if (ts.tv_nsec >= 1000000000)
			{
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&g_cond, &g_lock, &ts);
		}

		if (*stop != 0)
		{
			pthread_mutex_unlock(&g_lock);
			break;
		}

		size_t frames = g_frames;
		memcpy(work, g_pcm, frames * sizeof(short));
		g_ready = 0;

		pthread_mutex_unlock(&g_lock);

		// 处理期间录音线程照常投递，不持锁，也不会被这里阻塞
		handle_segment(work, frames);
	}

	fprintf(stderr, "[VOICE] 语音线程退出（覆盖掉 %lu 段）\n", g_dropped);

	return NULL;
}
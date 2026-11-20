// ============================================================
// tts.c  云端语音合成并播放
//
// 三步：文字发给模型换一个 wav 链接，把 wav 下到 /tmp，再用 aplay 放。
// 三步都能失败，失败只留一行串口日志，回答本身照样打出来。
//
// 两个为什么：
//   1. 用 posix_spawn 不用 system。本进程多线程，子进程起完立刻 exec，
//      不会碰到父进程锁状态的坑；system 会先多起一个 shell
//   2. 不能用 base64。返回里 data 是空串，只有 url
// ============================================================

#include "tts.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

// posix_spawnp 要用环境变量表
extern char **environ;

#define TTS_WAV      "/tmp/tts.wav"     // 每次覆盖，不占空间
#define TTS_VOICE    "Cherry"           // 女声，中文清楚
#define TTS_JSON_MAX 4096               // 正常响应四百多字节，留余量

// 起子进程并等它结束，返回子进程退出码
static int run_wait(char *const argv[])
{
	pid_t pid = 0;

	int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
	if (rc != 0)
	{
		fprintf(stderr, "[TTS] 起不来 %s: %s\n", argv[0], strerror(rc));
		return -1;
	}

	int status = 0;
	if (waitpid(pid, &status, 0) < 0)
		return -1;

	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int tts_init(void)
{
	// 四段耳机插上去，codec 判成带麦，HP 那条会把输出关掉，
	// aplay 不报错但耳机没声音。必须 HP_NO_MIC
	char *argv[] = { "amixer", "-c", "0", "sset", "Playback Path",
			 "HP_NO_MIC", NULL };

	if (run_wait(argv) != 0)
	{
		fprintf(stderr, "[TTS] 放音通路没设上，耳机可能不响\n");
		return -1;
	}

	return 0;
}

// 发一次合成请求，把音频链接取出来
static int ask_url(const struct ai_config *cfg, const char *text,
		   char *url, size_t url_size)
{
	char resp[TTS_JSON_MAX];

	cJSON *root = cJSON_CreateObject();
	if (root == NULL)
		return -1;

	cJSON_AddStringToObject(root, "model", AI_MODEL_TTS);

	cJSON *input = cJSON_AddObjectToObject(root, "input");
	if (input == NULL)
	{
		cJSON_Delete(root);
		return -1;
	}

	cJSON_AddStringToObject(input, "text", text);
	cJSON_AddStringToObject(input, "voice", TTS_VOICE);

	// 用 cJSON 拼请求，不手写 sprintf。回答里出现一个引号，
	// 手工拼的 JSON 就坏了
	char *body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (body == NULL)
		return -1;

	int rc = http_post_json(AI_TTS_URL, cfg->api_key, body, resp, sizeof(resp));
	cJSON_free(body);

	if (rc != 0)
		return rc;

	cJSON *jr = cJSON_Parse(resp);
	if (jr == NULL)
	{
		fprintf(stderr, "[TTS] 响应不是 JSON: %.200s\n", resp);
		return -1;
	}

	cJSON *output = cJSON_GetObjectItemCaseSensitive(jr, "output");
	cJSON *audio = output ? cJSON_GetObjectItemCaseSensitive(output, "audio") : NULL;
	cJSON *u = audio ? cJSON_GetObjectItemCaseSensitive(audio, "url") : NULL;

	if (u == NULL || !cJSON_IsString(u) || u->valuestring == NULL)
	{
		fprintf(stderr, "[TTS] 响应里没有音频链接: %.200s\n", resp);
		cJSON_Delete(jr);
		return -1;
	}

	snprintf(url, url_size, "%s", u->valuestring);
	cJSON_Delete(jr);

	return 0;
}

int tts_say(const struct ai_config *cfg, const char *text)
{
	if (cfg == NULL || text == NULL || text[0] == '\0')
		return -1;

	char url[1024];

	if (ask_url(cfg, text, url, sizeof(url)) != 0)
	{
		fprintf(stderr, "[TTS] 合成失败\n");
		return -1;
	}

	if (http_get_to_file(url, TTS_WAV) != 0)
	{
		fprintf(stderr, "[TTS] wav 下载失败\n");
		return -1;
	}

	// -q 关掉 aplay 自己的统计输出，串口只留我们的日志
	char *argv[] = { "aplay", "-q", "-D", "plughw:0,0", TTS_WAV, NULL };

	if (run_wait(argv) != 0)
	{
		fprintf(stderr, "[TTS] 播放失败\n");
		return -1;
	}

	return 0;
}
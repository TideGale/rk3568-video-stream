// ============================================================
// aitest.c  第 5 步的验证程序，四个子命令
//
//   ./aitest text  "用一句话说明你是什么模型"
//   ./aitest vlm   /root/frame_0.jpg "画面里有什么"
//   ./aitest asr   /tmp/a.wav
//   ./aitest trans /tmp/a.wav    走 /audio/transcriptions，兼容层没这个接口
//
// 目的：在接进 capture 之前，先把 TLS、curl、cJSON、base64
// 四段分别验通。哪个子命令报错，就知道是哪段的问题。
//
// 编译：用仓库根目录的 Makefile，make aitest
// 链接库：-lcurl -lssl -lcrypto -lm -lpthread -ldl
// ============================================================

#include "httpclient.h"
#include "ai.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 把整个文件读进 malloc 出来的缓冲
static unsigned char *read_file(const char *path, size_t *out_len)
{
	FILE *fp = fopen(path, "rb");
	if (fp == NULL)
	{
		perror("[TEST] fopen");
		return NULL;
	}

	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (sz <= 0)
	{
		fprintf(stderr, "[TEST] 文件是空的: %s\n", path);
		fclose(fp);
		return NULL;
	}

	unsigned char *buf = malloc((size_t)sz);
	if (buf == NULL)
	{
		fclose(fp);
		return NULL;
	}

	size_t got = fread(buf, 1, (size_t)sz, fp);
	fclose(fp);

	if (got != (size_t)sz)
	{
		fprintf(stderr, "[TEST] 只读到 %zu / %ld 字节\n", got, sz);
		free(buf);
		return NULL;
	}

	*out_len = got;
	return buf;
}

static int cmd_text(const struct ai_config *cfg, const char *q)
{
	char answer[2048];
	printf("[TEST] 纯文本问答\n");

	if (ai_ask_text(cfg, AI_MODEL_TEXT, q, answer, sizeof(answer)) != 0)
		return 1;

	printf("[TEST] 答: %s\n", answer);
	return 0;
}

static int cmd_vlm(const struct ai_config *cfg, const char *path, const char *q,
		   const char *model)
{
	size_t len = 0;
	unsigned char *jpeg = read_file(path, &len);
	if (jpeg == NULL)
		return 1;

	printf("[TEST] 看图问答, 图片 %zu 字节, 模型 %s\n", len, model);

	char answer[2048];
	int rc = ai_ask_image(cfg, model, q, jpeg, len,
			      answer, sizeof(answer));
	free(jpeg);

	if (rc != 0)
		return 1;

	printf("[TEST] 答: %s\n", answer);
	return 0;
}

static int cmd_asr(const struct ai_config *cfg, const char *path,
		   const char *model)
{
	size_t len = 0;
	unsigned char *raw = read_file(path, &len);
	if (raw == NULL)
		return 1;

	// wav 头固定 44 字节，跳过就是纯 PCM
	if (len <= 44)
	{
		fprintf(stderr, "[TEST] 文件太小, 不像 wav\n");
		free(raw);
		return 1;
	}

	size_t pcm_bytes = len - 44;
	size_t frames = pcm_bytes / 2;          // 16 位 = 每帧 2 字节

	printf("[TEST] 语音识别, wav %zu 字节, PCM %zu 帧 (%.2f 秒), 模型 %s\n",
	       len, frames, (double)frames / 16000.0, model);

	char answer[2048];
	int rc = ai_ask_audio(cfg, model,
			      (const short *)(raw + 44), frames,
			      answer, sizeof(answer));
	free(raw);

	if (rc != 0)
		return 1;

	printf("[TEST] 识别结果: %s\n", answer);
	return 0;
}

// 文件上传版：整个 wav 走 multipart 传到 /audio/transcriptions
static int cmd_trans(const struct ai_config *cfg, const char *path,
		     const char *model)
{
	size_t len = 0;
	unsigned char *wav = read_file(path, &len);
	if (wav == NULL)
		return 1;

	printf("[TEST] 文件转写, wav %zu 字节, 模型 %s\n", len, model);

	char answer[2048];
	int rc = ai_transcribe(cfg, model, wav, len, answer, sizeof(answer));
	free(wav);

	if (rc != 0)
		return 1;

	printf("[TEST] 识别结果: %s\n", answer);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr,
			"用法:\n"
			"  %s text  \"问题\"\n"
			"  %s vlm   <图片路径> \"问题\"\n"
			"  %s asr   <wav路径>    走 chat 接口的 input_audio\n"
			"  %s trans <wav路径>    走 /audio/transcriptions，兼容层没这个接口\n",
			argv[0], argv[0], argv[0], argv[0]);
		return 1;
	}

	struct ai_config cfg;
	if (ai_config_load(AI_KEY_PATH, &cfg) < 0)
		return 1;

	printf("[TEST] base_url %s\n", cfg.base_url);
	printf("[TEST] api_key  %.8s... (%zu 字节)\n",
	       cfg.api_key, strlen(cfg.api_key));

	// 多线程程序里也是进程级初始化一次，这里单线程无所谓，但保持同样写法
	http_global_init();

	int rc = 1;

	if (strcmp(argv[1], "text") == 0)
	{
		rc = cmd_text(&cfg, argc > 2 ? argv[2] : "用一句话说明你是什么模型");
	}
	else if (strcmp(argv[1], "vlm") == 0)
	{
		if (argc < 4)
		{
			fprintf(stderr, "vlm 需要图片路径和问题，第三个参数可选模型名\n");
			rc = 1;
		}
		else
		{
			// 第四个参数可以临时指定模型，不写就用配置里的
			const char *model = argc > 4 ? argv[4] : AI_MODEL_VLM;
			rc = cmd_vlm(&cfg, argv[2], argv[3], model);
		}
	}
	else if (strcmp(argv[1], "trans") == 0)
	{
		if (argc < 3)
		{
			fprintf(stderr, "trans 需要 wav 路径，第三个参数可选模型名\n");
			rc = 1;
		}
		else
		{
			const char *model = argc > 3 ? argv[3] : AI_MODEL_ASR;
			rc = cmd_trans(&cfg, argv[2], model);
		}
	}
	else if (strcmp(argv[1], "asr") == 0)
	{
		if (argc < 3)
		{
			fprintf(stderr, "asr 需要 wav 路径，第三个参数可选模型名\n");
			rc = 1;
		}
		else
		{
			const char *model = argc > 3 ? argv[3] : AI_MODEL_ASR;
			rc = cmd_asr(&cfg, argv[2], model);
		}
	}
	else
	{
		fprintf(stderr, "不认识的子命令: %s\n", argv[1]);
		rc = 1;
	}

	http_global_cleanup();

	return rc;
}
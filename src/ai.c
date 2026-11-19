// ============================================================
// ai.c  云端能力：语音识别、看图回答、纯文本问答
//
// 三段请求体构造各不相同，但发请求和解析回答完全一样，
// 所以只有 build_* 三个函数分开，其余共用。
//
// 内存注意：看图那条路要把 45KB 的 JPEG 编码成 60KB 的 base64 文本，
// 再整段拼进 JSON，实际峰值约 130KB。板子内存够，但要心里有数。
// ============================================================

#include "ai.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 响应缓冲。一次回答通常几百字节，8KB 足够；超了会被截断并报警告
#define AI_RESP_SIZE 8192

static const char b64_tab[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *ai_base64(const unsigned char *data, size_t len)
{
	// 每 3 字节变 4 个字符，向上取整，再加结尾 '\0'
	size_t out_len = ((len + 2) / 3) * 4;
	char *out = malloc(out_len + 1);
	if (out == NULL)
		return NULL;

	size_t i = 0;
	size_t j = 0;

	while (i + 3 <= len)
	{
		unsigned int v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
		out[j++] = b64_tab[(v >> 18) & 0x3F];
		out[j++] = b64_tab[(v >> 12) & 0x3F];
		out[j++] = b64_tab[(v >> 6)  & 0x3F];
		out[j++] = b64_tab[v & 0x3F];
		i += 3;
	}

	// 末尾剩 1 或 2 个字节，补 '='
	if (len - i == 1)
	{
		unsigned int v = data[i] << 16;
		out[j++] = b64_tab[(v >> 18) & 0x3F];
		out[j++] = b64_tab[(v >> 12) & 0x3F];
		out[j++] = '=';
		out[j++] = '=';
	}
	else if (len - i == 2)
	{
		unsigned int v = (data[i] << 16) | (data[i + 1] << 8);
		out[j++] = b64_tab[(v >> 18) & 0x3F];
		out[j++] = b64_tab[(v >> 12) & 0x3F];
		out[j++] = b64_tab[(v >> 6) & 0x3F];
		out[j++] = '=';
	}

	out[j] = '\0';
	return out;
}

// 从 choices[0].message.content 里取回答
static int extract_answer(const char *resp, char *answer, size_t answer_size)
{
	cJSON *root = cJSON_Parse(resp);
	if (root == NULL)
	{
		fprintf(stderr, "[AI] 响应不是合法 JSON: %.200s\n", resp);
		return -1;
	}

	cJSON *choices = cJSON_GetObjectItem(root, "choices");
	cJSON *first   = choices != NULL ? cJSON_GetArrayItem(choices, 0) : NULL;
	cJSON *msg     = first != NULL ? cJSON_GetObjectItem(first, "message") : NULL;
	cJSON *content = msg != NULL ? cJSON_GetObjectItem(msg, "content") : NULL;

	if (content == NULL || !cJSON_IsString(content))
	{
		fprintf(stderr, "[AI] 没取到 content: %.200s\n", resp);
		cJSON_Delete(root);
		return -1;
	}

	snprintf(answer, answer_size, "%s", content->valuestring);
	cJSON_Delete(root);

	return 0;
}

// 发请求并解析。三个 build_* 之后都走这里
static int post_and_parse(const struct ai_config *cfg, const char *body,
			  char *answer, size_t answer_size)
{
	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", cfg->base_url);

	// 设了 AI_DEBUG 就把请求体的开头和结尾打出来，方便对格式。
	// 中间那段是 base64 的图片或音频，不打进日志
	if (getenv("AI_DEBUG") != NULL)
	{
		size_t blen = strlen(body);
		size_t tail = blen > 200 ? 200 : blen;
		fprintf(stderr, "[AI] 请求体共 %zu 字节\n", blen);
		fprintf(stderr, "[AI] 开头: %.200s\n", body);
		fprintf(stderr, "[AI] 结尾: %s\n", body + blen - tail);
	}

	// 响应缓冲放在栈上，不能用 static：
	// 第 6 步里语音识别和看图可能各跑一条线程，static 会互相踩
	char resp[AI_RESP_SIZE];

	int rc = http_post_json(url, cfg->api_key, body, resp, sizeof(resp));
	if (rc != 0)
		return rc;

	return extract_answer(resp, answer, answer_size);
}

int ai_ask_text(const struct ai_config *cfg, const char *model,
		const char *question, char *answer, size_t answer_size)
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "model", model);

	cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
	cJSON *m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "role", "user");
	cJSON_AddStringToObject(m, "content", question);
	cJSON_AddItemToArray(msgs, m);

	cJSON_AddNumberToObject(root, "max_tokens", 300);

	char *body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (body == NULL)
		return -1;

	int rc = post_and_parse(cfg, body, answer, answer_size);
	free(body);

	return rc;
}

int ai_ask_image(const struct ai_config *cfg, const char *model,
		 const char *question,
		 const unsigned char *jpeg, size_t jpeg_len,
		 char *answer, size_t answer_size)
{
	char *b64 = ai_base64(jpeg, jpeg_len);
	if (b64 == NULL)
	{
		fprintf(stderr, "[AI] base64 编码失败\n");
		return -1;
	}

	// data URI 的前缀。这里写死 jpeg，因为我们抓的就是 JPEG
	size_t uri_len = strlen(b64) + 32;
	char *uri = malloc(uri_len);
	if (uri == NULL)
	{
		free(b64);
		return -1;
	}
	snprintf(uri, uri_len, "data:image/jpeg;base64,%s", b64);
	free(b64);                      // 已经被拷进 uri，原始 base64 可以放了

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "model", model);

	cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
	cJSON *m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "role", "user");

	// 看图时 content 必须是数组，不是字符串。顺序是先图后文效果好一点
	cJSON *parts = cJSON_AddArrayToObject(m, "content");

	// 图文顺序默认先图后文；设了 AI_TEXT_FIRST 就反过来。
	// 不同平台的模型对顺序要求不一样，出 400 时先试这一项
	int text_first = getenv("AI_TEXT_FIRST") != NULL;

	cJSON *txt = cJSON_CreateObject();
	cJSON_AddStringToObject(txt, "type", "text");
	cJSON_AddStringToObject(txt, "text", question);

	cJSON *img = cJSON_CreateObject();
	cJSON_AddStringToObject(img, "type", "image_url");
	cJSON *imgobj = cJSON_AddObjectToObject(img, "image_url");
	cJSON_AddStringToObject(imgobj, "url", uri);

	if (text_first)
	{
		cJSON_AddItemToArray(parts, txt);
		cJSON_AddItemToArray(parts, img);
	}
	else
	{
		cJSON_AddItemToArray(parts, img);
		cJSON_AddItemToArray(parts, txt);
	}

	cJSON_AddItemToArray(msgs, m);
	cJSON_AddNumberToObject(root, "max_tokens", 300);

	char *body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	free(uri);

	if (body == NULL)
		return -1;

	int rc = post_and_parse(cfg, body, answer, answer_size);
	free(body);

	return rc;
}

// 给 PCM 加上 44 字节 wav 头，拼成内存里的完整 wav 文件
static unsigned char *pcm_to_wav(const short *pcm, size_t frames, size_t *out_len)
{
	unsigned int datalen  = (unsigned int)(frames * sizeof(short));
	unsigned int total    = 44 + datalen;
	unsigned int rate     = 16000;
	unsigned short ch     = 1;
	unsigned short bits   = 16;
	unsigned int byterate = rate * ch * bits / 8;
	unsigned short align  = (unsigned short)(ch * bits / 8);
	unsigned int riff     = 36 + datalen;
	unsigned int fmtlen   = 16;
	unsigned short pcmfmt = 1;

	unsigned char *w = malloc(total);
	if (w == NULL)
		return NULL;

	unsigned char *p = w;

	memcpy(p, "RIFF", 4); p += 4;  memcpy(p, &riff, 4);     p += 4;
	memcpy(p, "WAVEfmt ", 8); p += 8; memcpy(p, &fmtlen, 4); p += 4;
	memcpy(p, &pcmfmt, 2); p += 2; memcpy(p, &ch, 2);       p += 2;
	memcpy(p, &rate, 4);   p += 4; memcpy(p, &byterate, 4); p += 4;
	memcpy(p, &align, 2);  p += 2; memcpy(p, &bits, 2);     p += 2;
	memcpy(p, "data", 4);  p += 4; memcpy(p, &datalen, 4);  p += 4;

	memcpy(p, pcm, datalen);

	*out_len = total;
	return w;
}

int ai_ask_audio(const struct ai_config *cfg, const char *model,
		 const short *pcm, size_t frames,
		 char *answer, size_t answer_size)
{
	// 16 位单声道 16000Hz，3 秒就是 96KB，base64 之后 128KB
	size_t wav_len = 0;
	unsigned char *wav = pcm_to_wav(pcm, frames, &wav_len);
	if (wav == NULL)
		return -1;

	char *b64 = ai_base64(wav, wav_len);
	free(wav);
	if (b64 == NULL)
		return -1;

	// 阿里云靠 data URI 前缀判断音频格式，裸 base64 会报
	// {"code":"UNSUPPORTED_FORMAT","message":"format is empty"}
	// 单独给一个 "format" 字段它不认，所以前缀是必需的。
	// 设 AI_AUDIO_URI=0 可以退回裸 base64，用于对比测试
	int use_uri = getenv("AI_AUDIO_URI") == NULL ||
		      strcmp(getenv("AI_AUDIO_URI"), "0") != 0;

	char *payload = b64;
	if (use_uri)
	{
		size_t n = strlen(b64) + 32;
		payload = malloc(n);
		if (payload == NULL)
		{
			free(b64);
			return -1;
		}
		snprintf(payload, n, "data:audio/wav;base64,%s", b64);
		free(b64);
	}

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "model", model);

	cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
	cJSON *m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "role", "user");

	// 音频同样必须是内容数组
	cJSON *parts = cJSON_AddArrayToObject(m, "content");

	cJSON *aud = cJSON_CreateObject();
	cJSON_AddStringToObject(aud, "type", "input_audio");
	cJSON *audobj = cJSON_AddObjectToObject(aud, "input_audio");
	cJSON_AddStringToObject(audobj, "data", payload);
	cJSON_AddStringToObject(audobj, "format", "wav");
	cJSON_AddItemToArray(parts, aud);

	cJSON *txt = cJSON_CreateObject();
	cJSON_AddStringToObject(txt, "type", "text");
	cJSON_AddStringToObject(txt, "text", "把这段语音转成文字，只输出文字，不要解释。");
	cJSON_AddItemToArray(parts, txt);

	cJSON_AddItemToArray(msgs, m);
	cJSON_AddNumberToObject(root, "max_tokens", 300);

	char *body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	free(payload);                  // use_uri 时 payload 是新分配的，裸 base64 时就是 b64

	if (body == NULL)
		return -1;

	int rc = post_and_parse(cfg, body, answer, answer_size);
	free(body);

	return rc;
}
// ============================================================
// 文件上传版的语音识别
//
// 走 POST {base_url}/audio/transcriptions，multipart 传 wav。
// chat/completions 那条路阿里云不认（返回 UNSUPPORTED_FORMAT），
// 这个接口是 OpenAI 白标，wav 当文件传，没有 format 字段的麻烦。
//
// 响应有两种常见形态，两种都认：
//   {"text":"识别结果"}
//   {"choices":[{"message":{"content":"识别结果"}}]}
// ============================================================
int ai_transcribe(const struct ai_config *cfg, const char *model,
		  const unsigned char *wav, size_t wav_len,
		  char *answer, size_t answer_size)
{
	if (cfg == NULL || wav == NULL || wav_len == 0)
		return -1;

	char url[512];
	snprintf(url, sizeof(url), "%s/audio/transcriptions", cfg->base_url);

	if (getenv("AI_DEBUG") != NULL)
		fprintf(stderr, "[AI] 上传 %zu 字节到 %s，模型 %s\n",
			wav_len, url, model);

	char resp[AI_RESP_SIZE];

	int rc = http_post_multipart(url, cfg->api_key, model,
				     "a.wav", "audio/wav",
				     wav, wav_len, resp, sizeof(resp));
	if (rc != 0)
		return rc;

	if (getenv("AI_DEBUG") != NULL)
		fprintf(stderr, "[AI] 响应: %.300s\n", resp);

	cJSON *root = cJSON_Parse(resp);
	if (root == NULL)
	{
		fprintf(stderr, "[AI] 响应不是合法 JSON: %.200s\n", resp);
		return -1;
	}

	// 形态一：顶层 text 字段
	cJSON *text = cJSON_GetObjectItem(root, "text");
	if (text != NULL && cJSON_IsString(text))
	{
		snprintf(answer, answer_size, "%s", text->valuestring);
		cJSON_Delete(root);
		return 0;
	}

	// 形态二：和 chat/completions 一样走 choices
	cJSON *choices = cJSON_GetObjectItem(root, "choices");
	cJSON *first   = choices != NULL ? cJSON_GetArrayItem(choices, 0) : NULL;
	cJSON *msg     = first != NULL ? cJSON_GetObjectItem(first, "message") : NULL;
	cJSON *content = msg != NULL ? cJSON_GetObjectItem(msg, "content") : NULL;

	if (content != NULL && cJSON_IsString(content))
	{
		snprintf(answer, answer_size, "%s", content->valuestring);
		cJSON_Delete(root);
		return 0;
	}

	fprintf(stderr, "[AI] 响应里找不到文字: %.200s\n", resp);
	cJSON_Delete(root);

	return -1;
}

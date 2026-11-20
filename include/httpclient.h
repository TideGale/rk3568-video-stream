// ============================================================
// httpclient.h  HTTPS 请求与密钥配置
//
// 只管两件事：从配置文件读密钥、发一个 JSON POST 请求。
// 不关心请求内容是语音识别还是看图，那一层在 ai.c 里。
// ============================================================

#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include <stddef.h>

// 密钥文件默认位置，权限必须是 600，内容三行 key=value
#define AI_KEY_PATH "/root/.config/aikey"

struct ai_config
{
	char base_url[256];         // 形如 https://xxx/compatible-mode/v1
	char api_key[256];          // sk- 开头
	char model[64];             // 默认模型名，可被单次请求覆盖
};

// 读配置文件。成功返回 0，缺字段或打不开返回 -1 并打印原因
int ai_config_load(const char *path, struct ai_config *cfg);

// 发一个 JSON POST，响应写进 resp（含结尾 '\0'）
// 返回 0 表示 HTTP 2xx；否则返回负数的 curl 错误码或 HTTP 状态码
int http_post_json(const char *url, const char *api_key,
		   const char *json, char *resp, size_t resp_size);

// 发一个 multipart/form-data POST：一个文本字段 model + 一个文件字段 file。
// 语音识别这类接口要求传文件而不是 base64，就用它
int http_post_multipart(const char *url, const char *api_key,
			const char *model,
			const char *filename, const char *mime,
			const unsigned char *data, size_t len,
			char *resp, size_t resp_size);

// 进程启动时调一次，退出时调一次。多线程下必须这么用
void http_global_init(void);
void http_global_cleanup(void);

// 指定根证书包路径。嵌入式根文件系统常常没装 ca-certificates，
// 不指定就会报 unable to get local issuer certificate。
// 配置文件里的 ca_path= 会自动调这个函数
void http_set_ca(const char *path);

// 把一个 URL 原样下到本地文件。TTS 返回的 wav 链接用它
int http_get_to_file(const char *url, const char *path);

#endif
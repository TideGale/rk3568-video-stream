// ============================================================
// httpclient.c  HTTPS 请求与密钥配置
//
// 用 libcurl 发请求。三个地方容易忽略，都写在这里：
//   1. 多线程下必须 CURLOPT_NOSIGNAL，否则 DNS 超时用 alarm 会杀掉进程
//   2. 写回调必须限长，服务器返回超长响应不能冲垮缓冲区
//   3. curl 的错误码和 HTTP 状态码是两回事，要分开报
// ============================================================

#include "httpclient.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

// 根证书包路径。空表示用 libcurl 自己找的默认位置
static char g_ca_path[256];

void http_set_ca(const char *path)
{
	if (path == NULL)
	{
		g_ca_path[0] = '\0';
		return;
	}
	snprintf(g_ca_path, sizeof(g_ca_path), "%s", path);
}

// 写回调的上下文：缓冲区 + 已写入长度 + 容量
struct resp_buf
{
	char  *data;
	size_t len;
	size_t cap;
	int    truncated;               // 响应超长被截断，要告诉调用者
};

// libcurl 每收到一块数据就调这里
static size_t on_write(void *ptr, size_t size, size_t nmemb, void *userp)
{
	struct resp_buf *b = (struct resp_buf *)userp;
	size_t n = size * nmemb;

	// 留一个字节给结尾 '\0'。装不下就只装一部分并打标记，绝不越界
	if (b->len + n + 1 > b->cap)
	{
		size_t room = b->cap > b->len + 1 ? b->cap - b->len - 1 : 0;
		memcpy(b->data + b->len, ptr, room);
		b->len += room;
		b->data[b->len] = '\0';
		b->truncated = 1;
		return n;                   // 返回值必须等于 n，否则 curl 认为出错
	}

	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = '\0';

	return n;
}

static void trim(char *s)
{
	// 去掉首尾空白，配置文件的等号两边常带空格
	char *p = s;
	while (*p != '\0' && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);

	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

int ai_config_load(const char *path, struct ai_config *cfg)
{
	if (cfg == NULL)
		return -1;

	memset(cfg, 0, sizeof(*cfg));

	FILE *fp = fopen(path, "r");
	if (fp == NULL)
	{
		fprintf(stderr, "[AI] 打不开密钥文件 %s\n", path);
		return -1;
	}

	char line[512];
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		trim(line);

		// 空行和 # 开头的注释跳过
		if (line[0] == '\0' || line[0] == '#')
			continue;

		char *eq = strchr(line, '=');
		if (eq == NULL)
			continue;

		*eq = '\0';
		char *k = line;
		char *v = eq + 1;
		trim(k);
		trim(v);

		// snprintf 而不是 strcpy，长度不对就截断而不是溢出
		if (strcmp(k, "base_url") == 0)
			snprintf(cfg->base_url, sizeof(cfg->base_url), "%s", v);
		else if (strcmp(k, "api_key") == 0)
			snprintf(cfg->api_key, sizeof(cfg->api_key), "%s", v);
		else if (strcmp(k, "model") == 0)
			snprintf(cfg->model, sizeof(cfg->model), "%s", v);
		else if (strcmp(k, "ca_path") == 0)
			http_set_ca(v);         // 板子缺根证书时，这里指定 cacert.pem
	}

	fclose(fp);

	// 三个字段缺一不可，缺了就早点报，别等到发请求才 401
	if (cfg->base_url[0] == '\0' || cfg->api_key[0] == '\0')
	{
		fprintf(stderr, "[AI] 密钥文件缺 base_url 或 api_key\n");
		return -1;
	}

	if (cfg->model[0] == '\0')
		snprintf(cfg->model, sizeof(cfg->model), "%s", "qwen3-vl-plus");

	return 0;
}

void http_global_init(void)
{
	// libcurl 的全局初始化，整个进程只需一次。多线程下别在每个线程里调
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_global_cleanup(void)
{
	curl_global_cleanup();
}


int http_post_multipart(const char *url, const char *api_key,
			const char *model,
			const char *filename, const char *mime,
			const unsigned char *data, size_t len,
			char *resp, size_t resp_size)
{
	if (url == NULL || data == NULL || resp == NULL || resp_size < 2)
		return -1;

	resp[0] = '\0';

	// 边界串用固定值：每次进程启动一样，但对服务端来说只要是唯一串就合法
	const char *bnd = "----rk3568boundary7d3f9a";

	// 先算总长度，省一次 realloc。各段格式固定，用 snprintf 打头再拼二进制
	char head[512];
	int head_len = snprintf(head, sizeof(head),
		"--%s\r\n"
		"Content-Disposition: form-data; name=\"model\"\r\n\r\n"
		"%s\r\n"
		"--%s\r\n"
		"Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
		"Content-Type: %s\r\n\r\n",
		bnd, model, bnd, filename, mime);

	char tail[128];
	int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", bnd);

	if (head_len <= 0 || tail_len <= 0)
		return -1;

	size_t total = (size_t)head_len + len + (size_t)tail_len;
	unsigned char *body = malloc(total);
	if (body == NULL)
		return -1;

	memcpy(body, head, (size_t)head_len);
	memcpy(body + head_len, data, len);
	memcpy(body + head_len + len, tail, (size_t)tail_len);

	CURL *curl = curl_easy_init();
	if (curl == NULL)
	{
		free(body);
		return -1;
	}

	char auth[512];
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);

	char ctype[128];
	snprintf(ctype, sizeof(ctype), "Content-Type: multipart/form-data; boundary=%s", bnd);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, ctype);
	headers = curl_slist_append(headers, auth);

	struct resp_buf rb = { resp, 0, resp_size, 0 };

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)body);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)total);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);   // 转写比对话慢，给宽一点

	if (g_ca_path[0] != '\0')
		curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_path);

	CURLcode rc = curl_easy_perform(curl);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(body);

	if (rc != CURLE_OK)
	{
		fprintf(stderr, "[AI] 上传失败: %s\n", curl_easy_strerror(rc));
		return -(int)rc;
	}

	if (rb.truncated)
		fprintf(stderr, "[AI] 警告: 响应超过 %zu 字节被截断\n", resp_size);

	if (http_code < 200 || http_code >= 300)
	{
		fprintf(stderr, "[AI] HTTP %ld, 响应: %.300s\n", http_code, resp);
		return (int)http_code;
	}

	return 0;
}

int http_post_json(const char *url, const char *api_key,
		   const char *json, char *resp, size_t resp_size)
{
	if (url == NULL || json == NULL || resp == NULL || resp_size < 2)
		return -1;

	resp[0] = '\0';

	CURL *curl = curl_easy_init();
	if (curl == NULL)
	{
		fprintf(stderr, "[AI] curl_easy_init 失败\n");
		return -1;
	}

	// Authorization 头里带密钥。拼成 "Authorization: Bearer xxx"
	char auth[512];
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, auth);

	struct resp_buf rb = { resp, 0, resp_size, 0 };

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json));
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);

	// 多线程必需：否则 libcurl 用 alarm 做 DNS 超时，会杀掉整个进程
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	// 连接 10 秒、整体 30 秒。模型推理慢，整体不能给太短
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

	// 跟随重定向（阿里云偶尔会 302 到别的节点）
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	// 指定根证书包。板子上没装 ca-certificates 时必须给路径，
	// 否则 TLS 校验过不去，报 unable to get local issuer certificate
	if (g_ca_path[0] != '\0')
		curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_path);

	CURLcode rc = curl_easy_perform(curl);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK)
	{
		// 这一类是网络层失败：DNS、连不上、超时、证书问题
		fprintf(stderr, "[AI] 请求失败: %s\n", curl_easy_strerror(rc));
		return -(int)rc;
	}

	if (rb.truncated)
		fprintf(stderr, "[AI] 警告: 响应超过 %zu 字节被截断\n", resp_size);

	if (http_code < 200 || http_code >= 300)
	{
		// 这一类是服务端拒绝：401 密钥错、400 参数错、429 限流
		fprintf(stderr, "[AI] HTTP %ld, 响应: %.200s\n", http_code, resp);
		return (int)http_code;
	}

	return 0;
}

// 写文件的回调。必须返回实际写入的字节数，
// 返回少了 curl 会当成写失败直接中断
struct file_sink
{
	FILE *fp;
	size_t len;
};

static size_t on_write_file(void *ptr, size_t size, size_t nmemb, void *userp)
{
	struct file_sink *s = (struct file_sink *)userp;
	size_t n = size * nmemb;
	size_t w = fwrite(ptr, 1, n, s->fp);

	s->len += w;
	return w;
}

int http_get_to_file(const char *url, const char *path)
{
	if (url == NULL || path == NULL)
		return -1;

	FILE *fp = fopen(path, "wb");
	if (fp == NULL)
	{
		perror("[AI] fopen");
		return -1;
	}

	CURL *curl = curl_easy_init();
	if (curl == NULL)
	{
		fclose(fp);
		return -1;
	}

	struct file_sink sink = { fp, 0 };

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write_file);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

	// 音频链接现在是 http，网关以后换成 https 也能用
	if (g_ca_path[0] != '\0')
		curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_path);

	CURLcode rc = curl_easy_perform(curl);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_easy_cleanup(curl);
	fclose(fp);

	if (rc != CURLE_OK)
	{
		fprintf(stderr, "[AI] 下载失败: %s\n", curl_easy_strerror(rc));
		unlink(path);
		return -(int)rc;
	}

	if (http_code < 200 || http_code >= 300)
	{
		fprintf(stderr, "[AI] 下载 HTTP %ld\n", http_code);
		unlink(path);
		return (int)http_code;
	}

	return 0;
}
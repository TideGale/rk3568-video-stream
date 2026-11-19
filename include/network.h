// ============================================================
// network.h  HTTP MJPEG 推流服务的对外接口
//
// 只在 capture.c 里 include，内部实现不暴露。
// ============================================================

#ifndef NETWORK_H
#define NETWORK_H

#include "ringbuf.h"

#define NET_PORT    8080                // 监听端口，浏览器直接访问
#define NET_MAX_CLI 4                   // 最多同时连 4 个客户端

int  net_start(int port);                   // 建监听并起 accept 线程
void net_broadcast(const struct frame *f);  // 把一帧送给所有客户端

void net_keep_frame(const struct frame *f);               // 留一份最新帧，给 /capture 用
void net_update_stats(double fps, unsigned long frames,
		      unsigned long drop_net, unsigned long drop_prev);

// 取一份最新帧的副本，调用者负责 free。返回 0 成功，-1 表示还没采到帧
// 语音模块用它做看图回答和语音拍照
int  net_get_frame(unsigned char **buf, size_t *len, unsigned long *seq);

// 把运行数据格式化成一行 JSON，返回写入长度。
// HTTP 的 /status 和语音播报共用，保证两边数字一致
int  net_format_status(char *buf, size_t size);

void net_stop(void);                        // 关监听、断开所有客户端

#endif

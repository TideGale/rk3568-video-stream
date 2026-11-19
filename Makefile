# ============================================================
# RK3568 视频终端 Makefile
#
# v1.0 时代是单条 gcc 命令编 5 个文件，v2.0 加到 11 个源文件、
# 还要链接 alsa/curl/openssl 三个交叉编译库，命令行已经长到
# 一条敲不完，所以迁到 Makefile。
#
# 用法：
#   make            编 capture（主程序）
#   make aitest     编 aitest（云端接口验证工具）
#   make probe      编 probe（V4L2 设备探针）
#   make clean      清产物
#
# 交叉编译需要两个前缀：
#   ALSA_PREFIX  alsa-lib 装的位置（第 2 步编的）
#   AI_PREFIX    curl + openssl 装的位置（第 5 步编的）
# ============================================================

# 交叉编译器。本机有 aarch64-linux-gnu-gcc 就直接用；换成自己的工具链改这一行
CC      := aarch64-linux-gnu-gcc

# 两个第三方库前缀。用 $(HOME) 展开，换机器只改这里
ALSA_PREFIX ?= $(HOME)/mylib/arm-alsa
AI_PREFIX   ?= $(HOME)/mylib/arm-ai

CFLAGS  := -Wall -O2 -pthread -std=gnu99 -MMD -MP -D_GNU_SOURCE \
           -Isrc -Iinclude -Ithird_party \
           -I$(ALSA_PREFIX)/include -I$(AI_PREFIX)/include

# 链接顺序不能随便改：curl 依赖 ssl 和 crypto，必须写在前面
# -ldl 必须补：openssl 的静态库用到 dlopen/dlsym，glibc 2.34 之前这些都是独立库
ALSA_LIBS := -L$(ALSA_PREFIX)/lib -lasound
AI_LIBS   := -L$(AI_PREFIX)/lib -lcurl -lssl -lcrypto
BASE_LIBS := -lm -lpthread -ldl

# 只做视频（不依赖音频和云端）的源文件
VIDEO_SRC := src/capture.c src/network.c src/preview.c src/ringbuf.c
VIDEO_OBJ := $(VIDEO_SRC:.c=.o)

# 音频模块，单独成组，方便只看视频时排除
AUDIO_OBJ := src/audio.o

# 云端与语音交互：识别、看图、意图判断。要联网，依赖 curl + openssl
VOICE_OBJ := src/voice.o src/ai.o src/httpclient.o

# 第三方 C 文件，跟主代码一起编
THIRD_OBJ := third_party/cJSON.o

.PHONY: all clean help

all: capture

# ---------- 主程序 ----------
capture: $(VIDEO_OBJ) $(AUDIO_OBJ) $(VOICE_OBJ) $(THIRD_OBJ)
	$(CC) -o $@ $^ $(ALSA_LIBS) $(AI_LIBS) $(BASE_LIBS)
	@echo "编好了: $@"
	@file $@ | sed 's/^/  /'

# ---------- 云端接口验证工具 ----------
aitest: src/aitest.o src/ai.o src/httpclient.o third_party/cJSON.o
	$(CC) -o $@ $^ $(AI_LIBS) $(BASE_LIBS)
	@echo "编好了: $@"

# ---------- 独立录音小工具，用来造测试 wav ----------
audiorec: src/audiorec.o src/audio.o
	$(CC) -o $@ $^ $(ALSA_LIBS) $(BASE_LIBS)
	@echo "编好了: $@"

# ---------- 设备探针（不依赖任何第三方库）----------
probe: src/probe.o
	$(CC) -o $@ $^ $(BASE_LIBS)
	@echo "编好了: $@"

# 通用规则：源文件在 src/ 和 third_party/，头文件在 include/
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o src/*.d third_party/*.o third_party/*.d capture aitest probe audiorec
	@echo "清干净了"

help:
	@echo "make          编 capture"
	@echo "make aitest   编云端接口验证工具"
	@echo "make probe    编设备探针"
	@echo "make clean    清产物"
	@echo ""
	@echo "ALSA_PREFIX = $(ALSA_PREFIX)"
	@echo "AI_PREFIX   = $(AI_PREFIX)"

# 自动头文件依赖：编译时 -MMD 生成 .d，这里 include 进来。
# 没有这一行，改了 .h 之后 make 会认为 .o 还是最新的，不重编
-include $(wildcard src/*.d) $(wildcard third_party/*.d)

// ============================================================
// audiorec.c  独立录音小工具，用来造测试用的 wav
//
//   ./audiorec /tmp/a.wav 3
//
// 它和 capture 里的音频线程用的是同一套采集代码（audio_record），
// 所以录出来的东西和线上一致。单独编成一个小程序，是因为
// audio.c 要作为模块被 capture 链接，里面不能再有 main。
// ============================================================

#include "audio.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc != 3)
	{
		fprintf(stderr, "用法: %s <输出wav> <秒数>\n", argv[0]);
		return 1;
	}

	struct audio_result r;
	if (audio_record(AUDIO_DEV, atoi(argv[2]), argv[1], &r) < 0)
		return 1;

	if (r.peak < 200)
		printf("[AUD] 警告: 峰值只有 %d, 基本上是静音\n", r.peak);

	return 0;
}

#ifndef HOST_STUB_ANDROID_LOG_H
#define HOST_STUB_ANDROID_LOG_H
#include <stdio.h>
#define ANDROID_LOG_DEBUG 3
#define __android_log_print(prio, tag, ...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

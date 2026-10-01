/**
 * minimp3.c — vendored 集成单元（上游官方集成方式）
 *
 * 来源：github.com/lieff/minimp3（公有领域）。
 * 上游为单头文件库：在此编译单元 #define MINIMP3_IMPLEMENTATION 引入实现。
 * 注意：minimp3.h 当前为契约占位（见其文件头注释），集成时将上游
 * minimp3.h 原文拷入 audio/minimp3/minimp3.h 即完成 vendoring，本文件不动。
 */
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

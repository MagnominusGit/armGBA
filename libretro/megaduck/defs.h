// armGBA: the little of SameBoy's defs.h that Super Junior SameDuck's laptop code needs
#pragma once

#include <stdbool.h>

typedef struct GB_gameboy_s GB_gameboy_t;

#define likely(x)   __builtin_expect((bool)(x), 1)
#define unlikely(x) __builtin_expect((bool)(x), 0)

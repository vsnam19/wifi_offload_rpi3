/*
 * Minimal common.h stub for standalone wpa_ctrl compilation.
 * Includes os.h and provides basic type/macro definitions.
 */
#ifndef COMMON_H
#define COMMON_H

#include "os.h"

#include <stdint.h>
#include <endian.h>
#include <byteswap.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#ifndef BIT
#define BIT(n) (1U << (n))
#endif

#endif /* COMMON_H */

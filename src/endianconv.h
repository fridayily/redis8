/* See endianconv.c top comments for more information
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2011-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __ENDIANCONV_H
#define __ENDIANCONV_H

#include "config.h"
#include <stdint.h>

void memrev16(void *p);
void memrev32(void *p);
void memrev64(void *p);
uint16_t intrev16(uint16_t v);
uint32_t intrev32(uint32_t v);
uint64_t intrev64(uint64_t v);

/* variants of the function doing the actual conversion only if the target
 * host is big endian
 * 这段代码定义了一组条件编译的宏，用于在不同字节序（endianness）的系统上处理数据的字节序转换。
 * 它的核心思想是：只在大端（Big Endian）系统上执行字节序转换，
 * 在小端（Little Endian）系统上则不执行任何操作（或直接返回原值）。
 */
#if (BYTE_ORDER == LITTLE_ENDIAN)
#define memrev16ifbe(p) ((void)(0))
#define memrev32ifbe(p) ((void)(0))
#define memrev64ifbe(p) ((void)(0))
#define intrev16ifbe(v) (v)
#define intrev32ifbe(v) (v)
#define intrev64ifbe(v) (v)
#else
#define memrev16ifbe(p) memrev16(p)
#define memrev32ifbe(p) memrev32(p)
#define memrev64ifbe(p) memrev64(p)
#define intrev16ifbe(v) intrev16(v)
#define intrev32ifbe(v) intrev32(v)
#define intrev64ifbe(v) intrev64(v)
#endif

/* The functions htonu64() and ntohu64() convert the specified value to
 * network byte ordering and back. In big endian systems they are no-ops. */
#if (BYTE_ORDER == BIG_ENDIAN)
#define htonu64(v) (v)
#define ntohu64(v) (v)
#else
#define htonu64(v) intrev64(v)
#define ntohu64(v) intrev64(v)
#endif

#ifdef REDIS_TEST
int endianconvTest(int argc, char *argv[], int flags);
#endif

#endif

/* Listpack -- A lists of strings serialization format
 *
 * This file implements the specification you can find at:
 *
 *  https://github.com/antirez/listpack
 *
 * Copyright (c) 2017-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <stdint.h>
#include <limits.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "listpack.h"
#include "listpack_malloc.h"
#include "redisassert.h"
#include "util.h"

#define LP_HDR_SIZE 6       /* 32 bit total len + 16 bit number of elements. */
#define LP_HDR_NUMELE_UNKNOWN UINT16_MAX
#define LP_MAX_INT_ENCODING_LEN 9
#define LP_MAX_BACKLEN_SIZE 5
#define LP_ENCODING_INT 0
#define LP_ENCODING_STRING 1

#define LP_ENCODING_7BIT_UINT 0
#define LP_ENCODING_7BIT_UINT_MASK 0x80
#define LP_ENCODING_IS_7BIT_UINT(byte) (((byte)&LP_ENCODING_7BIT_UINT_MASK)==LP_ENCODING_7BIT_UINT)
#define LP_ENCODING_7BIT_UINT_ENTRY_SIZE 2

// 编码占用1字节
// 0x80 10 000000  6BIT STR 后6bit 表示长度,表示 63字节字符串
// 0xC0 11 000000  便于取该字节的后 6 bit
#define LP_ENCODING_6BIT_STR 0x80
#define LP_ENCODING_6BIT_STR_MASK 0xC0
// 判断该字节是否表示一个 LP_ENCODING_6BIT_STR
#define LP_ENCODING_IS_6BIT_STR(byte) (((byte)&LP_ENCODING_6BIT_STR_MASK)==LP_ENCODING_6BIT_STR)

#define LP_ENCODING_13BIT_INT 0xC0
#define LP_ENCODING_13BIT_INT_MASK 0xE0
#define LP_ENCODING_IS_13BIT_INT(byte) (((byte)&LP_ENCODING_13BIT_INT_MASK)==LP_ENCODING_13BIT_INT)
// 两个字节表示存储的entry, 1个字节表示这个 entry 的长度
#define LP_ENCODING_13BIT_INT_ENTRY_SIZE 3

// 编码占用2字节 0xE0 11100000 12BIT_STR 前 4 bit 是标示符, 12 bit 表示长度
#define LP_ENCODING_12BIT_STR 0xE0
#define LP_ENCODING_12BIT_STR_MASK 0xF0
#define LP_ENCODING_IS_12BIT_STR(byte) (((byte)&LP_ENCODING_12BIT_STR_MASK)==LP_ENCODING_12BIT_STR)

// 0xF1 表示 LP_ENCODING_16BIT_INT
// 有 16 bit 表示有符号整数长度
#define LP_ENCODING_16BIT_INT 0xF1
#define LP_ENCODING_16BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_16BIT_INT(byte) (((byte)&LP_ENCODING_16BIT_INT_MASK)==LP_ENCODING_16BIT_INT)
#define LP_ENCODING_16BIT_INT_ENTRY_SIZE 4

#define LP_ENCODING_24BIT_INT 0xF2
#define LP_ENCODING_24BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_24BIT_INT(byte) (((byte)&LP_ENCODING_24BIT_INT_MASK)==LP_ENCODING_24BIT_INT)
#define LP_ENCODING_24BIT_INT_ENTRY_SIZE 5

#define LP_ENCODING_32BIT_INT 0xF3
#define LP_ENCODING_32BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_32BIT_INT(byte) (((byte)&LP_ENCODING_32BIT_INT_MASK)==LP_ENCODING_32BIT_INT)
#define LP_ENCODING_32BIT_INT_ENTRY_SIZE 6

#define LP_ENCODING_64BIT_INT 0xF4
#define LP_ENCODING_64BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_64BIT_INT(byte) (((byte)&LP_ENCODING_64BIT_INT_MASK)==LP_ENCODING_64BIT_INT)
#define LP_ENCODING_64BIT_INT_ENTRY_SIZE 10

// 1 字节 0xF0 标示符, 32bit 4 字节表示长度,总共 5 字节
#define LP_ENCODING_32BIT_STR 0xF0
#define LP_ENCODING_32BIT_STR_MASK 0xFF
#define LP_ENCODING_IS_32BIT_STR(byte) (((byte)&LP_ENCODING_32BIT_STR_MASK)==LP_ENCODING_32BIT_STR)

#define LP_EOF 0xFF

#define LP_ENCODING_6BIT_STR_LEN(p) ((p)[0] & 0x3F)
#define LP_ENCODING_12BIT_STR_LEN(p) ((((p)[0] & 0xF) << 8) | (p)[1])
#define LP_ENCODING_32BIT_STR_LEN(p) (((uint32_t)(p)[1]<<0) | \
                                      ((uint32_t)(p)[2]<<8) | \
                                      ((uint32_t)(p)[3]<<16) | \
                                      ((uint32_t)(p)[4]<<24))

#define lpGetTotalBytes(p)           (((uint32_t)(p)[0]<<0) | \
                                      ((uint32_t)(p)[1]<<8) | \
                                      ((uint32_t)(p)[2]<<16) | \
                                      ((uint32_t)(p)[3]<<24))

#define lpGetNumElements(p)          (((uint32_t)(p)[4]<<0) | \
                                      ((uint32_t)(p)[5]<<8))

// 将一个 32 位无符号整数 v 以小端字节序（Little-Endian）的方式存储到 p 指向的字节数组中
#define lpSetTotalBytes(p,v) do { \
    (p)[0] = (v)&0xff; \
    (p)[1] = ((v)>>8)&0xff; \
    (p)[2] = ((v)>>16)&0xff; \
    (p)[3] = ((v)>>24)&0xff; \
} while(0)

#define lpSetNumElements(p,v) do { \
    (p)[4] = (v)&0xff; \
    (p)[5] = ((v)>>8)&0xff; \
} while(0)

/* Validates that 'p' is not outside the listpack.
 * All function that return a pointer to an element in the listpack will assert
 * that this element is valid, so it can be freely used.
 * Generally functions such lpNext and lpDelete assume the input pointer is
 * already validated (since it's the return value of another function).
 * 用于验证指针p是否在 listpack 的有效范围内
 * 大于等于listpack起始位置加上头部大小(确保不在头部内)
 * 小于listpack起始位置加上总字节数(确保不超过listpack边界)
 */
#define ASSERT_INTEGRITY(lp, p) do { \
    assert((p) >= (lp)+LP_HDR_SIZE && (p) < (lp)+lpGetTotalBytes((lp))); \
} while (0)

/* Similar to the above, but validates the entire element length rather than just
 * it's pointer. */
#define ASSERT_INTEGRITY_LEN(lp, p, len) do { \
    assert((p) >= (lp)+LP_HDR_SIZE && (p)+(len) < (lp)+lpGetTotalBytes((lp))); \
} while (0)

static inline void lpAssertValidEntry(unsigned char* lp, size_t lpbytes, unsigned char *p);

/* Don't let listpacks grow over 1GB in any case, don't wanna risk overflow in
 * Total Bytes header field */
#define LISTPACK_MAX_SAFETY_SIZE (1<<30)
int lpSafeToAdd(unsigned char* lp, size_t add) {
    size_t len = lp? lpGetTotalBytes(lp): 0;
    if (len + add > LISTPACK_MAX_SAFETY_SIZE)
        return 0;
    return 1;
}

/* Convert a string into a signed 64 bit integer.
 * The function returns 1 if the string could be parsed into a (non-overflowing)
 * signed 64 bit int, 0 otherwise. The 'value' will be set to the parsed value
 * when the function returns success.
 *
 * Note that this function demands that the string strictly represents
 * a int64 value: no spaces or other characters before or after the string
 * representing the number are accepted, nor zeroes at the start if not
 * for the string "0" representing the zero number.
 *
 * Because of its strictness, it is safe to use this function to check if
 * you can convert a string into a long long, and obtain back the string
 * from the number without any loss in the string representation. *
 *
 * -----------------------------------------------------------------------------
 *
 * Credits: this function was adapted from the Redis source code, file
 * "utils.c", function string2ll(), and is copyright:
 *
 * Copyright(C) 2011, Pieter Noordhuis
 * Copyright(C) 2011-current, Redis Ltd.
 *
 * The function is released under the BSD 3-clause license.
 */
int lpStringToInt64(const char *s, unsigned long slen, int64_t *value) {
    const char *p = s;
    unsigned long plen = 0;
    int negative = 0;
    uint64_t v;

    /* Abort if length indicates this cannot possibly be an int */
    if (slen == 0 || slen >= LONG_STR_SIZE)
        return 0;

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        if (value != NULL) *value = 0;
        return 1;
    }

    if (p[0] == '-') {
        negative = 1;
        p++; plen++;

        /* Abort on only a negative sign. */
        if (plen == slen)
            return 0;
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0]-'0';
        p++; plen++;
    } else {
        return 0;
    }

    while (plen < slen && p[0] >= '0' && p[0] <= '9') {
        if (v > (UINT64_MAX / 10)) /* Overflow. */
            return 0;
        v *= 10;

        if (v > (UINT64_MAX - (p[0]-'0'))) /* Overflow. */
            return 0;
        v += p[0]-'0';

        p++; plen++;
    }

    /* Return if not all bytes were used. */
    if (plen < slen)
        return 0;

    if (negative) {
        if (v > ((uint64_t)(-(INT64_MIN+1))+1)) /* Overflow. */
            return 0;
        if (value != NULL) *value = -v;
    } else {
        if (v > INT64_MAX) /* Overflow. */
            return 0;
        if (value != NULL) *value = v;
    }
    return 1;
}

/* Create a new, empty listpack.
 * On success the new listpack is returned, otherwise an error is returned.
 * Pre-allocate at least `capacity` bytes of memory,
 * over-allocated memory can be shrunk by `lpShrinkToFit`.
 * */
unsigned char *lpNew(size_t capacity) {
    unsigned char *lp = lp_malloc(capacity > LP_HDR_SIZE+1 ? capacity : LP_HDR_SIZE+1);
    if (lp == NULL) return NULL;
    // 前 4 个字节存储总字节数量
    lpSetTotalBytes(lp,LP_HDR_SIZE+1);
    // 2字节存储元素数量
    lpSetNumElements(lp,0);
    // 结束标记
    lp[LP_HDR_SIZE] = LP_EOF;
    return lp;
}

/* Free the specified listpack. */
void lpFree(unsigned char *lp) {
    lp_free(lp);
}

/* Generic version of lpFree. */
void lpFreeGeneric(void *lp) {
    lp_free((unsigned char *)lp);
}

/* Shrink the memory to fit. */
unsigned char* lpShrinkToFit(unsigned char *lp) {
    size_t size = lpGetTotalBytes(lp);
    if (size < lp_malloc_size(lp)) {
        return lp_realloc(lp, size);
    } else {
        return lp;
    }
}

/* Stores the integer encoded representation of 'v' in the 'intenc' buffer.
 * 将64位整数编码成不同的紧凑格式
 * 编码数据存入 intenc, 编码长度存到 enclen
 */
static inline void lpEncodeIntegerGetType(int64_t v, unsigned char *intenc, uint64_t *enclen) {
    if (v >= 0 && v <= 127) {
        /* Single byte 0-127 integer.
         * 数字范围 0-127,直接存储数字本身
         * 最高位为 0 ,表示这是7位整数编码
         */
        if (intenc != NULL) intenc[0] = v;
        if (enclen != NULL) *enclen = 1;
    } else if (v >= -4096 && v <= 4095) {
        /* 13 bit integer.
         * 1<<13 = 8192
         * -1 + 8192 = 8191
         * 对应规则
         * 4095 -> 4095
         * 4094 -> 4094
         * 0 -> 0
         * -1 -> 8191  8191 为 negmax 2^13 - 1
         * -2 -> 8190
         * -4096 -> 4096  4096 为 negstart ,2^12
         *
         * 如果解码出来的值为 uval
         * 原值为 uval - 8191 -1 ,即 uval - negmax - 1
         *
         * 即 0-4095 即 0 到 (2^12 -1) 为正数
         * 即 4096-8191 映射为负数: -4096 到 -1
         */
        if (v < 0) v = ((int64_t)1<<13)+v;
        if (intenc != NULL) {
            /*
             * V >> 8 得到高 8 位
             * (v>>8)|LP_ENCODING_13BIT_INT; 110 为 标记, 剩余5位为数字, 加低8位就是 13 bit
             */
            intenc[0] = (v>>8)|LP_ENCODING_13BIT_INT;
            intenc[1] = v&0xff;
        }
        if (enclen != NULL) *enclen = 2;
    } else if (v >= -32768 && v <= 32767) {
        /* 16 bit integer. */
        if (v < 0) v = ((int64_t)1<<16)+v;
        if (intenc != NULL) {
            intenc[0] = LP_ENCODING_16BIT_INT;
            intenc[1] = v&0xff;
            intenc[2] = v>>8;
        }
        if (enclen != NULL) *enclen = 3;
    } else if (v >= -8388608 && v <= 8388607) {
        /* 24 bit integer. */
        if (v < 0) v = ((int64_t)1<<24)+v;
        if (intenc != NULL) {
            intenc[0] = LP_ENCODING_24BIT_INT;
            intenc[1] = v&0xff;
            intenc[2] = (v>>8)&0xff;
            intenc[3] = v>>16;
        }
        if (enclen != NULL) *enclen = 4;
    } else if (v >= -2147483648 && v <= 2147483647) {
        /* 32 bit integer. */
        if (v < 0) v = ((int64_t)1<<32)+v;
        if (intenc != NULL) {
            intenc[0] = LP_ENCODING_32BIT_INT;
            intenc[1] = v&0xff;
            intenc[2] = (v>>8)&0xff;
            intenc[3] = (v>>16)&0xff;
            intenc[4] = v>>24;
        }
        if (enclen != NULL) *enclen = 5;
    } else {
        /* 64 bit integer. */
        uint64_t uv = v;
        if (intenc != NULL) {
            intenc[0] = LP_ENCODING_64BIT_INT;
            intenc[1] = uv&0xff;
            intenc[2] = (uv>>8)&0xff;
            intenc[3] = (uv>>16)&0xff;
            intenc[4] = (uv>>24)&0xff;
            intenc[5] = (uv>>32)&0xff;
            intenc[6] = (uv>>40)&0xff;
            intenc[7] = (uv>>48)&0xff;
            intenc[8] = uv>>56;
        }
        if (enclen != NULL) *enclen = 9;
    }
}

/* Given an element 'ele' of size 'size', determine if the element can be
 * represented inside the listpack encoded as integer, and returns
 * LP_ENCODING_INT if so. Otherwise returns LP_ENCODING_STR if no integer
 * encoding is possible.
 *
 * If the LP_ENCODING_INT is returned, the function stores the integer encoded
 * representation of the element in the 'intenc' buffer.
 *
 * Regardless of the returned encoding, 'enclen' is populated by reference to
 * the number of bytes that the string or integer encoded element will require
 * in order to be represented. */
static inline int lpEncodeGetType(unsigned char *ele, uint32_t size, unsigned char *intenc, uint64_t *enclen) {
    int64_t v;
    // 如果要操作的元素可以转换成 int64, 则编码为 INT
    if (lpStringToInt64((const char*)ele, size, &v)) {
        lpEncodeIntegerGetType(v, intenc, enclen);
        return LP_ENCODING_INT;
    } else {
        if (size < 64) *enclen = 1+size;
        else if (size < 4096) *enclen = 2+size;
        else *enclen = 5+(uint64_t)size;
        return LP_ENCODING_STRING;
    }
}

/* Store a reverse-encoded variable length field, representing the length
 * of the previous element of size 'l', in the target buffer 'buf'.
 * The function returns the number of bytes used to encode it, from
 * 1 to 5. If 'buf' is NULL the function just returns the number of bytes
 * needed in order to encode the backlen.
 *
 * 可变长度编码：根据数值 l 的大小选择 1-5 字节存储，小值用少字节，大值用多字节，平衡空间效率
 * 每个字节的最高位（第 7 位）作为 “延续位”：1 表示后续还有字节，0 表示当前是最后一个字节
 */
static inline unsigned long lpEncodeBacklen(unsigned char *buf, uint64_t l) {
    if (l <= 127) {
        if (buf) buf[0] = l;
        return 1;
    } else if (l < 16383) {
        // 2字节编码：128 <= 长度 < 16383（2^14 - 1）
        if (buf) {
            buf[0] = l>>7;  // 高7位（数值部分）
            buf[1] = (l&127)|128; // 低7位 + 最高位1（表示后续还有字节）
        }
        return 2;
    } else if (l < 2097151) {
        // 3字节编码：16383 <= 长度 < 2097151（2^21 - 1）
        if (buf) {
            buf[0] = l>>14;  // 高7位
            buf[1] = ((l>>7)&127)|128; // 中间7位 + 延续位1
            buf[2] = (l&127)|128;  // 低7位 + 延续位1
        }
        return 3;
    } else if (l < 268435455) {
        // 4字节编码：2097151 <= 长度 < 268435455（2^28 - 1）
        if (buf) {
            buf[0] = l>>21;
            buf[1] = ((l>>14)&127)|128;
            buf[2] = ((l>>7)&127)|128;
            buf[3] = (l&127)|128;
        }
        return 4;
    } else {
        // 5字节编码：长度 >= 268435455（最大支持 2^35 - 1）
        if (buf) {
            buf[0] = l>>28;
            buf[1] = ((l>>21)&127)|128;
            buf[2] = ((l>>14)&127)|128;
            buf[3] = ((l>>7)&127)|128;
            buf[4] = (l&127)|128;
        }
        return 5;
    }
}

/* Calculate the number of bytes required to reverse-encode a variable length
 * field representing the length of the previous element of size 'l', ranging
 * from 1 to 5. */
static inline unsigned long lpEncodeBacklenBytes(uint64_t l) {
    if (l <= 127) {
        return 1;
    } else if (l < 16383) {
        return 2;
    } else if (l < 2097151) {
        return 3;
    } else if (l < 268435455) {
        return 4;
    } else {
        return 5;
    }
}

/* Decode the backlen and returns it. If the encoding looks invalid (more than
 * 5 bytes are used), UINT64_MAX is returned to report the problem. */
static inline uint64_t lpDecodeBacklen(unsigned char *p) {
    uint64_t val = 0;
    uint64_t shift = 0;
    do {
        val |= (uint64_t)(p[0] & 127) << shift;
        if (!(p[0] & 128)) break;
        shift += 7;
        p--;
        if (shift > 28) return UINT64_MAX;
    } while(1);
    return val;
}

/* Encode the string element pointed by 's' of size 'len' in the target
 * buffer 's'. The function should be called with 'buf' having always enough
 * space for encoding the string. This is done by calling lpEncodeGetType()
 * before calling this function. */
static inline void lpEncodeString(unsigned char *buf, unsigned char *s, uint32_t len) {
    if (len < 64) {
        buf[0] = len | LP_ENCODING_6BIT_STR;
        memcpy(buf+1,s,len);
    } else if (len < 4096) {
        buf[0] = (len >> 8) | LP_ENCODING_12BIT_STR;
        buf[1] = len & 0xff;
        memcpy(buf+2,s,len);
    } else {
        buf[0] = LP_ENCODING_32BIT_STR;
        buf[1] = len & 0xff;
        buf[2] = (len >> 8) & 0xff;
        buf[3] = (len >> 16) & 0xff;
        buf[4] = (len >> 24) & 0xff;
        memcpy(buf+5,s,len);
    }
}

/* Return the encoded length of the listpack element pointed by 'p'.
 * This includes the encoding byte, length bytes, and the element data itself.
 * If the element encoding is wrong then 0 is returned.
 * Note that this method may access additional bytes (in case of 12 and 32 bit
 * str), so should only be called when we know 'p' was already validated by
 * lpCurrentEncodedSizeBytes or ASSERT_INTEGRITY_LEN (possibly since 'p' is
 * a return value of another function that validated its return.
 *
 * 用于计算 listpack 中指定元素（由 p 指向）的总编码长度，
 * 包括编码标识字节、长度字节以及元素数据本身的字节数。如果元素的编码格式无效，则返回 0
 *
 * 函数名中的 Unsafe 表示它假设输入的 p 已经过有效性验证（如通过 lpCurrentEncodedSizeBytes 或断言校验）。
 * 因为对于 12 位和 32 位字符串编码，函数需要访问 p 指向的后续字节来计算长度，若 p 无效可能导致越界访问。
 */
static inline uint32_t lpCurrentEncodedSizeUnsafe(unsigned char *p) {
    if (LP_ENCODING_IS_7BIT_UINT(p[0])) return 1;
    if (LP_ENCODING_IS_6BIT_STR(p[0])) return 1+LP_ENCODING_6BIT_STR_LEN(p);
    if (LP_ENCODING_IS_13BIT_INT(p[0])) return 2;
    if (LP_ENCODING_IS_16BIT_INT(p[0])) return 3;
    if (LP_ENCODING_IS_24BIT_INT(p[0])) return 4;
    if (LP_ENCODING_IS_32BIT_INT(p[0])) return 5;
    if (LP_ENCODING_IS_64BIT_INT(p[0])) return 9;
    // 12位字符串：2字节编码 + 字符串长度
    if (LP_ENCODING_IS_12BIT_STR(p[0])) return 2+LP_ENCODING_12BIT_STR_LEN(p);
    // 32位字符串：5字节编码 + 字符串长度
    if (LP_ENCODING_IS_32BIT_STR(p[0])) return 5+LP_ENCODING_32BIT_STR_LEN(p);
    if (p[0] == LP_EOF) return 1;
    // 无效编码：返回0
    return 0;
}

/* Return bytes needed to encode the length of the listpack element pointed by 'p'.
 * This includes just the encoding byte, and the bytes needed to encode the length
 * of the element (excluding the element data itself)
 * 用于计算 listpack 中某个元素的长度编码部分所需的字节数
 * （仅包含编码标识和长度信息，不包含元素数据本身）
 * If the element encoding is wrong then 0 is returned. */
static inline uint32_t lpCurrentEncodedSizeBytes(const unsigned char encoding) {
    // 7位无符号整数编码：仅需1字节（包含编码标识和数值）
    if (LP_ENCODING_IS_7BIT_UINT(encoding)) return 1;
    if (LP_ENCODING_IS_6BIT_STR(encoding)) return 1;
    if (LP_ENCODING_IS_13BIT_INT(encoding)) return 1;
    if (LP_ENCODING_IS_16BIT_INT(encoding)) return 1;
    if (LP_ENCODING_IS_24BIT_INT(encoding)) return 1;
    if (LP_ENCODING_IS_32BIT_INT(encoding)) return 1;
    if (LP_ENCODING_IS_64BIT_INT(encoding)) return 1;
    // 12位字符串长度编码：需要2字节（1字节标识+1字节长度）
    if (LP_ENCODING_IS_12BIT_STR(encoding)) return 2;
    // 32位字符串长度编码：需要5字节（1字节标识+4字节长度）
    if (LP_ENCODING_IS_32BIT_STR(encoding)) return 5;
    if (encoding == LP_EOF) return 1;
    return 0;
}

/* Skip the current entry returning the next. It is invalid to call this
 * function if the current element is the EOF element at the end of the
 * listpack, however, while this function is used to implement lpNext(),
 * it does not return NULL when the EOF element is encountered. */
static inline unsigned char *lpSkip(unsigned char *p) {
    unsigned long entrylen = lpCurrentEncodedSizeUnsafe(p);
    entrylen += lpEncodeBacklenBytes(entrylen);
    p += entrylen;
    return p;
}

/* This is similar to lpNext() but avoids the inner call to lpBytes when you already know the listpack size. */
unsigned char *lpNextWithBytes(unsigned char *lp, unsigned char *p, const size_t lpbytes) {
    assert(p);
    p = lpSkip(p);
    if (p[0] == LP_EOF) return NULL;
    lpAssertValidEntry(lp, lpbytes, p);
    return p;
}

/* If 'p' points to an element of the listpack, calling lpNext() will return
 * the pointer to the next element (the one on the right), or NULL if 'p'
 * already pointed to the last element of the listpack. */
unsigned char *lpNext(unsigned char *lp, unsigned char *p) {
    assert(p);
    p = lpSkip(p);
    if (p[0] == LP_EOF) return NULL;
    lpAssertValidEntry(lp, lpBytes(lp), p);
    return p;
}

/* If 'p' points to an element of the listpack, calling lpPrev() will return
 * the pointer to the previous element (the one on the left), or NULL if 'p'
 * already pointed to the first element of the listpack. */
unsigned char *lpPrev(unsigned char *lp, unsigned char *p) {
    assert(p);
    if (p-lp == LP_HDR_SIZE) return NULL;
    p--; /* Seek the first backlen byte of the last element. */
    uint64_t prevlen = lpDecodeBacklen(p);
    prevlen += lpEncodeBacklenBytes(prevlen);
    p -= prevlen-1; /* Seek the first byte of the previous entry. */
    lpAssertValidEntry(lp, lpBytes(lp), p);
    return p;
}

/* Return a pointer to the first element of the listpack, or NULL if the
 * listpack has no elements. */
unsigned char *lpFirst(unsigned char *lp) {
    unsigned char *p = lp + LP_HDR_SIZE; /* Skip the header. */
    if (p[0] == LP_EOF) return NULL;
    lpAssertValidEntry(lp, lpBytes(lp), p);
    return p;
}

/* Return a pointer to the last element of the listpack, or NULL if the
 * listpack has no elements. */
unsigned char *lpLast(unsigned char *lp) {
    unsigned char *p = lp+lpGetTotalBytes(lp)-1; /* Seek EOF element. */
    return lpPrev(lp,p); /* Will return NULL if EOF is the only element. */
}

/* Return the number of elements inside the listpack. This function attempts
 * to use the cached value when within range, otherwise a full scan is
 * needed. As a side effect of calling this function, the listpack header
 * could be modified, because if the count is found to be already within
 * the 'numele' header field range, the new value is set. */
unsigned long lpLength(unsigned char *lp) {
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) return numele;

    /* Too many elements inside the listpack. We need to scan in order
     * to get the total number. */
    uint32_t count = 0;
    unsigned char *p = lpFirst(lp);
    while(p) {
        count++;
        p = lpNext(lp,p);
    }

    /* If the count is again within range of the header numele field,
     * set it. */
    if (count < LP_HDR_NUMELE_UNKNOWN) lpSetNumElements(lp,count);
    return count;
}

/* Return the listpack element pointed by 'p'.
 *
 * The function changes behavior depending on the passed 'intbuf' value.
 * Specifically, if 'intbuf' is NULL:
 *
 * If the element is internally encoded as an integer, the function returns
 * NULL and populates the integer value by reference in 'count'. Otherwise if
 * the element is encoded as a string a pointer to the string (pointing inside
 * the listpack itself) is returned, and 'count' is set to the length of the
 * string.
 *
 * If instead 'intbuf' points to a buffer passed by the caller, that must be
 * at least LP_INTBUF_SIZE bytes, the function always returns the element as
 * it was a string (returning the pointer to the string and setting the
 * 'count' argument to the string length by reference). However if the element
 * is encoded as an integer, the 'intbuf' buffer is used in order to store
 * the string representation.
 *
 * The user should use one or the other form depending on what the value will
 * be used for. If there is immediate usage for an integer value returned
 * by the function, than to pass a buffer (and convert it back to a number)
 * is of course useless.
 *
 * If 'entry_size' is not NULL, *entry_size is set to the entry length of the
 * listpack element pointed by 'p'. This includes the encoding bytes, length
 * bytes, the element data itself, and the backlen bytes.
 *
 * If the function is called against a badly encoded ziplist, so that there
 * is no valid way to parse it, the function returns like if there was an
 * integer encoded with value 12345678900000000 + <unrecognized byte>, this may
 * be an hint to understand that something is wrong. To crash in this case is
 * not sensible because of the different requirements of the application using
 * this lib.
 *
 * 如果调用此函数时遇到及一个编码错误的压缩列表（ziplist），导致无法以有效有效的方式解析它，
 * 函数的返回结果可能会类似解析出一个值为 12345678900000000 加上 <无法识别的字节> 的整数编码形式，
 * 这或许能提示我们存在问题。在这种情况下，让程序崩溃并不合理，因为使用该库的应用程序有着不同的需求。
 *
 * Similarly, there is no error returned since the listpack normally can be
 * assumed to be valid, so that would be a very high API cost.
 *
 *
 * if 'intbuf' is NULL:
 *  如果 element 本身是 integer, 该函数返回 NULL, 并通过引用（by reference）方式在 count 参数中填充（populates）整数值
 *  如果 element 本身是 string, ，函数会返回指向该字符串的指针（指向 listpack 内部），同时将 count 设置为字符串的长度
 *
 * 如果 intbuf 指向调用者传入的缓冲区（该缓冲区必须至少有 LP_INTBUF_SIZE 字节），
 * 则此函数始终将元素作为字符串返回（返回字符串指针，并通过引用将 count 参数设置为字符串长度）。
 * 不过，若元素是以整数形式编码的，则会使用 intbuf 缓冲区来存储其字符串表示形式
 *
 * 如果 entry_size 不为 NULL，则会将 p 所指向的 listpack 元素的条目长度赋值给 *entry_size。
 * 这包括编码字节、长度字节、元素数据本身以及反向长度（backlen）字节
 */
static inline unsigned char *lpGetWithSize(unsigned char *p, int64_t *count, unsigned char *intbuf, uint64_t *entry_size) {
    int64_t val;
    uint64_t uval, negstart, negmax;

    assert(p); /* assertion for valgrind (avoid NPD) */
    if (LP_ENCODING_IS_7BIT_UINT(p[0])) {
        negstart = UINT64_MAX; /* 7 bit ints are always positive. */
        negmax = 0;
        uval = p[0] & 0x7f;
        if (entry_size) *entry_size = LP_ENCODING_7BIT_UINT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_6BIT_STR(p[0])) {
        *count = LP_ENCODING_6BIT_STR_LEN(p);
        if (entry_size) *entry_size = 1 + *count + lpEncodeBacklenBytes(*count + 1);
        return p+1;
    } else if (LP_ENCODING_IS_13BIT_INT(p[0])) {
        uval = ((p[0]&0x1f)<<8) | p[1];
        negstart = (uint64_t)1<<12;
        negmax = 8191;
        if (entry_size) *entry_size = LP_ENCODING_13BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_16BIT_INT(p[0])) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8;
        negstart = (uint64_t)1<<15;
        negmax = UINT16_MAX;
        if (entry_size) *entry_size = LP_ENCODING_16BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_24BIT_INT(p[0])) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8 |
               (uint64_t)p[3]<<16;
        negstart = (uint64_t)1<<23;
        negmax = UINT32_MAX>>8;
        if (entry_size) *entry_size = LP_ENCODING_24BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_32BIT_INT(p[0])) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8 |
               (uint64_t)p[3]<<16 |
               (uint64_t)p[4]<<24;
        negstart = (uint64_t)1<<31;
        negmax = UINT32_MAX;
        if (entry_size) *entry_size = LP_ENCODING_32BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_64BIT_INT(p[0])) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8 |
               (uint64_t)p[3]<<16 |
               (uint64_t)p[4]<<24 |
               (uint64_t)p[5]<<32 |
               (uint64_t)p[6]<<40 |
               (uint64_t)p[7]<<48 |
               (uint64_t)p[8]<<56;
        negstart = (uint64_t)1<<63;
        negmax = UINT64_MAX;
        if (entry_size) *entry_size = LP_ENCODING_64BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_12BIT_STR(p[0])) {
        *count = LP_ENCODING_12BIT_STR_LEN(p);
        if (entry_size) *entry_size = 2 + *count + lpEncodeBacklenBytes(*count + 2);
        return p+2;
    } else if (LP_ENCODING_IS_32BIT_STR(p[0])) {
        *count = LP_ENCODING_32BIT_STR_LEN(p);
        if (entry_size) *entry_size = 5 + *count + lpEncodeBacklenBytes(*count + 5);
        return p+5;
    } else {
        uval = 12345678900000000ULL + p[0];
        negstart = UINT64_MAX;
        negmax = 0;
    }

    /* We reach this code path only for integer encodings.
     * Convert the unsigned value to the signed one using two's complement
     * rule. */
    if (uval >= negstart) {
        /* This three steps conversion should avoid undefined behaviors
         * in the unsigned -> signed conversion. */
        uval = negmax-uval;
        val = uval;
        val = -val-1;
    } else {
        val = uval;
    }

    /* Return the string representation of the integer or the value itself
     * depending on intbuf being NULL or not. */
    if (intbuf) {
        *count = ll2string((char*)intbuf,LP_INTBUF_SIZE,(long long)val);
        return intbuf;
    } else {
        *count = val;
        return NULL;
    }
}

/* Return the listpack element pointed by 'p'.
 *
 * The function has the same behaviour as lpGetWithSize when 'entry_size' is NULL,
 * but avoids a lot of unecesarry branching performance penalties. */
static inline unsigned char *lpGetWithBuf(unsigned char *p, int64_t *count, unsigned char *intbuf) {
    int64_t val;
    uint64_t uval, negstart, negmax;
    assert(p); /* assertion for valgrind (avoid NPD) */
    const unsigned char encoding = p[0];

    /* string encoding */
    if (LP_ENCODING_IS_6BIT_STR(encoding)) {
        *count = LP_ENCODING_6BIT_STR_LEN(p);
        return p+1;
    }
    if (LP_ENCODING_IS_12BIT_STR(encoding)) {
        *count = LP_ENCODING_12BIT_STR_LEN(p);
        return p+2;
    }
    if (LP_ENCODING_IS_32BIT_STR(encoding)) {
        *count = LP_ENCODING_32BIT_STR_LEN(p);
        return p+5;
    }
    /* int encoding */
    if (LP_ENCODING_IS_7BIT_UINT(encoding)) {
        negstart = UINT64_MAX; /* 7 bit ints are always positive. */
        negmax = 0;
        uval = encoding & 0x7f;
    } else if (LP_ENCODING_IS_13BIT_INT(encoding)) {
        // encoding & 0x1f：提取 encoding 的低 5 位, 这是 13 位整数的高 5 位。
        // p[1]：取第二个字节的 8 位，作为 13 位整数的低 8 位
        // 两者通过 <<8 和 | 拼接，得到一个 13 位的无符号整数 uval
        // 原值为 uval - 8191 -1 ,即 uval - negmax - 1
        uval = ((encoding&0x1f)<<8) | p[1];
        negstart = (uint64_t)1<<12;
        negmax = 8191;
    } else if (LP_ENCODING_IS_16BIT_INT(encoding)) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8;
        negstart = (uint64_t)1<<15;
        negmax = UINT16_MAX;
    } else if (LP_ENCODING_IS_24BIT_INT(encoding)) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8 |
               (uint64_t)p[3]<<16;
        negstart = (uint64_t)1<<23;
        negmax = UINT32_MAX>>8;
    } else if (LP_ENCODING_IS_32BIT_INT(encoding)) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8 |
               (uint64_t)p[3]<<16 |
               (uint64_t)p[4]<<24;
        negstart = (uint64_t)1<<31;
        negmax = UINT32_MAX;
    } else if (LP_ENCODING_IS_64BIT_INT(encoding)) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8 |
               (uint64_t)p[3]<<16 |
               (uint64_t)p[4]<<24 |
               (uint64_t)p[5]<<32 |
               (uint64_t)p[6]<<40 |
               (uint64_t)p[7]<<48 |
               (uint64_t)p[8]<<56;
        negstart = (uint64_t)1<<63;
        negmax = UINT64_MAX;
    } else {
        uval = 12345678900000000ULL + encoding;
        negstart = UINT64_MAX;
        negmax = 0;
    }

    /* We reach this code path only for integer encodings.
     * Convert the unsigned value to the signed one using two's complement
     * rule. */
    if (uval >= negstart) {
        /*
         * 大于负数的开始值,按负数处理
         * [0,1,..4095,4096(-4096 negstart),4097(-4095),...8191(-1 negmax)]
         */
        /* This three steps conversion should avoid undefined behaviors
         * in the unsigned -> signed conversion.
         */
        // 相对于 negmax 的偏移量(正数)
        uval = negmax-uval;
        // 将正数偏移量赋值给有符号变量 val
        val = uval;
        // 将正数偏移量转换为对应的负数
        val = -val-1;
    } else {
        // 按正数处理
        val = uval;
    }

    /* Return the string representation of the integer or the value itself
     * depending on intbuf being NULL or not. */
    if (intbuf) {
        *count = ll2string((char*)intbuf,LP_INTBUF_SIZE,(long long)val);
        return intbuf;
    } else {
        *count = val;
        return NULL;
    }
}

unsigned char *lpGet(unsigned char *p, int64_t *count, unsigned char *intbuf) {
    return lpGetWithBuf(p, count, intbuf);
}

/* This is just a wrapper to lpGet() that is able to get entry value directly.
 * When the function returns NULL, it populates the integer value by reference in 'lval'.
 * Otherwise if the element is encoded as a string a pointer to the string (pointing
 * inside the listpack itself) is returned, and 'slen' is set to the length of the
 * string. */
unsigned char *lpGetValue(unsigned char *p, unsigned int *slen, long long *lval) {
    unsigned char *vstr;
    int64_t ele_len;

    vstr = lpGet(p, &ele_len, NULL);
    if (vstr) {
        *slen = ele_len;
    } else {
        *lval = ele_len;
    }
    return vstr;
}

/* This is just a wrapper to lpGet() that is able to get an integer from an entry directly.
 * Returns 1 and stores the integer in 'lval' if the entry is an integer.
 * Returns 0 if the entry is a string. */
int lpGetIntegerValue(unsigned char *p, long long *lval) {
    int64_t ele_len;
    if (!lpGet(p, &ele_len, NULL)) {
        *lval = ele_len;
        return 1;
    }
    return 0;
}

/* Find pointer to the entry with a comparator callback.
 *
 * 'cmp' is a comparator callback. If it returns zero, current entry pointer
 * will be returned. 'user' is passed to this callback.
 * Skip 'skip' entries between every comparison.
 * Returns NULL when the field could not be found. */
static inline unsigned char *lpFindCbInternal(unsigned char *lp, unsigned char *p,
                                              void *user, lpCmp cmp, unsigned int skip)
{
    int skipcnt = 0;
    unsigned char *value;
    int64_t ll;
    uint64_t entry_size = 123456789; /* initialized to avoid warning. */
    uint32_t lp_bytes = lpBytes(lp);

    // 如果起始指针 p 为 NULL，则从第一个元素开始
    if (!p)
        p = lpFirst(lp);

    while (p) {
        //  比较一次不成功后跳过 skipcnt 次数再次比较
        if (skipcnt == 0) {
            value = lpGetWithSize(p, &ll, NULL, &entry_size);
            if (value) {
                /* check the value doesn't reach outside the listpack before accessing it */
                assert(p >= lp + LP_HDR_SIZE && p + entry_size < lp + lp_bytes);
            }

            if (unlikely(cmp(lp, p, user, value, ll) == 0))
                return p;

            /* Reset skip count */
            skipcnt = skip;
            p += entry_size;
        } else {
            /* Skip entry */
            skipcnt--;

            /* Move to next entry, avoid use `lpNext` due to `lpAssertValidEntry` in
            * `lpNext` will call `lpBytes`, will cause performance degradation */
            p = lpSkip(p);
        }

        /* The next call to lpGetWithSize could read at most 8 bytes past `p`
         * We use the slower validation call only when necessary. */
        if (p + 8 >= lp + lp_bytes)
            lpAssertValidEntry(lp, lp_bytes, p);
        else
            assert(p >= lp + LP_HDR_SIZE && p < lp + lp_bytes);
        if (p[0] == LP_EOF) break;
    }

    return NULL;
}


/*
 * 实现通过自定义比较逻辑在 listpack 中查找条目的功能，支持跳过指定数量的条目以优化效率。
 */
unsigned char *lpFindCb(unsigned char *lp, unsigned char *p,
                        void *user, lpCmp cmp, unsigned int skip)
{
    return lpFindCbInternal(lp, p, user, cmp, skip);
}

struct lpFindArg {
    unsigned char *s; /* Item to search */
    uint32_t slen;    /* Item len */
    int vencoding;
    int64_t vll;
};

/* Comparator function to find item */
static inline int lpFindCmp(const unsigned char *lp, unsigned char *p,
                            void *user, unsigned char *s, long long slen) {
    (void) lp;
    (void) p;
    struct lpFindArg *arg = user;

    if (s) {
        if (slen == arg->slen && memcmp(arg->s, s, slen) == 0) {
            return 0;
        }
    } else {
        /* Find out if the searched field can be encoded. Note that
         * we do it only the first time, once done vencoding is set
         * to non-zero and vll is set to the integer value. */
        if (arg->vencoding == 0) {
            /* If the entry can be encoded as integer we set it to
             * 1, else set it to UCHAR_MAX, so that we don't retry
             * again the next time. */
            if (arg->slen >= 32 || arg->slen == 0 || !lpStringToInt64((const char*)arg->s, arg->slen, &arg->vll)) {
                arg->vencoding = UCHAR_MAX;
            } else {
                arg->vencoding = 1;
            }
        }

        /* Compare current entry with specified entry, do it only
         * if vencoding != UCHAR_MAX because if there is no encoding
         * possible for the field it can't be a valid integer. */
        if (arg->vencoding != UCHAR_MAX && slen == arg->vll) {
            return 0;
        }
    }

    return 1;
}

/* Find pointer to the entry equal to the specified entry. Skip 'skip' entries
 * between every comparison. Returns NULL when the field could not be found.
 *
 * s：待匹配的字符串（或二进制数据）指针。
 * slen：s 的长度（字节数)
 * skip：每次比较之间需要跳过的条目数量
 */
unsigned char *lpFind(unsigned char *lp, unsigned char *p, unsigned char *s,
                      uint32_t slen, unsigned int skip)
{
    struct lpFindArg arg = {
        .s = s,
        .slen = slen
    };
    return lpFindCbInternal(lp, p, &arg, lpFindCmp, skip);
}

/* Insert, delete or replace the specified string element 'elestr' of length
 * 'size' or integer element 'eleint' at the specified position 'p', with 'p'
 * being a listpack element pointer obtained with lpFirst(), lpLast(), lpNext(),
 * lpPrev() or lpSeek().
 *
 *  在指定的 p 位置, Insert, delete or replace 给定的 size 大小的 elestr, 或者 eleint
 *  p 是  lpFirst(), lpLast(), lpNext(), lpPrev() or lpSeek() 返回的指向 listpack 元素的指针
 *
 * The element is inserted before, after, or replaces the element pointed
 * by 'p' depending on the 'where' argument, that can be LP_BEFORE, LP_AFTER
 * or LP_REPLACE.
 *
 *  根据 where 参数的取值（可以是 LP_BEFORE、LP_AFTER 或 LP_REPLACE），
 *  元素会被插入到 p 所指向元素的前面、后面，或者替换 p 所指向的元素。
 *
 * If both 'elestr' and `eleint` are NULL, the function removes the element
 * pointed by 'p' instead of inserting one.
 *
 * If `eleint` is non-NULL, 'size' is the length of 'eleint', the function insert
 * or replace with a 64 bit integer, which is stored in the 'eleint' buffer.
 * If 'elestr` is non-NULL, 'size' is the length of 'elestr', the function insert
 * or replace with a string, which is stored in the 'elestr' buffer.
 * 
 * Returns NULL on out of memory or when the listpack total length would exceed
 * the max allowed size of 2^32-1, otherwise the new pointer to the listpack
 * holding the new element is returned (and the old pointer passed is no longer
 * considered valid)
 *
 * 如果内存不足，或者当 listpack 的总长度超过最大允许大小（2³²-1）时，返回 NULL；
 * 否则，返回指向包含新元素的 listpack 的新指针
 *
 * If 'newp' is not NULL, at the end of a successful call '*newp' will be set
 * to the address of the element just added, so that it will be possible to
 * continue an interaction with lpNext() and lpPrev().
 *
 * 如果 newp 不为 NULL，那么在成功调用结束时，*newp 会被设置为刚添加的元素的地址，
 * 这样就可以继续通过 lpNext() 和 lpPrev() 进行交互
 *
 * For deletion operations (both 'elestr' and 'eleint' set to NULL) 'newp' is
 * set to the next element, on the right of the deleted one, or to NULL if the
 * deleted element was the last one.
 * 对于删除操作（即 elestr 和 eleint 均设为 NULL），newp 会被设置为被删除元素右侧的下一个元素；
 * 如果被删除的元素是最后一个，则 newp 会被设为 NULL。
 *
 * listpack 结构
 * |total-bytes(4)|num-elements(2)|entry1|entry2|...|0xFF|
 *
 * entry 结构
 * encoding-type|entry-data|entry-len
 *
 * encoding-type 可以分为单字节编码和多字节编码
 *
 * 其中 entry-len 用可变长度编码表示 encoding-type + entry-data 长度
 * entry-len 占用1-5字节, 每个字节第1位用0或1表示当前字节是否是最后一个字节, 0表示是,1表示否
 * 采用大端模式,高字节保存低地址,低字节保存高地址
 */
unsigned char *lpInsert(unsigned char *lp, unsigned char *elestr, unsigned char *eleint,
                        uint32_t size, unsigned char *p, int where, unsigned char **newp)
{
    unsigned char intenc[LP_MAX_INT_ENCODING_LEN];
    unsigned char backlen[LP_MAX_BACKLEN_SIZE];

    uint64_t enclen; /* The length of the encoded element. */
    int delete = (elestr == NULL && eleint == NULL);

    /* when deletion, it is conceptually replacing the element with a
     * zero-length element. So whatever we get passed as 'where', set
     * it to LP_REPLACE. */
    if (delete) where = LP_REPLACE;

    /* If we need to insert after the current element, we just jump to the
     * next element (that could be the EOF one) and handle the case of
     * inserting before. So the function will actually deal with just two
     * cases: LP_BEFORE and LP_REPLACE. */
    if (where == LP_AFTER) {
        p = lpSkip(p);
        where = LP_BEFORE;
        ASSERT_INTEGRITY(lp, p);
    }

    /* Store the offset of the element 'p', so that we can obtain its
     * address again after a reallocation.
     * lp 是指向整个 listpack 结构起始地址的指针
     * p 是指向 listpack 中某个特定元素的指针
     * poff 计算的是 p 指针相对于 lp 指针的偏移量（以字节为单位）
     *
     * 这行代码的作用是保存元素 p 在 listpack 中的位置偏移量，因为在后续操作中可能会对 listpack 进行重新分配内存（realloc），
     * 导致指针地址发生变化。通过保存偏移量，可以在重新分配内存后通过 lp + poff 重新计算出元素的新地址
     */
    unsigned long poff = p-lp;

    int enctype;
    if (elestr) {
        /* Calling lpEncodeGetType() results into the encoded version of the
        * element to be stored into 'intenc' in case it is representable as
        * an integer: in that case, the function returns LP_ENCODING_INT.
        * Otherwise if LP_ENCODING_STR is returned, we'll have to call
        * lpEncodeString() to actually write the encoded string on place later.
        *
        * Whatever the returned encoding is, 'enclen' is populated with the
        * length of the encoded element.
        *
        * elestr 如果能够转换成 int64 ,则编码后存到 intenc 中,编码长度存到 enclen
        * 否则就当作字符串,编码长度存到 enclen
         */
        enctype = lpEncodeGetType(elestr,size,intenc,&enclen);
        if (enctype == LP_ENCODING_INT) eleint = intenc;
    } else if (eleint) {
        enctype = LP_ENCODING_INT;
        enclen = size; /* 'size' is the length of the encoded integer element. */
    } else {
        enctype = -1;
        enclen = 0;
    }

    /* We need to also encode the backward-parsable length of the element
     * and append it to the end: this allows to traverse the listpack from
     * the end to the start.
     * 用可变长度编码存储元素的长度
     * 可以从listpack末尾向前遍历元素
     */
    unsigned long backlen_size = (!delete) ? lpEncodeBacklen(backlen,enclen) : 0;
    uint64_t old_listpack_bytes = lpGetTotalBytes(lp);
    uint32_t replaced_len  = 0;
    if (where == LP_REPLACE) {
        // 获取 p 指向的当前元素自身的编码长度（不包含其反向长度）
        replaced_len = lpCurrentEncodedSizeUnsafe(p);
        // 计算该元素的 “反向长度” 编码所需的字节数
        replaced_len += lpEncodeBacklenBytes(replaced_len);
        ASSERT_INTEGRITY_LEN(lp, p, replaced_len);
    }

    uint64_t new_listpack_bytes = old_listpack_bytes + enclen + backlen_size
                                  - replaced_len;
    if (new_listpack_bytes > UINT32_MAX) return NULL;

    /* We now need to reallocate in order to make space or shrink the
     * allocation (in case 'when' value is LP_REPLACE and the new element is
     * smaller). However we do that before memmoving the memory to
     * make room for the new element if the final allocation will get
     * larger, or we do it after if the final allocation will get smaller. */

    unsigned char *dst = lp + poff; /* May be updated after reallocation. */

    /* Realloc before: we need more room. */
    if (new_listpack_bytes > old_listpack_bytes &&
        new_listpack_bytes > lp_malloc_size(lp)) {
        if ((lp = lp_realloc(lp,new_listpack_bytes)) == NULL) return NULL;
        dst = lp + poff;
    }

    /*
     * 它将插入点之后的所有数据向后移动，为新元素和其反向长度字段留出空间。
     */
    /* Setup the listpack relocating the elements to make the exact room
     * we need to store the new one. */
    if (where == LP_BEFORE) {
        // 将从 dst 开始的 old_listpack_bytes-poff 字节数据移动到 dst+enclen+backlen_size 位置

        // dst 是要插入位置的指针
        // enclen 是新元素编码后的长度
        // backlen_size 是反向长度字段的大小
        // old_listpack_bytes 是旧 listpack 的总字节数
        // poff 是插入位置的偏移量
        memmove(dst+enclen+backlen_size,dst,old_listpack_bytes-poff);
    } else { /* LP_REPLACE. */
        memmove(dst+enclen+backlen_size,
                dst+replaced_len,
                old_listpack_bytes-poff-replaced_len);
    }

    /* Realloc after: we need to free space. */
    if (new_listpack_bytes < old_listpack_bytes) {
        if ((lp = lp_realloc(lp,new_listpack_bytes)) == NULL) return NULL;
        dst = lp + poff;
    }

    /* Store the entry. */
    if (newp) {
        *newp = dst;
        /* In case of deletion, set 'newp' to NULL if the next element is
         * the EOF element. */
        if (delete && dst[0] == LP_EOF) *newp = NULL;
    }
    if (!delete) {
        if (enctype == LP_ENCODING_INT) {
            // dst 后面添加 enclen 长度的元素
            memcpy(dst,eleint,enclen);
        } else if (elestr) {
            lpEncodeString(dst,elestr,size);
        } else {
            redis_unreachable();
        }
        // 移动指针
        dst += enclen;
        // 将长度编码添加到 dst 后面
        memcpy(dst,backlen,backlen_size);
        dst += backlen_size;
    }

    /* Update header. */
    if (where != LP_REPLACE || delete) {
        uint32_t num_elements = lpGetNumElements(lp);
        /*
         * 当 listpack 中元素数量超过 65535 时，头部无法存储确切的元素数量
         * 此时将元素计数设置为 LP_HDR_NUMELE_UNKNOWN，表示数量未知
         */
        if (num_elements != LP_HDR_NUMELE_UNKNOWN) {
            if (!delete)
                lpSetNumElements(lp,num_elements+1);
            else
                lpSetNumElements(lp,num_elements-1);
        }
    }
    lpSetTotalBytes(lp,new_listpack_bytes);

#if 0
    /* This code path is normally disabled: what it does is to force listpack
     * to return *always* a new pointer after performing some modification to
     * the listpack, even if the previous allocation was enough. This is useful
     * in order to spot bugs in code using listpacks: by doing so we can find
     * if the caller forgets to set the new pointer where the listpack reference
     * is stored, after an update. */
    unsigned char *oldlp = lp;
    lp = lp_malloc(new_listpack_bytes);
    memcpy(lp,oldlp,new_listpack_bytes);
    if (newp) {
        unsigned long offset = (*newp)-oldlp;
        *newp = lp + offset;
    }
    /* Make sure the old allocation contains garbage. */
    memset(oldlp,'A',new_listpack_bytes);
    lp_free(oldlp);
#endif

    return lp;
}

/* Insert the specified elements with 'entries' and 'len' at the specified
 * position 'p', with 'p' being a listpack element pointer obtained with
 * lpFirst(), lpLast(), lpNext(), lpPrev() or lpSeek().
 *
 * This is similar to lpInsert() but allows you to insert batch of entries in
 * one call. This function is more efficient than inserting entries one by one
 * as it does single realloc()/memmove() calls for all the entries.
 *
 * In each listpackEntry, if 'sval' is  not null, it is assumed entry is string
 * and 'sval' and 'slen' will be used. Otherwise, 'lval' will be used to append
 * the integer entry.
 *
 * The elements are inserted before or after the element pointed by 'p'
 * depending on the 'where' argument, that can be LP_BEFORE or LP_AFTER.
 *
 * If 'newp' is not NULL, at the end of a successful call '*newp' will be set
 * to the address of the element just added, so that it will be possible to
 * continue an interaction with lpNext() and lpPrev().
 *
 * Returns NULL on out of memory or when the listpack total length would exceed
 * the max allowed size of 2^32-1, otherwise the new pointer to the listpack
 * holding the new element is returned (and the old pointer passed is no longer
 * considered valid). */
unsigned char *lpBatchInsert(unsigned char *lp, unsigned char *p, int where,
                             listpackEntry *entries, unsigned int len,
                             unsigned char **newp)
{
    assert(where == LP_BEFORE || where == LP_AFTER);
    assert(entries != NULL && len > 0);

    struct listpackInsertEntry {
        int enctype;
        uint64_t enclen;
        unsigned char intenc[LP_MAX_INT_ENCODING_LEN];
        unsigned char backlen[LP_MAX_BACKLEN_SIZE];
        unsigned long backlen_size;
    };

    uint64_t addedlen = 0;       /* The encoded length of the added elements. */
    struct listpackInsertEntry tmp[3];  /* Encoded entries */
    struct listpackInsertEntry *enc = tmp;

    if (len > sizeof(tmp) / sizeof(struct listpackInsertEntry)) {
        /* If 'len' is larger than local buffer size, allocate on heap. */
        enc = zmalloc(len * sizeof(struct listpackInsertEntry));
    }

    /* If we need to insert after the current element, we just jump to the
     * next element (that could be the EOF one) and handle the case of
     * inserting before. So the function will actually deal with just one
     * case: LP_BEFORE. */
    if (where == LP_AFTER) {
        p = lpSkip(p);
        where = LP_BEFORE;
        ASSERT_INTEGRITY(lp, p);
    }

    for (unsigned int i = 0; i < len; i++) {
        listpackEntry *e = &entries[i];
        if (e->sval) {
           /* Calling lpEncodeGetType() results into the encoded version of the
            * element to be stored into 'intenc' in case it is representable as
            * an integer: in that case, the function returns LP_ENCODING_INT.
            * Otherwise, if LP_ENCODING_STR is returned, we'll have to call
            * lpEncodeString() to actually write the encoded string on place
            * later.
            *
            * Whatever the returned encoding is, 'enclen' is populated with the
            * length of the encoded element. */
            enc[i].enctype = lpEncodeGetType(e->sval, e->slen,
                                             enc[i].intenc, &enc[i].enclen);
        } else {
            enc[i].enctype = LP_ENCODING_INT;
            lpEncodeIntegerGetType(e->lval, enc[i].intenc, &enc[i].enclen);
        }
        addedlen += enc[i].enclen;

        /* We need to also encode the backward-parsable length of the element
         * and append it to the end: this allows to traverse the listpack from
         * the end to the start. */
        enc[i].backlen_size = lpEncodeBacklen(enc[i].backlen, enc[i].enclen);
        addedlen += enc[i].backlen_size;
    }

    uint64_t old_listpack_bytes = lpGetTotalBytes(lp);
    uint64_t new_listpack_bytes = old_listpack_bytes + addedlen;
    if (new_listpack_bytes > UINT32_MAX) return NULL;

    /* Store the offset of the element 'p', so that we can obtain its
     * address again after a reallocation. */
    unsigned long poff = p-lp;
    unsigned char *dst = lp + poff; /* May be updated after reallocation. */

    /* Realloc before: we need more room. */
    if (new_listpack_bytes > old_listpack_bytes &&
        new_listpack_bytes > lp_malloc_size(lp)) {
        if ((lp = lp_realloc(lp,new_listpack_bytes)) == NULL) return NULL;
        dst = lp + poff;
    }

    /* Setup the listpack relocating the elements to make the exact room
     * we need to store the new ones. */
    memmove(dst+addedlen,dst,old_listpack_bytes-poff);

    for (unsigned int i = 0; i < len; i++) {
        listpackEntry *ent = &entries[i];

        if (newp)
            *newp = dst;

        if (enc[i].enctype == LP_ENCODING_INT)
            memcpy(dst, enc[i].intenc, enc[i].enclen);
        else
            lpEncodeString(dst, ent->sval, ent->slen);

        dst += enc[i].enclen;
        memcpy(dst, enc[i].backlen, enc[i].backlen_size);
        dst += enc[i].backlen_size;
    }

    /* Update header. */
    uint32_t num_elements = lpGetNumElements(lp);
    if (num_elements != LP_HDR_NUMELE_UNKNOWN) {
        if ((int64_t) len > (int64_t) LP_HDR_NUMELE_UNKNOWN - (int64_t) num_elements)
            lpSetNumElements(lp, LP_HDR_NUMELE_UNKNOWN);
        else
            lpSetNumElements(lp,num_elements + len);
    }
    lpSetTotalBytes(lp,new_listpack_bytes);
    if (enc != tmp) lp_free(enc);

    return lp;
}

/* This is just a wrapper for lpInsert() to directly use a string. */
unsigned char *lpInsertString(unsigned char *lp, unsigned char *s, uint32_t slen,
                              unsigned char *p, int where, unsigned char **newp)
{
    return lpInsert(lp, s, NULL, slen, p, where, newp);
}

/* This is just a wrapper for lpInsert() to directly use a 64 bit integer
 * instead of a string. */
unsigned char *lpInsertInteger(unsigned char *lp, long long lval, unsigned char *p, int where, unsigned char **newp) {
    uint64_t enclen; /* The length of the encoded element. */
    unsigned char intenc[LP_MAX_INT_ENCODING_LEN];

    lpEncodeIntegerGetType(lval, intenc, &enclen);
    return lpInsert(lp, NULL, intenc, enclen, p, where, newp);
}

/* Append the specified element 's' of length 'slen' at the head of the listpack. */
unsigned char *lpPrepend(unsigned char *lp, unsigned char *s, uint32_t slen) {
    unsigned char *p = lpFirst(lp);
    if (!p) return lpAppend(lp, s, slen);
    return lpInsert(lp, s, NULL, slen, p, LP_BEFORE, NULL);
}

/* Append the specified integer element 'lval' at the head of the listpack. */
unsigned char *lpPrependInteger(unsigned char *lp, long long lval) {
    unsigned char *p = lpFirst(lp);
    if (!p) return lpAppendInteger(lp, lval);
    return lpInsertInteger(lp, lval, p, LP_BEFORE, NULL);
}

/* Append the specified element 'ele' of length 'size' at the end of the
 * listpack. It is implemented in terms of lpInsert(), so the return value is
 * the same as lpInsert(). */
unsigned char *lpAppend(unsigned char *lp, unsigned char *ele, uint32_t size) {
    // 获取 listpack 的总字节数
    uint64_t listpack_bytes = lpGetTotalBytes(lp);
    // 结束位置的指针
    unsigned char *eofptr = lp + listpack_bytes - 1;
    return lpInsert(lp,ele,NULL,size,eofptr,LP_BEFORE,NULL);
}

/* Append the specified integer element 'lval' at the end of the listpack. */
unsigned char *lpAppendInteger(unsigned char *lp, long long lval) {
    uint64_t listpack_bytes = lpGetTotalBytes(lp);
    unsigned char *eofptr = lp + listpack_bytes - 1;
    return lpInsertInteger(lp, lval, eofptr, LP_BEFORE, NULL);
}

/* Append batch of entries to the listpack.
 *
 * This call is more efficient than multiple lpAppend() calls as it only does
 * a single realloc() for all the given entries.
 *
 * In each listpackEntry, if 'sval' is  not null, it is assumed entry is string
 * and 'sval' and 'slen' will be used. Otherwise, 'lval' will be used to append
 * the integer entry. */
unsigned char *lpBatchAppend(unsigned char *lp, listpackEntry *entries, unsigned long len) {
    uint64_t listpack_bytes = lpGetTotalBytes(lp);
    unsigned char *eofptr = lp + listpack_bytes - 1;
    return lpBatchInsert(lp, eofptr, LP_BEFORE, entries, len, NULL);
}

/* This is just a wrapper for lpInsert() to directly use a string to replace
 * the current element. The function returns the new listpack as return
 * value, and also updates the current cursor by updating '*p'. */
unsigned char *lpReplace(unsigned char *lp, unsigned char **p, unsigned char *s, uint32_t slen) {
    return lpInsert(lp, s, NULL, slen, *p, LP_REPLACE, p);
}

/* This is just a wrapper for lpInsertInteger() to directly use a 64 bit integer
 * instead of a string to replace the current element. The function returns
 * the new listpack as return value, and also updates the current cursor
 * by updating '*p'. */
unsigned char *lpReplaceInteger(unsigned char *lp, unsigned char **p, long long lval) {
    return lpInsertInteger(lp, lval, *p, LP_REPLACE, p);
}

/* Remove the element pointed by 'p', and return the resulting listpack.
 * If 'newp' is not NULL, the next element pointer (to the right of the
 * deleted one) is returned by reference. If the deleted element was the
 * last one, '*newp' is set to NULL. */
unsigned char *lpDelete(unsigned char *lp, unsigned char *p, unsigned char **newp) {
    return lpInsert(lp,NULL,NULL,0,p,LP_REPLACE,newp);
}

/* Delete a range of entries from the listpack start with the element pointed by 'p'. */
unsigned char *lpDeleteRangeWithEntry(unsigned char *lp, unsigned char **p, unsigned long num) {
    size_t bytes = lpBytes(lp);
    unsigned long deleted = 0;
    unsigned char *eofptr = lp + bytes - 1;
    unsigned char *first, *tail;
    first = tail = *p;

    if (num == 0) return lp;  /* Nothing to delete, return ASAP. */

    /* Find the next entry to the last entry that needs to be deleted.
     * lpLength may be unreliable due to corrupt data, so we cannot
     * treat 'num' as the number of elements to be deleted.
     * 根据 num 数量移动尾指针
     */
    while (num--) {
        deleted++;
        tail = lpSkip(tail);
        if (tail[0] == LP_EOF) break;
        lpAssertValidEntry(lp, bytes, tail);
    }

    /* Store the offset of the element 'first', so that we can obtain its
     * address again after a reallocation. */
    unsigned long poff = first-lp;

    /* Move tail to the front of the listpack
     * first 到 tail 之间的数据是要删除的
     */
    memmove(first, tail, eofptr - tail + 1);
    /* 总字节数-删除字节数
     * 设置 total bytes 后, 范围外的数据不可访问
     * 但如果内存没有缩减,这部分数据不会改变
     * 缩减后会释放内存
     */
    lpSetTotalBytes(lp, bytes - (tail - first));
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN)
        lpSetNumElements(lp, numele-deleted);
    lp = lpShrinkToFit(lp);

    /* Store the entry. */
    *p = lp+poff;
    if ((*p)[0] == LP_EOF) *p = NULL;

    return lp;
}

/* Delete a range of entries from the listpack. */
unsigned char *lpDeleteRange(unsigned char *lp, long index, unsigned long num) {
    unsigned char *p;
    uint32_t numele = lpGetNumElements(lp);

    if (num == 0) return lp; /* Nothing to delete, return ASAP. */
    if ((p = lpSeek(lp, index)) == NULL) return lp;

    /* If we know we're gonna delete beyond the end of the listpack, we can just move
     * the EOF marker, and there's no need to iterate through the entries,
     * 当我们知道要删除的元素超出了 listpack 的末尾时，可以直接移动 EOF 标记来截断 listpack，而不需要逐个遍历和删除元素
     * but if we can't be sure how many entries there are, we rather avoid calling lpLength
     * since that means an additional iteration on all elements.
     * 当不确定 listpack 中的条目数量时，避免调用 lpLength 是为了减少不必要的全量遍历
     *
     * Note that index could overflow, but we use the value after seek, so when we
     * use it no overflow happens. */
    // 处理负索引
    if (numele != LP_HDR_NUMELE_UNKNOWN && index < 0) index = (long)numele + index;
    // 如果可以删除的 entry 数量 <= num, 则直接移动 EOF 标记
    if (numele != LP_HDR_NUMELE_UNKNOWN && (numele - (unsigned long)index) <= num) {
        p[0] = LP_EOF;
        lpSetTotalBytes(lp, p - lp + 1);
        lpSetNumElements(lp, index);
        lp = lpShrinkToFit(lp);
    } else {
        // 按需删除
        lp = lpDeleteRangeWithEntry(lp, &p, num);
    }

    return lp;
}

/* Delete the elements 'ps' passed as an array of 'count' element pointers and
 * return the resulting listpack. The elements must be given in the same order
 * as they apper in the listpack. */
unsigned char *lpBatchDelete(unsigned char *lp, unsigned char **ps, unsigned long count) {
    if (count == 0) return lp;
    unsigned char *dst = ps[0];
    size_t total_bytes = lpGetTotalBytes(lp);
    unsigned char *lp_end = lp + total_bytes; /* After the EOF element. */
    /*
     * lp_end[-1] 等价于 *(lp_end - 1)
     * 由于 lp_end 指向 EOF 标记之后的位置，所以 lp_end - 1 指向 EOF 标记本身
     */
    assert(lp_end[-1] == LP_EOF);
    /*
     * ----+--------+-----------+--------+---------+-----+---+
     * ... | Delete | Keep      | Delete | Keep    | ... |EOF|
     * ... |xxxxxxxx|           |xxxxxxxx|         | ... |   |
     * ----+--------+-----------+--------+---------+-----+---+
     *     ^        ^           ^                            ^
     *     |        |           |                            |
     *     ps[i]    |           ps[i+1]                      |
     *     skip     keep_start  keep_end                     lp_end
     *
     * The loop memmoves the bytes between keep_start and keep_end to dst.
     *
     * 假设有 listpack [0,1,2,3,4,5,6] 要删除 1,2,3,5
     * 第一轮
     *  i=0 dst=1,skip=1,keep_start=2,keep_end=2
     *  i=1 dst=1,skip=2,keep_start=3,keep_end=3
     *  i=2 dst=1,skip=3,keep_start=4,keep_end=5
     *  keep_start!=keep_end, bytes_to_keep=1
     *  memmove(dst, keep_start, bytes_to_keep);
     *  将 keep_start 之后的 bytes_to_keep(1) 数据移动到 dst
     *  得到 [0,4,2,3,4,5,6]  dst=2
     * 第二轮
     *  i=3 dst=2,skip=5,keep_start=6,keep_end=lpend
     *  得到 [0,4,6,3,4,5,6]  dst=3
     * 更新数据得到 [0,4,6]
     */
    for (unsigned long i = 0; i < count; i++) {
        unsigned char *skip = ps[i];
        assert(skip != NULL && skip[0] != LP_EOF);
        unsigned char *keep_start = lpSkip(skip);
        unsigned char *keep_end;
        if (i + 1 < count) {
            keep_end = ps[i + 1];

            /* Deleting consecutive elements. Nothing to keep between them. */
            if (keep_start == keep_end) continue;
        } else {
            /* Keep the rest of the listpack including the EOF marker. */
            keep_end = lp_end;
        }
        assert(keep_end > keep_start);
        size_t bytes_to_keep = keep_end - keep_start;
        memmove(dst, keep_start, bytes_to_keep);
        dst += bytes_to_keep;
    }
    /* Update total size and num elements. */
    /*
     * lpend 指向的是 EOL
     * dst 指向第1个无效的数据
     * 之间就是删除的字节数
     */
    size_t deleted_bytes = lp_end - dst;
    total_bytes -= deleted_bytes;
    assert(lp[total_bytes - 1] == LP_EOF);
    lpSetTotalBytes(lp, total_bytes);
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) lpSetNumElements(lp, numele - count);
    return lpShrinkToFit(lp);
}

/* Merge listpacks 'first' and 'second' by appending 'second' to 'first'.
 *
 * NOTE: The larger listpack is reallocated to contain the new merged listpack.
 * Either 'first' or 'second' can be used for the result.  The parameter not
 * used will be free'd and set to NULL.
 *
 * After calling this function, the input parameters are no longer valid since
 * they are changed and free'd in-place.
 *
 * The result listpack is the contents of 'first' followed by 'second'.
 *
 * On failure: returns NULL if the merge is impossible.
 * On success: returns the merged listpack (which is expanded version of either
 * 'first' or 'second', also frees the other unused input listpack, and sets the
 * input listpack argument equal to newly reallocated listpack return value. */
unsigned char *lpMerge(unsigned char **first, unsigned char **second) {
    /* If any params are null, we can't merge, so NULL. */
    if (first == NULL || *first == NULL || second == NULL || *second == NULL)
        return NULL;

    /* Can't merge same list into itself. */
    if (*first == *second)
        return NULL;

    size_t first_bytes = lpBytes(*first);
    unsigned long first_len = lpLength(*first);

    size_t second_bytes = lpBytes(*second);
    unsigned long second_len = lpLength(*second);

    int append;
    unsigned char *source, *target;
    size_t target_bytes, source_bytes;
    /* Pick the largest listpack so we can resize easily in-place.
     * We must also track if we are now appending or prepending to
     * the target listpack. */
    if (first_bytes >= second_bytes) {
        /* retain first, append second to first. */
        target = *first;
        target_bytes = first_bytes;
        source = *second;
        source_bytes = second_bytes;
        append = 1;
    } else {
        /* else, retain second, prepend first to second. */
        target = *second;
        target_bytes = second_bytes;
        source = *first;
        source_bytes = first_bytes;
        append = 0;
    }

    /* Calculate final bytes (subtract one pair of metadata) */
    unsigned long long lpbytes = (unsigned long long)first_bytes + second_bytes - LP_HDR_SIZE - 1;
    assert(lpbytes < UINT32_MAX); /* larger values can't be stored */
    unsigned long lplength = first_len + second_len;

    /* Combined lp length should be limited within UINT16_MAX */
    lplength = lplength < UINT16_MAX ? lplength : UINT16_MAX;

    /* Extend target to new lpbytes then append or prepend source. */
    target = lp_realloc(target, lpbytes);
    if (append) {
        /* append == appending to target */
        /* Copy source after target (copying over original [END]):
         *   [TARGET - END, SOURCE - HEADER] */
        memcpy(target + target_bytes - 1,
               source + LP_HDR_SIZE,
               source_bytes - LP_HDR_SIZE);
    } else {
        /* !append == prepending to target */
        /* Move target *contents* exactly size of (source - [END]),
         * then copy source into vacated space (source - [END]):
         *   [SOURCE - END, TARGET - HEADER] */
        memmove(target + source_bytes - 1,
                target + LP_HDR_SIZE,
                target_bytes - LP_HDR_SIZE);
        memcpy(target, source, source_bytes - 1);
    }

    lpSetNumElements(target, lplength);
    lpSetTotalBytes(target, lpbytes);

    /* Now free and NULL out what we didn't realloc */
    if (append) {
        lp_free(*second);
        *second = NULL;
        *first = target;
    } else {
        lp_free(*first);
        *first = NULL;
        *second = target;
    }

    return target;
}

// 复制 lp
unsigned char *lpDup(unsigned char *lp) {
    size_t lpbytes = lpBytes(lp);
    unsigned char *newlp = lp_malloc(lpbytes);
    memcpy(newlp, lp, lpbytes);
    return newlp;
}

/* Return the total number of bytes the listpack is composed of. */
size_t lpBytes(unsigned char *lp) {
    return lpGetTotalBytes(lp);
}

/* Returns the size 'lval' will require when encoded, in bytes */
size_t lpEntrySizeInteger(long long lval) {
    uint64_t enclen;
    lpEncodeIntegerGetType(lval, NULL, &enclen);
    unsigned long backlen = lpEncodeBacklenBytes(enclen);
    return enclen + backlen;
}

/* Returns the size of a listpack consisting of an integer repeated 'rep' times. */
size_t lpEstimateBytesRepeatedInteger(long long lval, unsigned long rep) {
    return LP_HDR_SIZE + lpEntrySizeInteger(lval) * rep + 1;
}

/* Seek the specified element and returns the pointer to the seeked element.
 * Positive indexes specify the zero-based element to seek from the head to
 * the tail, negative indexes specify elements starting from the tail, where
 * -1 means the last element, -2 the penultimate and so forth. If the index
 * is out of range, NULL is returned. */
unsigned char *lpSeek(unsigned char *lp, long index) {
    int forward = 1; /* Seek forward by default. */

    /* We want to seek from left to right or the other way around
     * depending on the listpack length and the element position.
     * However if the listpack length cannot be obtained in constant time,
     * we always seek from left to right. */
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) {
        if (index < 0) index = (long)numele+index;
        if (index < 0) return NULL; /* Index still < 0 means out of range. */
        if (index >= (long)numele) return NULL; /* Out of range the other side. */
        /* We want to scan right-to-left if the element we are looking for
         * is past the half of the listpack. */
        if (index > (long)numele/2) {
            forward = 0;
            /* Right to left scanning always expects a negative index. Convert
             * our index to negative form. */
            index -= numele;
        }
    } else {
        /* If the listpack length is unspecified, for negative indexes we
         * want to always scan right-to-left. */
        if (index < 0) forward = 0;
    }

    /* Forward and backward scanning is trivially based on lpNext()/lpPrev(). */
    if (forward) {
        unsigned char *ele = lpFirst(lp);
        while (index > 0 && ele) {
            ele = lpNext(lp,ele);
            index--;
        }
        return ele;
    } else {
        unsigned char *ele = lpLast(lp);
        while (index < -1 && ele) {
            ele = lpPrev(lp,ele);
            index++;
        }
        return ele;
    }
}

/* Same as lpFirst but without validation assert, to be used right before lpValidateNext. */
unsigned char *lpValidateFirst(unsigned char *lp) {
    unsigned char *p = lp + LP_HDR_SIZE; /* Skip the header. */
    if (p[0] == LP_EOF) return NULL;
    return p;
}

/* Validate the integrity of a single listpack entry and move to the next one.
 * The input argument 'pp' is a reference to the current record and is advanced on exit.
 *  the data pointed to by 'lp' will not be modified by the function.
 *  用于验证 listpack 中当前元素的完整性，并将指针移动到下一个元素
 *
 *  输入参数 'pp' 是对当前记录的引用，在退出时会指向（下一个条目的）起始位置
 *  'lp' 指向数据不会改变
 * Returns 1 if valid, 0 if invalid. */
int lpValidateNext(unsigned char *lp, unsigned char **pp, size_t lpbytes) {
    // lpbytes 总是 lp 的总字节大小
    // 为什么不用  ASSERT_INTEGRITY ?
#define OUT_OF_RANGE(p) ( \
        (p) < lp + LP_HDR_SIZE || \
        (p) > lp + lpbytes - 1)
    unsigned char *p = *pp;
    if (!p)
        return 0;

    /* Before accessing p, make sure it's valid. */
    if (OUT_OF_RANGE(p))
        return 0;

    if (*p == LP_EOF) {
        *pp = NULL;
        return 1;
    }

    /* check that we can read the encoded size
     * 所存储的元素的第一个字节可判断数据类型
     * 获取长度编码部分所需的字节数
     */
    uint32_t lenbytes = lpCurrentEncodedSizeBytes(p[0]);
    if (!lenbytes)
        return 0;

    /* make sure the encoded entry length doesn't reach outside the edge of the listpack */
    if (OUT_OF_RANGE(p + lenbytes))
        return 0;

    /* get the entry length and encoded backlen. */
    // 获取数据和其正向编码长度
    unsigned long entrylen = lpCurrentEncodedSizeUnsafe(p);
    // 获取反向编码长度
    unsigned long encodedBacklen = lpEncodeBacklenBytes(entrylen);
    entrylen += encodedBacklen;

    /* make sure the entry doesn't reach outside the edge of the listpack */
    if (OUT_OF_RANGE(p + entrylen))
        return 0;

    /* move to the next entry */
    p += entrylen;

    /* make sure the encoded length at the end patches the one at the beginning. */
    /* 用于确保条目末尾的反向长度与条目开头的正向长度一致
     * p-1 指向的是上一个 entry 的反向编码的最后一个字节,可以获取该 entry 的长度
     *
     * prevlen:  前一个 entry 的反向编码长度获取 entry 的真正长度
     * encodedBacklen: 前一个 entry 反向编码长度
     * 两个加起来是整个 entry 长度
     * entry 结构
     * encoding-type|entry-data|entry-len
     * prevlen = len(encoding-type|entry-data)
     * encodedBacklen = len(prevlen)
     */
    uint64_t prevlen = lpDecodeBacklen(p-1);
    if (prevlen + encodedBacklen != entrylen)
        return 0;

    *pp = p;
    return 1;
#undef OUT_OF_RANGE
}

/* Validate that the entry doesn't reach outside the listpack allocation. */
static inline void lpAssertValidEntry(unsigned char* lp, size_t lpbytes, unsigned char *p) {
    assert(lpValidateNext(lp, &p, lpbytes));
}

/* Validate the integrity of the data structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we scan all the entries one by one. */
int lpValidateIntegrity(unsigned char *lp, size_t size, int deep, 
                        listpackValidateEntryCB entry_cb, void *cb_userdata) {
    /* Check that we can actually read the header. (and EOF) */
    if (size < LP_HDR_SIZE + 1)
        return 0;

    /* Check that the encoded size in the header must match the allocated size. */
    size_t bytes = lpGetTotalBytes(lp);
    if (bytes != size)
        return 0;

    /* The last byte must be the terminator. */
    if (lp[size-1] != LP_EOF)
        return 0;

    if (!deep)
        return 1;

    /* Validate the individual entries. */
    uint32_t count = 0;
    uint32_t numele = lpGetNumElements(lp);
    unsigned char *p = lp + LP_HDR_SIZE;
    while(p && p[0] != LP_EOF) {
        unsigned char *prev = p;

        /* Validate this entry and move to the next entry in advance
         * to avoid callback crash due to corrupt listpack. */
        if (!lpValidateNext(lp, &p, bytes))
            return 0;

        /* Optionally let the caller validate the entry too. */
        if (entry_cb && !entry_cb(prev, numele, cb_userdata))
            return 0;

        count++;
    }

    /* Make sure 'p' really does point to the end of the listpack. */
    if (p != lp + size - 1)
        return 0;

    /* Check that the count in the header is correct */
    if (numele != LP_HDR_NUMELE_UNKNOWN && numele != count)
        return 0;

    return 1;
}

/* Compare entry pointer to by 'p' with string 's' of length 'slen'.
 * Return 1 if equal. */
unsigned int lpCompare(unsigned char *p, unsigned char *s, uint32_t slen) {
    unsigned char *value;
    int64_t sz;
    if (p[0] == LP_EOF) return 0;

    value = lpGet(p, &sz, NULL);
    if (value) {
        return (slen == sz) && memcmp(value,s,slen) == 0;
    } else {
        /* We use lpStringToInt64() to get an integer representation of the
         * string 's' and compare it to 'sval', it's much faster than convert
         * integer to string and comparing. */
        int64_t sval;
        if (lpStringToInt64((const char*)s, slen, &sval))
            return sz == sval;
    }

    return 0;
}

/* uint compare for qsort */
static int uintCompare(const void *a, const void *b) {
    return (*(unsigned int *) a - *(unsigned int *) b);
}

/* Helper method to store a string into from val or lval into dest */
static inline void lpSaveValue(unsigned char *val, unsigned int len, int64_t lval, listpackEntry *dest) {
    dest->sval = val;
    dest->slen = len;
    dest->lval = lval;
}

/* Randomly select a pair of key and value.
 * total_count is a pre-computed length/2 of the listpack (to avoid calls to lpLength)
 * 'key' and 'val' are used to store the result key value pair.
 * 'val' can be NULL if the value is not needed.
 * 'tuple_len' indicates entry count of a single logical item. It should be 2
 * if listpack was saved as key-value pair or more for key-value-...(n_entries). */
void lpRandomPair(unsigned char *lp, unsigned long total_count,
                  listpackEntry *key, listpackEntry *val, int tuple_len)
{
    unsigned char *p;

    assert(tuple_len >= 2);

    /* Avoid div by zero on corrupt listpack */
    assert(total_count);

    int r = (rand() % total_count) * tuple_len;
    assert((p = lpSeek(lp, r)));
    key->sval = lpGetValue(p, &(key->slen), &(key->lval));

    if (!val)
        return;
    assert((p = lpNext(lp, p)));
    val->sval = lpGetValue(p, &(val->slen), &(val->lval));
}

/* Randomly select 'count' entries and store them in the 'entries' array, which
 * needs to have space for 'count' listpackEntry structs. The order is random
 * and duplicates are possible. */
void lpRandomEntries(unsigned char *lp, unsigned int count, listpackEntry *entries) {
    struct pick {
        unsigned int index;
        unsigned int order;
    } *picks = lp_malloc(count * sizeof(struct pick));
    unsigned int total_size = lpLength(lp);
    assert(total_size);
    for (unsigned int i = 0; i < count; i++) {
        picks[i].index = rand() % total_size;
        picks[i].order = i;
    }

    /* Sort by index. */
    qsort(picks, count, sizeof(struct pick), uintCompare);

    /* Iterate over listpack in index order and store the values in the entries
     * array respecting the original order. */
    unsigned char *p = lpFirst(lp);
    unsigned int j = 0; /* index in listpack */
    for (unsigned int i = 0; i < count; i++) {
        /* Advance listpack pointer to until we reach 'index' listpack.
         * 如果 index 有重复,即连续相同的 index, 则会跳过这个for 循环
         */
        while (j < picks[i].index) {
            p = lpNext(lp, p);
            j++;
        }
        int storeorder = picks[i].order;
        unsigned int len = 0;
        long long llval = 0;
        unsigned char *str = lpGetValue(p, &len, &llval);
        lpSaveValue(str, len, llval, &entries[storeorder]);
    }
    lp_free(picks);
}

/* Randomly select count of key value pairs and store into 'keys' and
 * 'vals' args. The order of the picked entries is random, and the selections
 * are non-unique (repetitions are possible).
 * The 'vals' arg can be NULL in which case we skip these.
 * 'tuple_len' indicates entry count of a single logical item. It should be 2
 * if listpack was saved as key-value pair or more for key-value-...(n_entries). */
void lpRandomPairs(unsigned char *lp, unsigned int count, listpackEntry *keys, listpackEntry *vals, int tuple_len) {
    unsigned char *p, *key, *value;
    unsigned int klen = 0, vlen = 0;
    long long klval = 0, vlval = 0;

    assert(tuple_len >= 2);

    /* Notice: the index member must be first due to the use in uintCompare */
    typedef struct {
        unsigned int index;
        unsigned int order;
    } rand_pick;
    rand_pick *picks = lp_malloc(sizeof(rand_pick)*count);
    unsigned int total_size = lpLength(lp)/tuple_len;

    /* Avoid div by zero on corrupt listpack */
    assert(total_size);

    /* create a pool of random indexes (some may be duplicate). */
    for (unsigned int i = 0; i < count; i++) {
        /* Generate indexes that key exist at */
        picks[i].index = (rand() % total_size) * tuple_len;
        /* keep track of the order we picked them */
        picks[i].order = i;
    }

    /* sort by indexes. */
    qsort(picks, count, sizeof(rand_pick), uintCompare);

    /* fetch the elements form the listpack into a output array respecting the original order. */
    unsigned int lpindex = picks[0].index, pickindex = 0;
    p = lpSeek(lp, lpindex);
    while (p && pickindex < count) {
        key = lpGetValue(p, &klen, &klval);
        assert((p = lpNext(lp, p)));
        value = lpGetValue(p, &vlen, &vlval);
        // pickindex 就是 picks 中的索引
        // 这个 while 循环就是处理 index 重复的情况,不用重新取 key 和 value,而是复用第一次取得的
        while (pickindex < count && lpindex == picks[pickindex].index) {
            int storeorder = picks[pickindex].order;
            lpSaveValue(key, klen, klval, &keys[storeorder]);
            if (vals)
                lpSaveValue(value, vlen, vlval, &vals[storeorder]);
             pickindex++;
        }
        lpindex += tuple_len;

        for (int i = 0; i < tuple_len - 1; i++) {
            p = lpNext(lp, p);
        }
    }

    lp_free(picks);
}

/* Randomly select count of key value pairs and store into 'keys' and
 * 'vals' args. The selections are unique (no repetitions), and the order of
 * the picked entries is NOT-random.
 * The 'vals' arg can be NULL in which case we skip these.
 * 'tuple_len' indicates entry count of a single logical item. It should be 2
 * if listpack was saved as key-value pair or more for key-value-...(n_entries).
 * The return value is the number of items picked which can be lower than the
 * requested count if the listpack doesn't hold enough pairs. */
unsigned int lpRandomPairsUnique(unsigned char *lp, unsigned int count,
                                 listpackEntry *keys, listpackEntry *vals,
                                 int tuple_len)
{
    assert(tuple_len >= 2);

    unsigned char *p, *key;
    unsigned int klen = 0;
    long long klval = 0;
    unsigned int total_size = lpLength(lp)/tuple_len;
    unsigned int index = 0;
    if (count > total_size)
        count = total_size;

    p = lpFirst(lp);
    unsigned int picked = 0, remaining = count;
    // remaining 剩余要取的元素数
    while (picked < count && p) {
        // 调用 lpNextRandom 后 index 的值会改变
        // 且这里是以 tuple_len 为单位抽样
        assert((p = lpNextRandom(lp, p, &index, remaining, tuple_len)));
        key = lpGetValue(p, &klen, &klval);
        lpSaveValue(key, klen, klval, &keys[picked]);
        assert((p = lpNext(lp, p)));
        index++;
        if (vals) {
            key = lpGetValue(p, &klen, &klval);
            lpSaveValue(key, klen, klval, &vals[picked]);
        }
        // lpNextRandom 是以 tuple_len 为单位抽样,所以这里不必跳过 tuple_len 个元素
        p = lpNext(lp, p);
        remaining--;
        picked++;
        index++;
    }
    return picked;
}

/* Iterates forward to the "next random" element, given we are yet to pick
 * 'remaining' unique elements between the starting element 'p' (inclusive) and
 * the end of the list. The 'index' needs to be initialized according to the
 * current zero-based index matching the position of the starting element 'p'
 * and is updated to match the returned element's zero-based index. If
 * 'tuple_len' indicates entry count of a single logical item. e.g. This is
 * useful if listpack represents key-value pairs. In this case, tuple_len should
 * be two and even indexes will be picked.
 *
 * Note that this function can return p. In order to skip the previously
 * returned element, you need to call lpNext() or lpDelete() after each call to
 * lpNextRandom(). Idea:
 *
 *     assert(remaining <= lpLength(lp));
 *     p = lpFirst(lp);
 *     i = 0;
 *     while (remaining > 0) {
 *         p = lpNextRandom(lp, p, &i, remaining--, 1);
 *
 *         // ... Do stuff with p ...
 *
 *         p = lpNext(lp, p);
 *         i++;
 *     }
 *  从当前元素 p（包含）开始，向前迭代 listpack，从剩余未遍历的元素中随机挑选一个符合条件的元素（基于 tuple_len），
 *  确保每个元素被选中的概率均等，且只需一次遍历
 */
unsigned char *lpNextRandom(unsigned char *lp, unsigned char *p, unsigned int *index,
                            unsigned int remaining, int tuple_len)
{
    assert(tuple_len > 0);
    /* To only iterate once, every time we try to pick a member, the probability
     * we pick it is the quotient of the count left we want to pick and the
     * count still we haven't visited. This way, we could make every member be
     * equally likely to be picked. */
    unsigned int i = *index;
    unsigned int total_size = lpLength(lp);
    while (i < total_size && p != NULL) {
        // 通过 i % tuple_len == 0 筛选出属于 “逻辑条目起始位置” 的元素
        if (i % tuple_len != 0) {
            p = lpNext(lp, p);
            i++;
            continue;
        }

        /* Do we pick this element? */
        // 剩余未遍历的符合条件的元素总数
        unsigned int available = (total_size - i) / tuple_len;
        double randomDouble = ((double)rand()) / RAND_MAX;
        // 选择概率 = 剩余需要选择的数量 / 剩余可选数量
        double threshold = ((double)remaining) / available;
        if (randomDouble <= threshold) {
            *index = i;
            return p;
        }

        // 继续下一次抽样
        p = lpNext(lp, p);
        i++;
    }

    return NULL;
}

/* Print info of listpack which is used in debugCommand */
void lpRepr(unsigned char *lp) {
    unsigned char *p, *vstr;
    int64_t vlen;
    unsigned char intbuf[LP_INTBUF_SIZE];
    int index = 0;

    printf("{total bytes %zu} {num entries %lu}\n", lpBytes(lp), lpLength(lp));
        
    p = lpFirst(lp);
    while(p) {
        uint32_t encoded_size_bytes = lpCurrentEncodedSizeBytes(p[0]);
        uint32_t encoded_size = lpCurrentEncodedSizeUnsafe(p);
        unsigned long back_len = lpEncodeBacklenBytes(encoded_size);
        printf(
            "{\n"
                "\taddr: 0x%08lx,\n"
                "\tindex: %2d,\n"
                "\toffset: %1lu,\n"
                "\thdr+entrylen+backlen: %2lu,\n"
                "\thdrlen: %3u,\n"
                "\tbacklen: %2lu,\n"
                "\tpayload: %1u\n",
            (long unsigned)p,
            index,
            (unsigned long) (p-lp),
            encoded_size + back_len,
            encoded_size_bytes,
            back_len,
            encoded_size - encoded_size_bytes);
        printf("\tbytes: ");
        for (unsigned int i = 0; i < (encoded_size + back_len); i++) {
            printf("%02x|",p[i]);
        }
        printf("\n");

        vstr = lpGet(p, &vlen, intbuf);
        printf("\t[str]");
        if (vlen > 40) {
            if (fwrite(vstr, 40, 1, stdout) == 0) perror("fwrite");
            printf("...");
        } else {
            if (fwrite(vstr, vlen, 1, stdout) == 0) perror("fwrite");
        }
        printf("\n}\n");
        index++;
        p = lpNext(lp, p);
    }
    printf("{end}\n\n");
}

#ifdef REDIS_TEST

#include <sys/time.h>
#include "adlist.h"
#include "sds.h"
#include "testhelp.h"

#define UNUSED(x) (void)(x)
#define TEST(name) printf("test — %s\n", name);

char *mixlist[] = {"hello", "foo", "quux", "1024"};
char *intlist[] = {"4294967296", "-100", "100", "128000", 
                   "non integer", "much much longer non integer"};

static unsigned char *createList(void) {
    unsigned char *lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char*)mixlist[1], strlen(mixlist[1]));
    lp = lpAppend(lp, (unsigned char*)mixlist[2], strlen(mixlist[2]));
    lp = lpPrepend(lp, (unsigned char*)mixlist[0], strlen(mixlist[0]));
    lp = lpAppend(lp, (unsigned char*)mixlist[3], strlen(mixlist[3]));
    return lp;
}

static unsigned char *createIntList(void) {
    unsigned char *lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char*)intlist[2], strlen(intlist[2]));
    lp = lpAppend(lp, (unsigned char*)intlist[3], strlen(intlist[3]));
    lp = lpPrepend(lp, (unsigned char*)intlist[1], strlen(intlist[1]));
    lp = lpPrepend(lp, (unsigned char*)intlist[0], strlen(intlist[0]));
    lp = lpAppend(lp, (unsigned char*)intlist[4], strlen(intlist[4]));
    lp = lpAppend(lp, (unsigned char*)intlist[5], strlen(intlist[5]));
    return lp;
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}


/*
 * pos =0 ,添加到头部, =1 添加到尾部
 * maxsize: 测试的最大 listpack 大小
 * dnum: 每次测试递增的大小步长
 * 渐进式测试：从不同大小开始测试，观察性能随大小的变化
 */
static void stress(int pos, int num, int maxsize, int dnum) {
    int i, j, k;
    unsigned char *lp;
    char posstr[2][5] = { "HEAD", "TAIL" };
    long long start;
    for (i = 0; i < maxsize; i+=dnum) {
        lp = lpNew(0);
        for (j = 0; j < i; j++) {
            lp = lpAppend(lp, (unsigned char*)"quux", 4);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        // 执行 num 次 push+pop 操作：
        for (k = 0; k < num; k++) {
            if (pos == 0) {
                lp = lpPrepend(lp, (unsigned char*)"quux", 4);
            } else {
                lp = lpAppend(lp, (unsigned char*)"quux", 4);

            }
            lp = lpDelete(lp, lpFirst(lp), NULL);
        }
        printf("List size: %8d, bytes: %8zu, %dx push+pop (%s): %6lld usec\n",
               i, lpBytes(lp), num, posstr[pos], usec()-start);
        lpFree(lp);
    }
}

static unsigned char *pop(unsigned char *lp, int where) {
    unsigned char *p, *vstr;
    int64_t vlen;

    p = lpSeek(lp, where == 0 ? 0 : -1);
    vstr = lpGet(p, &vlen, NULL);
    if (where == 0)
        printf("Pop head: ");
    else
        printf("Pop tail: ");

    if (vstr) {
        if (vlen && fwrite(vstr, vlen, 1, stdout) == 0) perror("fwrite");
    } else {
        printf("%lld", (long long)vlen);
    }

    printf("\n");
    return lpDelete(lp, p, &p);
}

static int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min+rand()%(max-min+1);
    int minval, maxval;
    switch(rand() % 3) {
    case 0:
        // 所有可能的字节值，包括不可打印字符
        minval = 0;
        maxval = 255;
    break;
        // 包含数字、大写字母、小写字母及部分符号，如 0-9、A-Z、a-z
    case 1:
        minval = 48;
        maxval = 122;
    break;
    case 2:
        // 仅包含数字 0-4
        minval = 48;
        maxval = 52;
    break;
    default:
        assert(NULL);
    }

    while(p < len)
        target[p++] = minval+rand()%(maxval-minval+1);
    return len;
}

static void verifyEntry(unsigned char *p, unsigned char *s, size_t slen) {
    assert(lpCompare(p, s, slen));
}

static int lpValidation(unsigned char *p, unsigned int head_count, void *userdata) {
    UNUSED(p);
    UNUSED(head_count);

    int ret;
    long *count = userdata;
    ret = lpCompare(p, (unsigned char *)mixlist[*count], strlen(mixlist[*count]));
    (*count)++;
    return ret;
}

static int lpFindCbCmp(const unsigned char *lp, unsigned char *p, void *user, unsigned char *s, long long slen) {
    assert(lp);
    assert(p);

    // user: 用户提供的数据（在这里是待查找的字符串）
    char *n = user;

    if (!s) {
        // 当 s 为 NULL 时，表示当前 listpack 元素是整数编码：
        int64_t sval;
        // 尝试将待查找的字符串 n 转换为整数 sval
        if (lpStringToInt64((const char*)n, strlen(n), &sval))
            return slen == sval ? 0 : 1;
    } else {
        if (strlen(n) == (size_t) slen && memcmp(n, s, slen) == 0)
            return 0;
    }

    return 1;
}

int listpackTest(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);

    int i;
    unsigned char *lp, *p, *vstr;
    int64_t vlen;
    unsigned char intbuf[LP_INTBUF_SIZE];
    int accurate = (flags & REDIS_TEST_ACCURATE);

    TEST("Create int list") {
        lp = createIntList();
        assert(lpLength(lp) == 6);
        lpFree(lp);
    }

    TEST("Create list") {
        lp = createList();
        assert(lpLength(lp) == 4);
        lpFree(lp);
    }

    TEST("Test lpPrepend") {
        lp = lpNew(0);
        lp = lpPrepend(lp, (unsigned char*)"abc", 3);
        lp = lpPrepend(lp, (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp, 0), (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp, 1), (unsigned char*)"abc", 3);
        lpFree(lp);
    }

    TEST("Test lpPrependInteger") {
        lp = lpNew(0);
        lp = lpPrependInteger(lp, 127);
        lp = lpPrependInteger(lp, 4095);
        lp = lpPrependInteger(lp, 32767);
        lp = lpPrependInteger(lp, 8388607);
        lp = lpPrependInteger(lp, 2147483647);
        lp = lpPrependInteger(lp, 9223372036854775807);
        verifyEntry(lpSeek(lp, 0), (unsigned char*)"9223372036854775807", 19);
        verifyEntry(lpSeek(lp, -1), (unsigned char*)"127", 3);
        lpFree(lp);
    }

    TEST("Get element at index") {
        lp = createList();
        verifyEntry(lpSeek(lp, 0), (unsigned char*)"hello", 5);
        verifyEntry(lpSeek(lp, 3), (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp, -1), (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp, -4), (unsigned char*)"hello", 5);
        assert(lpSeek(lp, 4) == NULL);
        assert(lpSeek(lp, -5) == NULL);
        lpFree(lp);
    }
    
    TEST("Pop list") {
        lp = createList();
        lp = pop(lp, 1);
        lp = pop(lp, 0);
        lp = pop(lp, 1);
        lp = pop(lp, 1);
        lpFree(lp);
    }

    TEST("Get element at index") {
        lp = createList();
        verifyEntry(lpSeek(lp, 0), (unsigned char*)"hello", 5);
        verifyEntry(lpSeek(lp, 3), (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp, -1), (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp, -4), (unsigned char*)"hello", 5);
        assert(lpSeek(lp, 4) == NULL);
        assert(lpSeek(lp, -5) == NULL);
        lpFree(lp);
    }

    TEST("Iterate list from 0 to end") {
        lp = createList();
        p = lpFirst(lp);
        i = 0;
        while (p) {
            verifyEntry(p, (unsigned char*)mixlist[i], strlen(mixlist[i]));
            p = lpNext(lp, p);
            i++;
        }
        lpFree(lp);
    }
    
    TEST("Iterate list from 1 to end") {
        lp = createList();
        i = 1;
        p = lpSeek(lp, i);
        while (p) {
            verifyEntry(p, (unsigned char*)mixlist[i], strlen(mixlist[i]));
            p = lpNext(lp, p);
            i++;
        }
        lpFree(lp);
    }
    
    TEST("Iterate list from 2 to end") {
        lp = createList();
        i = 2;
        p = lpSeek(lp, i);
        while (p) {
            verifyEntry(p, (unsigned char*)mixlist[i], strlen(mixlist[i]));
            p = lpNext(lp, p);
            i++;
        }
        lpFree(lp);
    }
    
    TEST("Iterate from back to front") {
        lp = createList();
        p = lpLast(lp);
        i = 3;
        while (p) {
            verifyEntry(p, (unsigned char*)mixlist[i], strlen(mixlist[i]));
            p = lpPrev(lp, p);
            i--;
        }
        lpFree(lp);
    }
    
    TEST("Iterate from back to front, deleting all items") {
        lp = createList();
        p = lpLast(lp);
        i = 3;
        while ((p = lpLast(lp))) {
            verifyEntry(p, (unsigned char*)mixlist[i], strlen(mixlist[i]));
            lp = lpDelete(lp, p, &p);
            assert(p == NULL);
            i--;
        }
        lpFree(lp);
    }

    TEST("Delete whole listpack when num == -1");
    {
        lp = createList();
        lp = lpDeleteRange(lp, 0, -1);
        assert(lpLength(lp) == 0);
        assert(lp[LP_HDR_SIZE] == LP_EOF);
        assert(lpBytes(lp) == (LP_HDR_SIZE + 1));
        zfree(lp);

        lp = createList();
        unsigned char *ptr = lpFirst(lp);
        lp = lpDeleteRangeWithEntry(lp, &ptr, -1);
        assert(lpLength(lp) == 0);
        assert(lp[LP_HDR_SIZE] == LP_EOF);
        assert(lpBytes(lp) == (LP_HDR_SIZE + 1));
        zfree(lp);
    }

    TEST("Delete whole listpack with negative index");
    {
        lp = createList();
        lp = lpDeleteRange(lp, -4, 4);
        assert(lpLength(lp) == 0);
        assert(lp[LP_HDR_SIZE] == LP_EOF);
        assert(lpBytes(lp) == (LP_HDR_SIZE + 1));
        zfree(lp);

        lp = createList();
        unsigned char *ptr = lpSeek(lp, -4);
        lp = lpDeleteRangeWithEntry(lp, &ptr, 4);
        assert(lpLength(lp) == 0);
        assert(lp[LP_HDR_SIZE] == LP_EOF);
        assert(lpBytes(lp) == (LP_HDR_SIZE + 1));
        zfree(lp);
    }

    TEST("Delete inclusive range 0,0");
    {
        lp = createList();
        lp = lpDeleteRange(lp, 0, 1);
        assert(lpLength(lp) == 3);
        assert(lpSkip(lpLast(lp))[0] == LP_EOF); /* check set LP_EOF correctly */
        zfree(lp);

        lp = createList();
        unsigned char *ptr = lpFirst(lp);
        lp = lpDeleteRangeWithEntry(lp, &ptr, 1);
        assert(lpLength(lp) == 3);
        assert(lpSkip(lpLast(lp))[0] == LP_EOF); /* check set LP_EOF correctly */
        zfree(lp);
    }

    TEST("Delete inclusive range 0,1");
    {
        lp = createList();
        lp = lpDeleteRange(lp, 0, 2);
        assert(lpLength(lp) == 2);
        verifyEntry(lpFirst(lp), (unsigned char*)mixlist[2], strlen(mixlist[2]));
        zfree(lp);

        lp = createList();
        unsigned char *ptr = lpFirst(lp);
        lp = lpDeleteRangeWithEntry(lp, &ptr, 2);
        assert(lpLength(lp) == 2);
        verifyEntry(lpFirst(lp), (unsigned char*)mixlist[2], strlen(mixlist[2]));
        zfree(lp);
    }

    TEST("Delete inclusive range 1,2");
    {
        lp = createList();
        lp = lpDeleteRange(lp, 1, 2);
        assert(lpLength(lp) == 2);
        verifyEntry(lpFirst(lp), (unsigned char*)mixlist[0], strlen(mixlist[0]));
        zfree(lp);

        lp = createList();
        unsigned char *ptr = lpSeek(lp, 1);
        lp = lpDeleteRangeWithEntry(lp, &ptr, 2);
        assert(lpLength(lp) == 2);
        verifyEntry(lpFirst(lp), (unsigned char*)mixlist[0], strlen(mixlist[0]));
        zfree(lp);
    }
    
    TEST("Delete with start index out of range");
    {
        lp = createList();
        lp = lpDeleteRange(lp, 5, 1);
        assert(lpLength(lp) == 4);
        zfree(lp);
    }

    TEST("Delete with num overflow");
    {
        lp = createList();
        lp = lpDeleteRange(lp, 1, 5);
        assert(lpLength(lp) == 1);
        verifyEntry(lpFirst(lp), (unsigned char*)mixlist[0], strlen(mixlist[0]));
        zfree(lp);

        lp = createList();
        unsigned char *ptr = lpSeek(lp, 1);
        lp = lpDeleteRangeWithEntry(lp, &ptr, 5);
        assert(lpLength(lp) == 1);
        verifyEntry(lpFirst(lp), (unsigned char*)mixlist[0], strlen(mixlist[0]));
        zfree(lp);
    }

    TEST("Batch append") {
        listpackEntry ent[6] = {
                {.sval = (unsigned char*)mixlist[0], .slen = strlen(mixlist[0])},
                {.sval = (unsigned char*)mixlist[1], .slen = strlen(mixlist[1])},
                {.sval = (unsigned char*)mixlist[2], .slen = strlen(mixlist[2])},
                {.lval = 4294967296},
                {.sval = (unsigned char*)mixlist[3], .slen = strlen(mixlist[3])},
                {.lval = -100}
        };

        lp = lpNew(0);
        lp = lpBatchAppend(lp, ent, 2);
        verifyEntry(lpSeek(lp, 0), ent[0].sval, ent[0].slen);
        verifyEntry(lpSeek(lp, 1), ent[1].sval, ent[1].slen);
        assert(lpLength(lp) == 2);

        lp = lpBatchAppend(lp, &ent[2], 1);
        verifyEntry(lpSeek(lp, 0), ent[0].sval, ent[0].slen);
        verifyEntry(lpSeek(lp, 1), ent[1].sval, ent[1].slen);
        verifyEntry(lpSeek(lp, 2), ent[2].sval, ent[2].slen);
        assert(lpLength(lp) == 3);

        lp = lpDeleteRange(lp, 1, 1);
        verifyEntry(lpSeek(lp, 0), ent[0].sval, ent[0].slen);
        verifyEntry(lpSeek(lp, 1), ent[2].sval, ent[2].slen);
        assert(lpLength(lp) == 2);

        lp = lpBatchAppend(lp, &ent[3], 3);
        verifyEntry(lpSeek(lp, 0), ent[0].sval, ent[0].slen);
        verifyEntry(lpSeek(lp, 1), ent[2].sval, ent[2].slen);
        verifyEntry(lpSeek(lp, 2), (unsigned char*) "4294967296", 10);
        verifyEntry(lpSeek(lp, 3), ent[4].sval, ent[4].slen);
        verifyEntry(lpSeek(lp, 4), (unsigned char*) "-100", 4);
        assert(lpLength(lp) == 5);

        lp = lpDeleteRange(lp, 1, 3);
        verifyEntry(lpSeek(lp, 0), ent[0].sval, ent[0].slen);
        verifyEntry(lpSeek(lp, 1), (unsigned char*) "-100", 4);
        assert(lpLength(lp) == 2);

        lpFree(lp);
    }

    TEST("Batch insert") {
        lp = lpNew(0);
        listpackEntry ent[6] = {
                {.sval = (unsigned char*)mixlist[0], .slen = strlen(mixlist[0])},
                {.sval = (unsigned char*)mixlist[1], .slen = strlen(mixlist[1])},
                {.sval = (unsigned char*)mixlist[2], .slen = strlen(mixlist[2])},
                {.lval = 4294967296},
                {.sval = (unsigned char*)mixlist[3], .slen = strlen(mixlist[3])},
                {.lval = -100}
        };

        lp = lpBatchAppend(lp, ent, 4);
        assert(lpLength(lp) == 4);
        verifyEntry(lpSeek(lp, 0), ent[0].sval, ent[0].slen);
        verifyEntry(lpSeek(lp, 1), ent[1].sval, ent[1].slen);
        verifyEntry(lpSeek(lp, 2), ent[2].sval, ent[2].slen);
        verifyEntry(lpSeek(lp, 3), (unsigned char*)"4294967296", 10);

        /* Insert with LP_BEFORE */
        p = lpSeek(lp, 3);
        lp = lpBatchInsert(lp, p, LP_BEFORE, &ent[4], 2, &p);
        verifyEntry(p, (unsigned char*)"-100", 4);
        assert(lpLength(lp) == 6);
        verifyEntry(lpSeek(lp, 0), ent[0].sval, ent[0].slen);
        verifyEntry(lpSeek(lp, 1), ent[1].sval, ent[1].slen);
        verifyEntry(lpSeek(lp, 2), ent[2].sval, ent[2].slen);
        verifyEntry(lpSeek(lp, 3), ent[4].sval, ent[4].slen);
        verifyEntry(lpSeek(lp, 4), (unsigned char*)"-100", 4);
        verifyEntry(lpSeek(lp, 5), (unsigned char*)"4294967296", 10);

        lp = lpDeleteRange(lp, 1, 2);
        assert(lpLength(lp) == 4);
        verifyEntry(lpSeek(lp, 0), ent[0].sval, ent[0].slen);
        verifyEntry(lpSeek(lp, 1), ent[4].sval, ent[4].slen);
        verifyEntry(lpSeek(lp, 2), (unsigned char*)"-100", 4);
        verifyEntry(lpSeek(lp, 3), (unsigned char*)"4294967296", 10);

        /* Insert with LP_AFTER */
        p = lpSeek(lp, 0);
        lp = lpBatchInsert(lp, p, LP_AFTER, &ent[1], 2, &p);
        verifyEntry(p, ent[2].sval, ent[2].slen);
        assert(lpLength(lp) == 6);
        verifyEntry(lpSeek(lp, 0), ent[0].sval, ent[0].slen);
        verifyEntry(lpSeek(lp, 1), ent[1].sval, ent[1].slen);
        verifyEntry(lpSeek(lp, 2), ent[2].sval, ent[2].slen);
        verifyEntry(lpSeek(lp, 3), ent[4].sval, ent[4].slen);
        verifyEntry(lpSeek(lp, 4), (unsigned char*)"-100", 4);
        verifyEntry(lpSeek(lp, 5), (unsigned char*)"4294967296", 10);

        lp = lpDeleteRange(lp, 2, 4);
        assert(lpLength(lp) == 2);
        p = lpSeek(lp, 1);
        lp = lpBatchInsert(lp, p, LP_AFTER, &ent[2], 1, &p);
        verifyEntry(p, ent[2].sval, ent[2].slen);
        assert(lpLength(lp) == 3);
        verifyEntry(lpSeek(lp, 0), ent[0].sval, ent[0].slen);
        verifyEntry(lpSeek(lp, 1), ent[1].sval, ent[1].slen);
        verifyEntry(lpSeek(lp, 2), ent[2].sval, ent[2].slen);

        lpFree(lp);
    }

    TEST("Batch delete") {
        unsigned char *lp = createList(); /* char *mixlist[] = {"hello", "foo", "quux", "1024"} */
        assert(lpLength(lp) == 4); /* Pre-condition */
        unsigned char *p0 = lpFirst(lp),
            *p1 = lpNext(lp, p0),
            *p2 = lpNext(lp, p1),
            *p3 = lpNext(lp, p2);
        unsigned char *ps[] = {p0, p1, p3};
        lp = lpBatchDelete(lp, ps, 3);
        assert(lpLength(lp) == 1);
        verifyEntry(lpFirst(lp), (unsigned char*)mixlist[2], strlen(mixlist[2]));
        assert(lpValidateIntegrity(lp, lpBytes(lp), 1, NULL, NULL) == 1);
        lpFree(lp);
    }

    TEST("Delete foo while iterating") {
        lp = createList();
        p = lpFirst(lp);
        while (p) {
            if (lpCompare(p, (unsigned char*)"foo", 3)) {
                lp = lpDelete(lp, p, &p);
            } else {
                p = lpNext(lp, p);
            }
        }
        lpFree(lp);
    }

    TEST("Replace with same size") {
        lp = createList(); /* "hello", "foo", "quux", "1024" */
        unsigned char *orig_lp = lp;
        p = lpSeek(lp, 0);
        lp = lpReplace(lp, &p, (unsigned char*)"zoink", 5);
        p = lpSeek(lp, 3);
        lp = lpReplace(lp, &p, (unsigned char*)"y", 1);
        p = lpSeek(lp, 1);
        lp = lpReplace(lp, &p, (unsigned char*)"65536", 5);
        p = lpSeek(lp, 0);
        assert(!memcmp((char*)p,
                       "\x85zoink\x06"
                       "\xf2\x00\x00\x01\x04" /* 65536 as int24 */
                       "\x84quux\05" "\x81y\x02" "\xff",
                       22));
        assert(lp == orig_lp); /* no reallocations have happened */
        lpFree(lp);
    }

    TEST("Replace with different size") {
        lp = createList(); /* "hello", "foo", "quux", "1024" */
        p = lpSeek(lp, 1);
        lp = lpReplace(lp, &p, (unsigned char*)"squirrel", 8);
        p = lpSeek(lp, 0);
        assert(!strncmp((char*)p,
                        "\x85hello\x06" "\x88squirrel\x09" "\x84quux\x05"
                        "\xc4\x00\x02" "\xff",
                        27));
        lpFree(lp);
    }

    TEST("Regression test for >255 byte strings") {
        char v1[257] = {0}, v2[257] = {0};
        memset(v1,'x',256);
        memset(v2,'y',256);
        lp = lpNew(0);
        lp = lpAppend(lp, (unsigned char*)v1 ,strlen(v1));
        lp = lpAppend(lp, (unsigned char*)v2 ,strlen(v2));

        /* Pop values again and compare their value. */
        p = lpFirst(lp);
        vstr = lpGet(p, &vlen, NULL);
        assert(strncmp(v1, (char*)vstr, vlen) == 0);
        p = lpSeek(lp, 1);
        vstr = lpGet(p, &vlen, NULL);
        assert(strncmp(v2, (char*)vstr, vlen) == 0);
        lpFree(lp);
    }

    TEST("Create long list and check indices") {
        lp = lpNew(0);
        char buf[32];
        int i,len;
        for (i = 0; i < 1000; i++) {
            len = snprintf(buf, sizeof(buf), "%d", i);
            lp = lpAppend(lp, (unsigned char*)buf, len);
        }
        for (i = 0; i < 1000; i++) {
            p = lpSeek(lp, i);
            vstr = lpGet(p, &vlen, NULL);
            assert(i == vlen);

            p = lpSeek(lp, -i-1);
            vstr = lpGet(p, &vlen, NULL);
            assert(999-i == vlen);
        }
        lpFree(lp);
    }

    TEST("Compare strings with listpack entries") {
        lp = createList();
        p = lpSeek(lp,0);
        assert(lpCompare(p,(unsigned char*)"hello",5));
        assert(!lpCompare(p,(unsigned char*)"hella",5));

        p = lpSeek(lp,3);
        assert(lpCompare(p,(unsigned char*)"1024",4));
        assert(!lpCompare(p,(unsigned char*)"1025",4));
        lpFree(lp);
    }

    TEST("lpMerge two empty listpacks") {
        unsigned char *lp1 = lpNew(0);
        unsigned char *lp2 = lpNew(0);

        /* Merge two empty listpacks, get empty result back. */
        lp1 = lpMerge(&lp1, &lp2);
        assert(lpLength(lp1) == 0);
        zfree(lp1);
    }

    TEST("lpMerge two listpacks - first larger than second") {
        unsigned char *lp1 = createIntList();
        unsigned char *lp2 = createList();

        size_t lp1_bytes = lpBytes(lp1);
        size_t lp2_bytes = lpBytes(lp2);
        unsigned long lp1_len = lpLength(lp1);
        unsigned long lp2_len = lpLength(lp2);

        unsigned char *lp3 = lpMerge(&lp1, &lp2);
        assert(lp3 == lp1);
        assert(lp2 == NULL);
        assert(lpLength(lp3) == (lp1_len + lp2_len));
        assert(lpBytes(lp3) == (lp1_bytes + lp2_bytes - LP_HDR_SIZE - 1));
        verifyEntry(lpSeek(lp3, 0), (unsigned char*)"4294967296", 10);
        verifyEntry(lpSeek(lp3, 5), (unsigned char*)"much much longer non integer", 28);
        verifyEntry(lpSeek(lp3, 6), (unsigned char*)"hello", 5);
        verifyEntry(lpSeek(lp3, -1), (unsigned char*)"1024", 4);
        zfree(lp3);
    }

    TEST("lpMerge two listpacks - second larger than first") {
        unsigned char *lp1 = createList();
        unsigned char *lp2 = createIntList();

        size_t lp1_bytes = lpBytes(lp1);
        size_t lp2_bytes = lpBytes(lp2);
        unsigned long lp1_len = lpLength(lp1);
        unsigned long lp2_len = lpLength(lp2);

        unsigned char *lp3 = lpMerge(&lp1, &lp2);
        assert(lp3 == lp2);
        assert(lp1 == NULL);
        assert(lpLength(lp3) == (lp1_len + lp2_len));
        assert(lpBytes(lp3) == (lp1_bytes + lp2_bytes - LP_HDR_SIZE - 1));
        verifyEntry(lpSeek(lp3, 0), (unsigned char*)"hello", 5);
        verifyEntry(lpSeek(lp3, 3), (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp3, 4), (unsigned char*)"4294967296", 10);
        verifyEntry(lpSeek(lp3, -1), (unsigned char*)"much much longer non integer", 28);
        zfree(lp3);
    }

    TEST("lpNextRandom normal usage") {
        /* Create some data */
        unsigned char *lp = lpNew(0);
        unsigned char buf[100] = "asdf";
        unsigned int size = 100;
        for (size_t i = 0; i < size; i++) {
            lp = lpAppend(lp, buf, i);
        }
        assert(lpLength(lp) == size);

        /* Pick a subset of the elements of every possible subset size */
        for (unsigned int count = 0; count <= size; count++) {
            unsigned int remaining = count;
            unsigned char *p = lpFirst(lp);
            unsigned char *prev = NULL;
            unsigned index = 0;
            while (remaining > 0) {
                assert(p != NULL);
                p = lpNextRandom(lp, p, &index, remaining--, 1);
                assert(p != NULL);
                assert(p != prev);
                prev = p;
                p = lpNext(lp, p);
                index++;
            }
        }
        lpFree(lp);
    }

    TEST("lpNextRandom corner cases") {
        unsigned char *lp = lpNew(0);
        unsigned i = 0;

        /* Pick from empty listpack returns NULL. */
        assert(lpNextRandom(lp, NULL, &i, 2, 1) == NULL);

        /* Add some elements and find their pointers within the listpack. */
        lp = lpAppend(lp, (unsigned char *)"abc", 3);
        lp = lpAppend(lp, (unsigned char *)"def", 3);
        lp = lpAppend(lp, (unsigned char *)"ghi", 3);
        assert(lpLength(lp) == 3);
        unsigned char *p0 = lpFirst(lp);
        unsigned char *p1 = lpNext(lp, p0);
        unsigned char *p2 = lpNext(lp, p1);
        assert(lpNext(lp, p2) == NULL);

        /* Pick zero elements returns NULL. */
        i = 0; assert(lpNextRandom(lp, lpFirst(lp), &i, 0, 1) == NULL);

        /* Pick all returns all. */
        i = 0; assert(lpNextRandom(lp, p0, &i, 3, 1) == p0 && i == 0);
        i = 1; assert(lpNextRandom(lp, p1, &i, 2, 1) == p1 && i == 1);
        i = 2; assert(lpNextRandom(lp, p2, &i, 1, 1) == p2 && i == 2);

        /* Pick more than one when there's only one left returns the last one. */
        i = 2; assert(lpNextRandom(lp, p2, &i, 42, 1) == p2 && i == 2);

        /* Pick all even elements returns p0 and p2. */
        i = 0; assert(lpNextRandom(lp, p0, &i, 10, 2) == p0 && i == 0);
        i = 1; assert(lpNextRandom(lp, p1, &i, 10, 2) == p2 && i == 2);

        /* Don't crash even for bad index. */
        for (int j = 0; j < 100; j++) {
            unsigned char *p;
            switch (j % 4) {
            case 0: p = p0; break;
            case 1: p = p1; break;
            case 2: p = p2; break;
            case 3: p = NULL; break;
            }
            i = j % 7;
            unsigned int remaining = j % 5;
            p = lpNextRandom(lp, p, &i, remaining, 1);
            assert(p == p0 || p == p1 || p == p2 || p == NULL);
        }
        lpFree(lp);
    }

    TEST("Random pair with one element") {
        listpackEntry key, val;
        unsigned char *lp = lpNew(0);
        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lpRandomPair(lp, 1, &key, &val, 2);
        assert(memcmp(key.sval, "abc", key.slen) == 0);
        assert(val.lval == 123);
        lpFree(lp);
    }

    TEST("Random pair with many elements") {
        listpackEntry key, val;
        unsigned char *lp = lpNew(0);
        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lp = lpAppend(lp, (unsigned char*)"456", 3);
        lp = lpAppend(lp, (unsigned char*)"def", 3);
        lpRandomPair(lp, 2, &key, &val, 2);
        if (key.sval) {
            assert(!memcmp(key.sval, "abc", key.slen));
            assert(key.slen == 3);
            assert(val.lval == 123);
        }
        if (!key.sval) {
            assert(key.lval == 456);
            assert(!memcmp(val.sval, "def", val.slen));
        }
        lpFree(lp);
    }

    TEST("Random pair with tuple_len 3") {
        listpackEntry key, val;
        unsigned char *lp = lpNew(0);
        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lp = lpAppend(lp, (unsigned char*)"xxx", 3);
        lp = lpAppend(lp, (unsigned char*)"456", 3);
        lp = lpAppend(lp, (unsigned char*)"def", 3);
        lp = lpAppend(lp, (unsigned char*)"xxx", 3);
        lp = lpAppend(lp, (unsigned char*)"281474976710655", 15);
        lp = lpAppend(lp, (unsigned char*)"789", 3);
        lp = lpAppend(lp, (unsigned char*)"xxx", 3);

        for (int i = 0; i < 5; i++) {
            lpRandomPair(lp, 3, &key, &val, 3);
            if (key.sval) {
                if (!memcmp(key.sval, "abc", key.slen)) {
                    assert(key.slen == 3);
                    assert(val.lval == 123);
                } else {
                    assert(0);
                };
            }
            if (!key.sval) {
                if (key.lval == 456)
                    assert(!memcmp(val.sval, "def", val.slen));
                else if (key.lval == 281474976710655LL)
                    assert(val.lval == 789);
                else
                    assert(0);
            }
        }

        lpFree(lp);
    }

    TEST("Random pairs with one element") {
        int count = 5;
        unsigned char *lp = lpNew(0);
        listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
        listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lpRandomPairs(lp, count, keys, vals, 2);
        assert(memcmp(keys[4].sval, "abc", keys[4].slen) == 0);
        assert(vals[4].lval == 123);
        zfree(keys);
        zfree(vals);
        lpFree(lp);
    }

    TEST("Random pairs with many elements") {
        int count = 5;
        lp = lpNew(0);
        listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
        listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lp = lpAppend(lp, (unsigned char*)"456", 3);
        lp = lpAppend(lp, (unsigned char*)"def", 3);
        lpRandomPairs(lp, count, keys, vals, 2);
        for (int i = 0; i < count; i++) {
            if (keys[i].sval) {
                assert(!memcmp(keys[i].sval, "abc", keys[i].slen));
                assert(keys[i].slen == 3);
                assert(vals[i].lval == 123);
            }
            if (!keys[i].sval) {
                assert(keys[i].lval == 456);
                assert(!memcmp(vals[i].sval, "def", vals[i].slen));
            }
        }
        zfree(keys);
        zfree(vals);
        lpFree(lp);
    }

    TEST("Random pairs with many elements and tuple_len 3") {
        int count = 5;
        lp = lpNew(0);
        listpackEntry *keys = zcalloc(sizeof(listpackEntry) * count);
        listpackEntry *vals = zcalloc(sizeof(listpackEntry) * count);

        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lp = lpAppend(lp, (unsigned char*)"xxx", 3);
        lp = lpAppend(lp, (unsigned char*)"456", 3);
        lp = lpAppend(lp, (unsigned char*)"def", 3);
        lp = lpAppend(lp, (unsigned char*)"xxx", 3);
        lp = lpAppend(lp, (unsigned char*)"281474976710655", 15);
        lp = lpAppend(lp, (unsigned char*)"789", 3);
        lp = lpAppend(lp, (unsigned char*)"xxx", 3);

        lpRandomPairs(lp, count, keys, vals, 3);
        for (int i = 0; i < count; i++) {
            if (keys[i].sval) {
                if (!memcmp(keys[i].sval, "abc", keys[i].slen)) {
                    assert(keys[i].slen == 3);
                    assert(vals[i].lval == 123);
                } else {
                    assert(0);
                };
            }
            if (!keys[i].sval) {
                if (keys[i].lval == 456)
                    assert(!memcmp(vals[i].sval, "def", vals[i].slen));
                else if (keys[i].lval == 281474976710655LL)
                    assert(vals[i].lval == 789);
                else
                    assert(0);
            }
        }

        zfree(keys);
        zfree(vals);
        lpFree(lp);
    }

    TEST("Random pairs unique with one element") {
        unsigned picked;
        int count = 5;
        lp = lpNew(0);
        listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
        listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        picked = lpRandomPairsUnique(lp, count, keys, vals, 2);
        assert(picked == 1);
        assert(memcmp(keys[0].sval, "abc", keys[0].slen) == 0);
        assert(vals[0].lval == 123);
        zfree(keys);
        zfree(vals);
        lpFree(lp);
    }

    TEST("Random pairs unique with many elements") {
        unsigned picked;
        int count = 5;
        lp = lpNew(0);
        listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
        listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lp = lpAppend(lp, (unsigned char*)"456", 3);
        lp = lpAppend(lp, (unsigned char*)"def", 3);
        picked = lpRandomPairsUnique(lp, count, keys, vals, 2);
        assert(picked == 2);
        for (int i = 0; i < 2; i++) {
            if (keys[i].sval) {
                assert(!memcmp(keys[i].sval, "abc", keys[i].slen));
                assert(keys[i].slen == 3);
                assert(vals[i].lval == 123);
            }
            if (!keys[i].sval) {
                assert(keys[i].lval == 456);
                assert(!memcmp(vals[i].sval, "def", vals[i].slen));
            }
        }
        zfree(keys);
        zfree(vals);
        lpFree(lp);
    }

    TEST("Random pairs unique with many elements and tuple_len 3") {
        unsigned picked;
        int count = 5;
        lp = lpNew(0);
        listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
        listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lp = lpAppend(lp, (unsigned char*)"xxx", 3);
        lp = lpAppend(lp, (unsigned char*)"456", 3);
        lp = lpAppend(lp, (unsigned char*)"def", 3);
        lp = lpAppend(lp, (unsigned char*)"xxx", 3);
        lp = lpAppend(lp, (unsigned char*)"281474976710655", 15);
        lp = lpAppend(lp, (unsigned char*)"789", 3);
        lp = lpAppend(lp, (unsigned char*)"xxx", 3);
        picked = lpRandomPairsUnique(lp, count, keys, vals, 3);
        assert(picked == 3);
        for (int i = 0; i < 3; i++) {
            if (keys[i].sval) {
                if (!memcmp(keys[i].sval, "abc", keys[i].slen)) {
                    assert(keys[i].slen == 3);
                    assert(vals[i].lval == 123);
                } else {
                    assert(0);
                };
            }
            if (!keys[i].sval) {
                if (keys[i].lval == 456)
                    assert(!memcmp(vals[i].sval, "def", vals[i].slen));
                else if (keys[i].lval == 281474976710655LL)
                    assert(vals[i].lval == 789);
                else
                    assert(0);
            }
        }
        zfree(keys);
        zfree(vals);
        lpFree(lp);
    }

    TEST("push various encodings") {
        lp = lpNew(0);

        /* Push integer encode element using lpAppend */
        lp = lpAppend(lp, (unsigned char*)"127", 3);
        assert(LP_ENCODING_IS_7BIT_UINT(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)"4095", 4);
        assert(LP_ENCODING_IS_13BIT_INT(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)"32767", 5);
        assert(LP_ENCODING_IS_16BIT_INT(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)"8388607", 7);
        assert(LP_ENCODING_IS_24BIT_INT(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)"2147483647", 10);
        assert(LP_ENCODING_IS_32BIT_INT(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)"9223372036854775807", 19);
        assert(LP_ENCODING_IS_64BIT_INT(lpLast(lp)[0]));

        /* Push integer encode element using lpAppendInteger */
        lp = lpAppendInteger(lp, 127);
        assert(LP_ENCODING_IS_7BIT_UINT(lpLast(lp)[0]));
        verifyEntry(lpLast(lp), (unsigned char*)"127", 3);
        lp = lpAppendInteger(lp, 4095);
        verifyEntry(lpLast(lp), (unsigned char*)"4095", 4);
        assert(LP_ENCODING_IS_13BIT_INT(lpLast(lp)[0]));
        lp = lpAppendInteger(lp, 32767);
        verifyEntry(lpLast(lp), (unsigned char*)"32767", 5);
        assert(LP_ENCODING_IS_16BIT_INT(lpLast(lp)[0]));
        lp = lpAppendInteger(lp, 8388607);
        verifyEntry(lpLast(lp), (unsigned char*)"8388607", 7);
        assert(LP_ENCODING_IS_24BIT_INT(lpLast(lp)[0]));
        lp = lpAppendInteger(lp, 2147483647);
        verifyEntry(lpLast(lp), (unsigned char*)"2147483647", 10);
        assert(LP_ENCODING_IS_32BIT_INT(lpLast(lp)[0]));
        lp = lpAppendInteger(lp, 9223372036854775807);
        verifyEntry(lpLast(lp), (unsigned char*)"9223372036854775807", 19);
        assert(LP_ENCODING_IS_64BIT_INT(lpLast(lp)[0]));

        /* string encode */
        unsigned char *str = zmalloc(65535);
        memset(str, 0, 65535);
        lp = lpAppend(lp, (unsigned char*)str, 63);
        assert(LP_ENCODING_IS_6BIT_STR(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)str, 4095);
        assert(LP_ENCODING_IS_12BIT_STR(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)str, 65535);
        assert(LP_ENCODING_IS_32BIT_STR(lpLast(lp)[0]));
        zfree(str);
        lpFree(lp);
    }

    TEST("Test lpFind") {
        lp = createList();
        assert(lpFind(lp, lpFirst(lp), (unsigned char*)"abc", 3, 0) == NULL);
        verifyEntry(lpFind(lp, lpFirst(lp), (unsigned char*)"hello", 5, 0), (unsigned char*)"hello", 5);
        verifyEntry(lpFind(lp, lpFirst(lp), (unsigned char*)"1024", 4, 0), (unsigned char*)"1024", 4);
        lpFree(lp);
    }

    TEST("Test lpFindCb") {
        lp = createList(); /* "hello", "foo", "quux", "1024" */
        assert(lpFindCb(lp, lpFirst(lp), "abc", lpFindCbCmp, 0) == NULL);
        verifyEntry(lpFindCb(lp, NULL, "hello", lpFindCbCmp, 0), (unsigned char*)"hello", 5);
        verifyEntry(lpFindCb(lp, NULL, "1024", lpFindCbCmp, 0), (unsigned char*)"1024", 4);
        verifyEntry(lpFindCb(lp, NULL, "quux", lpFindCbCmp, 0), (unsigned char*)"quux", 4);
        verifyEntry(lpFindCb(lp, NULL, "foo", lpFindCbCmp, 0), (unsigned char*)"foo", 3);
        lpFree(lp);

        lp = lpNew(0);
        assert(lpFindCb(lp, lpFirst(lp), "hello", lpFindCbCmp, 0) == NULL);
        assert(lpFindCb(lp, lpFirst(lp), "1024", lpFindCbCmp, 0) == NULL);
        lpFree(lp);
    }

    TEST("Test lpValidateIntegrity") {
        lp = createList();
        long count = 0;
        assert(lpValidateIntegrity(lp, lpBytes(lp), 1, lpValidation, &count) == 1);
        lpFree(lp);
    }

    TEST("Test number of elements exceeds LP_HDR_NUMELE_UNKNOWN") {
        lp = lpNew(0);
        // 这里写了 LP_HDR_NUMELE_UNKNOWN + 1 个元素
        for (int i = 0; i < LP_HDR_NUMELE_UNKNOWN + 1; i++)
            lp = lpAppend(lp, (unsigned char*)"1", 1);
        // 插入 LP_HDR_NUMELE_UNKNOWN 个元素
        assert(lpGetNumElements(lp) == LP_HDR_NUMELE_UNKNOWN);
        assert(lpLength(lp) == LP_HDR_NUMELE_UNKNOWN+1);

        lp = lpDeleteRange(lp, -2, 2);
        assert(lpGetNumElements(lp) == LP_HDR_NUMELE_UNKNOWN);
        assert(lpLength(lp) == LP_HDR_NUMELE_UNKNOWN-1);
        // 元素少于 LP_HDR_NUMELE_UNKNOWN, 调用 lpLength 更新元素数量
        assert(lpGetNumElements(lp) == LP_HDR_NUMELE_UNKNOWN-1); /* update length after lpLength */
        lpFree(lp);
    }

    TEST("Test number of elements exceeds LP_HDR_NUMELE_UNKNOWN with batch insert") {
        listpackEntry ent[2] = {
                {.sval = (unsigned char*)mixlist[0], .slen = strlen(mixlist[0])},
                {.sval = (unsigned char*)mixlist[1], .slen = strlen(mixlist[1])}
        };

        lp = lpNew(0);
        for (int i = 0; i < (LP_HDR_NUMELE_UNKNOWN/2) + 1; i++)
            lp = lpBatchAppend(lp, ent, 2);

        assert(lpGetNumElements(lp) == LP_HDR_NUMELE_UNKNOWN);
        assert(lpLength(lp) == LP_HDR_NUMELE_UNKNOWN+1);

        lp = lpDeleteRange(lp, -2, 2);
        assert(lpGetNumElements(lp) == LP_HDR_NUMELE_UNKNOWN);
        assert(lpLength(lp) == LP_HDR_NUMELE_UNKNOWN-1);
        assert(lpGetNumElements(lp) == LP_HDR_NUMELE_UNKNOWN-1); /* update length after lpLength */
        lpFree(lp);
    }

    TEST("Stress with random payloads of different encoding") {
        unsigned long long start = usec();
        int i,j,len,where;
        unsigned char *p;
        char buf[1024];
        int buflen;
        list *ref;
        listNode *refnode;

        int iteration = accurate ? 20000 : 20;
        for (i = 0; i < iteration; i++) {
            lp = lpNew(0);
            ref = listCreate();
            listSetFreeMethod(ref, sdsfreegeneric);
            len = rand() % 256;

            /* Create lists */
            for (j = 0; j < len; j++) {
                where = (rand() & 1) ? 0 : 1;
                if (rand() % 2) {
                    buflen = randstring(buf,1,sizeof(buf)-1);
                } else {
                    switch(rand() % 3) {
                    case 0:
                        buflen = snprintf(buf,sizeof(buf),"%lld",(0LL + rand()) >> 20);
                        break;
                    case 1:
                        buflen = snprintf(buf,sizeof(buf),"%lld",(0LL + rand()));
                        break;
                    case 2:
                        buflen = snprintf(buf,sizeof(buf),"%lld",(0LL + rand()) << 20);
                        break;
                    default:
                        assert(NULL);
                    }
                }

                /* Add to listpack */
                if (where == 0) {
                    lp = lpPrepend(lp, (unsigned char*)buf, buflen);
                } else {
                    lp = lpAppend(lp, (unsigned char*)buf, buflen);
                }

                /* Add to reference list */
                if (where == 0) {
                    listAddNodeHead(ref,sdsnewlen(buf, buflen));
                } else if (where == 1) {
                    listAddNodeTail(ref,sdsnewlen(buf, buflen));
                } else {
                    assert(NULL);
                }
            }

            assert(listLength(ref) == lpLength(lp));
            for (j = 0; j < len; j++) {
                /* Naive way to get elements, but similar to the stresser
                 * executed from the Tcl test suite. */
                p = lpSeek(lp,j);
                refnode = listIndex(ref,j);

                vstr = lpGet(p, &vlen, intbuf);
                assert(memcmp(vstr,listNodeValue(refnode),vlen) == 0);
            }
            lpFree(lp);
            listRelease(ref);
        }
        printf("Done. usec=%lld\n\n", usec()-start);
    }

    TEST("Stress with variable listpack size") {
        unsigned long long start = usec();
        int maxsize = accurate ? 16384 : 16;
        stress(0,100000,maxsize,256);
        stress(1,100000,maxsize,256);
        printf("Done. usec=%lld\n\n", usec()-start);
    }

    /* Benchmarks */
    {
        int iteration = accurate ? 100000 : 100;
        lp = lpNew(0);
        TEST("Benchmark lpAppend") {
            unsigned long long start = usec();
            for (int i=0; i<iteration; i++) {
                char buf[4096] = "asdf";
                lp = lpAppend(lp, (unsigned char*)buf, 4);
                lp = lpAppend(lp, (unsigned char*)buf, 40);
                lp = lpAppend(lp, (unsigned char*)buf, 400);
                lp = lpAppend(lp, (unsigned char*)buf, 4000);
                lp = lpAppend(lp, (unsigned char*)"1", 1);
                lp = lpAppend(lp, (unsigned char*)"10", 2);
                lp = lpAppend(lp, (unsigned char*)"100", 3);
                lp = lpAppend(lp, (unsigned char*)"1000", 4);
                lp = lpAppend(lp, (unsigned char*)"10000", 5);
                lp = lpAppend(lp, (unsigned char*)"100000", 6);
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        TEST("Benchmark lpFind string") {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                unsigned char *fptr = lpFirst(lp);
                fptr = lpFind(lp, fptr, (unsigned char*)"nothing", 7, 1);
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        TEST("Benchmark lpFind number") {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                unsigned char *fptr = lpFirst(lp);
                fptr = lpFind(lp, fptr, (unsigned char*)"99999", 5, 1);
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        TEST("Benchmark lpSeek") {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                lpSeek(lp, 99999);
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        TEST("Benchmark lpValidateIntegrity") {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                lpValidateIntegrity(lp, lpBytes(lp), 1, NULL, NULL);
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        TEST("Benchmark lpCompare with string") {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                unsigned char *eptr = lpSeek(lp,0);
                while (eptr != NULL) {
                    lpCompare(eptr,(unsigned char*)"nothing",7);
                    eptr = lpNext(lp,eptr);
                }
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        TEST("Benchmark lpCompare with number") {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                unsigned char *eptr = lpSeek(lp,0);
                while (eptr != NULL) {
                    lpCompare(lp, (unsigned char*)"99999", 5);
                    eptr = lpNext(lp,eptr);
                }
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        lpFree(lp);
    }

    return 0;
}

#endif

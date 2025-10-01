/*
 * Copyright Redis Ltd. 2024 - present
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 *
 * WHAT IS MSTR (M-STRING)?
 * ------------------------
 * mstr stands for immutable string with optional metadata attached.
 *
 * sds string is widely used across the system and serves as a general purpose
 * container to hold data. The need to optimize memory and aggregate strings
 * along with metadata and store it into Redis data-structures as single bulk keep
 * reoccur. One thought might be, why not to extend sds to support metadata. The
 * answer is that sds is mutable string in its nature, with wide API (split, join,
 * etc.). Pushing metadata logic into sds will make it very fragile, and complex
 * to maintain.
 *
 * sds 字符串在整个系统中被广泛使用，是一种通用的数据存储容器。然而，在实际应用中，经常需要优化内存使用，
 * 将字符串与元数据（metadata）聚合在一起，并作为单个单个数据块存储到 Redis 数据结构中。
 * 有人可能会想，为什么不扩展 sds 使其支持元数据呢？答案是，sds 本质上是可变字符串，
 * 且拥有丰富的 API（如分割、拼接等操作）。如果将元数据逻辑嵌入到 sds 中，会导致其变得非常脆弱，且难以维护
 *
 * Another idea involved using a simple struct with flags and a dynamic buf[] at the
 * end. While this could be viable, it introduces considerable complexity and would
 * need maintenance across different contexts.
 * 另一个思路是使用一个简单的结构体，包含标志（flags）和末尾的动态缓冲区（buf []）。
 * 虽然这在理论上可行，但会引入相当大的复杂性，而且需要在不同场景下进行维护
 *
 * As an alternative, we introduce a new implementation of immutable strings,
 * with limited API, and with the option to attach metadata. The representation
 * of the string, without any metadata, in its basic form, resembles SDS but
 * without the API to manipulate the string. Only to attach metadata to it. The
 * following diagram shows the memory layout of mstring (mstrhdr8) when no
 * metadata is attached:
 *
 * 作为替代方案，我们引入了一种新的不可变字符串实现，它具有有限的 API，并且可以附加元数据。
 * 在不包含任何元数据的情况下，这种字符串的基本表示形式与 SDS 类似，但没有用于操作字符串的 API，
 * 仅支持为其附加元数据。下图展示了不附加元数据时，mstring（mstrhdr8）的内存布局
 *
 *  struct __attribute__ ((__packed__)) mstrhdr8 {
 *     uint8_t unused;
 *     uint8_t len;
 *     unsigned char info;
 *      char buf[];
 * };
 *
 *     +----------------------------------------------+
 *     | mstrhdr8                       | c-string |  |
 *     +--------------------------------+-------------+
 *     |8b   |2b     |1b      |5b       |?bytes    |8b|
 *     | Len | Type  |m-bit=0 | Unused  | String   |\0|
 *     +----------------------------------------------+
 *                                      ^
 *                                      |
 *  mstrNew() returns pointer to here --+
 *
 * If  metadata-flag is set, depicted in diagram above as m-bit in the diagram,
 * then the header will be preceded with additional 16 bits of metadata flags such
 * that if i'th bit is set, then the i'th metadata structure is attached to the
 * mstring. The metadata layout and their sizes are defined by mstrKind structure
 * (More below).
 *
 * The following diagram shows the memory layout of mstr (mstrhdr8) when 3 bits in mFlags
 * are set to indicate that 3 fields of metadata are attached to the mstring at the
 * beginning.
 *
 *   TYPE 指 MSTR_TYPE_5,MSTR_TYPE_8,MSTR_TYPE_16,MSTR_TYPE_64
 *   m-bit 是否有元数据
 *
 *   在小端系统中, mstrhdr8 的内存布局为
 *
 *   +-------------------------------------------------------------------------------+
 *   | METADATA FIELDS       | mflags | mstrhdr8                       | c-string |  |
 *   +-----------------------+--------+--------------------------------+-------------+
 *   |?bytes |?bytes |?bytes |16b     |8b   |5b     |1b      |2b       |?bytes    |8b|
 *   | Meta3 | Meta2 | Meta0 | 0x1101 | Len |Unused |m-bit=1 | Type    | String   |\0|
 *   +-------------------------------------------------------------------------------+
 *                                                                     ^
 *                                                                     |
 *                         mstrNewWithMeta() returns pointer to here --+
 *
 *
 *   下面是官方的注释, 这里画的结构应该是错误的
 *   mstrIsMetaAttached 中明确倒数第 3 个位为 m-bit
 *
 *   +-------------------------------------------------------------------------------+
 *   | METADATA FIELDS       | mflags | mstrhdr8                       | c-string |  |
 *   +-----------------------+--------+--------------------------------+-------------+
 *   |?bytes |?bytes |?bytes |16b     |8b   |2b     |1b      |5b       |?bytes    |8b|
 *   | Meta3 | Meta2 | Meta0 | 0x1101 | Len | Type  |m-bit=1 | Unused  | String   |\0|
 *   +-------------------------------------------------------------------------------+
 *                                                                     ^
 *                                                                     |
 *                         mstrNewWithMeta() returns pointer to here --+
 *
 * mstr allows to define different kinds (groups) of mstrings, each with its
 * own unique metadata layout. For example, in case of hash-fields, all instances of
 * it can optionally have TTL metadata attached to it. This is achieved by first
 * prototyping a single mstrKind structure that defines the metadata layout and sizes
 * of this specific kind. Now each hash-field instance has still the freedom to
 * attach or not attach the metadata to it, and metadata flags (mFlags) of the
 * instance will reflect this decision.
 *
 * mstr 允许定义不同种类（组）的 mstring，每种都有其独特的元数据布局。例如，对于哈希字段（hash-fields），
 * 其所有实例都可以选择性地附加 TTL 元数据。要实现这一点，首先需要定义一个 mstrKind 结构体原型，
 * 用于指定该特定种类的元数据布局和大小。这样一来，每个哈希字段实例仍然可以自由选择是否附加元数据，
 * 而实例的元数据标志（mFlags）会反映这一选择。
 *
 * In the future, the keys of Redis keyspace can be another kind of mstring that
 * has TTL, LRU or even dictEntry metadata embedded into. Unlike vptr in c++, this
 * struct won't be attached to mstring but will be passed as yet another argument
 * to API, to save memory. In addition, each instance of a given mstrkind can hold
 * any subset of metadata and the 8 bits of metadata-flags will reflect it.
 *
 * 未来，Redis 键空间的键可以是另一种 mstring，其中可以嵌入 TTL、LRU 甚至 dictEntry 等元数据。
 * 与 C++ 中的 vptr 不同，这种结构体不会附加到 mstring 上，而是作为另一个参数传递给 API，以节省内存。此外，
 * 给定 mstrkind 的每个实例可以包含元数据的任意子集，而 8 位的元数据标志（metadata-flags）会反映这一点。
 *
 * The following example shows how to define mstrKind for possible future keyspace
 * that aggregates several keyspace related metadata into one compact, singly
 * allocated, mstring.
 *
 *      typedef enum HkeyMetaFlags {
 *          HKEY_META_VAL_REF_COUNT    = 0,  // refcount
 *          HKEY_META_VAL_REF          = 1,  // Val referenced
 *          HKEY_META_EXPIRE           = 2,  // TTL and more
 *          HKEY_META_TYPE_ENC_LRU     = 3,  // TYPE + LRU + ENC
 *          HKEY_META_DICT_ENT_NEXT    = 4,  // Next dict entry
 *          // Following two must be together and in this order
 *          HKEY_META_VAL_EMBED8       = 5,  // Val embedded, max 7 bytes
 *          HKEY_META_VAL_EMBED16      = 6,  // Val embedded, max 15 bytes (23 with EMBED8)
 *      } HkeyMetaFlags;
 *
 *      mstrKind hkeyKind = {
 *          .name = "hkey",
 *          .metaSize[HKEY_META_VAL_REF_COUNT] = 4,
 *          .metaSize[HKEY_META_VAL_REF]       = 8,
 *          .metaSize[HKEY_META_EXPIRE]        = sizeof(ExpireMeta),
 *          .metaSize[HKEY_META_TYPE_ENC_LRU]  = 8,
 *          .metaSize[HKEY_META_DICT_ENT_NEXT] = 8,
 *          .metaSize[HKEY_META_VAL_EMBED8]    = 8,
 *          .metaSize[HKEY_META_VAL_EMBED16]   = 16,
 *      };
 *
 * MSTR-ALIGNMENT
 * --------------
 * There are two types of alignments to take into consideration:
 * 1. Alignment of the metadata.
 * 2. Alignment of returned mstr pointer
 *
 * 1) As the metadatas layout are reversed to their enumeration, it is recommended
 *    to put metadata with "better" alignment first in memory layout (enumerated
 *    last) and the worst, or those that simply don't require any alignment will be
 *    last in memory layout (enumerated first). This is similar the to the applied
 *    consideration when defining new struct in C. Note also that each metadata
 *    might either be attached to mstr or not which complicates the design phase
 *    of a new mstrKind a little.
 *
 *    In the example above, HKEY_META_VAL_REF_COUNT, with worst alignment of 4
 *    bytes, is enumerated first, and therefore, will be last in memory layout.
 *
 * 2) Few optimizations in Redis rely on the fact that sds address is always an odd
 *    pointer. We can achieve the same with a little effort. It was already taken
 *    care that all headers of type mstrhdrX has odd size. With that in mind, if
 *    a new kind of mstr is required to be limited to odd addresses, then we must
 *    make sure that sizes of all related metadatas that are defined in mstrKind
 *    are even in size.
 *
 *    Redis的一些优化依赖于SDS地址总是奇数指针这一特性
 *    MSTR设计时也考虑了实现同样的特性
 *
 *    所有mstrhdrX类型的头部大小都被设计为奇数
 *    如果需要MSTR指针地址保持为奇数，则要求在mstrKind中定义的所有相关元数据大小都必须是偶数
 *    这是因为偶数大小的元数据不会改变指针的奇偶性
 *
 */

#ifndef __MSTR_H
#define __MSTR_H

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

/* Selective copy of ifndef from server.h instead of including it */
#ifndef static_assert
#define static_assert(expr, lit) extern char __static_assert_failure[(expr) ? 1:-1]
#endif

#define MSTR_TYPE_5         0
#define MSTR_TYPE_8         1
#define MSTR_TYPE_16        2
#define MSTR_TYPE_64        3
#define MSTR_TYPE_MASK      3
#define MSTR_TYPE_BITS      2

#define MSTR_META_MASK      4

#define MSTR_HDR(T,s) ((struct mstrhdr##T *)((s)-(sizeof(struct mstrhdr##T))))
#define MSTR_HDR_VAR(T,s) struct mstrhdr##T *sh = (void*)((s)-(sizeof(struct mstrhdr##T)));

#define MSTR_META_BITS  1  /* is metadata attached? */
#define MSTR_TYPE_5_LEN(f) ((f) >> (MSTR_TYPE_BITS + MSTR_META_BITS))
#define CREATE_MSTR_INFO(len, ismeta, type) ( (((len<<MSTR_META_BITS) + ismeta) << (MSTR_TYPE_BITS)) | type )

/* mimic plain c-string */
typedef char *mstr;

/* Flags that can be set on mstring to indicate for attached metadata. It is
 * */
typedef uint16_t mstrFlags;

// LSB (Least Significant Bit): 最低有效位
// MSB (Most Significant Bit): 最高有效位
struct __attribute__ ((__packed__)) mstrhdr5 {
    unsigned char info; /* 2 lsb of type, 1 metadata, and 5 msb of string length */
    char buf[];
};
struct __attribute__ ((__packed__)) mstrhdr8 {
    uint8_t unused;  /* To achieve odd size header (See comment above) */
    uint8_t len;
    unsigned char info; /* 2 lsb of type, 6 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) mstrhdr16 {
    uint16_t len;
    unsigned char info; /* 2 lsb of type, 6 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) mstrhdr64 {
    uint64_t len;
    unsigned char info; /* 2 lsb of type, 6 unused bits */
    char buf[];
};
// 16
#define NUM_MSTR_FLAGS (sizeof(mstrFlags)*8)

/* mstrKind is used to define a kind (a group) of mstring with its own metadata layout */
 typedef struct mstrKind {
    const char *name;
    int metaSize[NUM_MSTR_FLAGS];
} mstrKind;
// sizeof(mstrKind) = 72
// name 8 字节 metasize 64 字节

mstr mstrNew(const char *initStr, size_t lenStr, int trymalloc);

mstr mstrNewWithMeta(struct mstrKind *kind, const char *initStr, size_t lenStr, mstrFlags flags, int trymalloc);

mstr mstrNewCopy(struct mstrKind *kind, mstr src, mstrFlags newFlags);

void *mstrGetAllocPtr(struct mstrKind *kind, mstr str);

void mstrFree(struct mstrKind *kind, mstr s);

mstrFlags *mstrFlagsRef(mstr s);

void *mstrMetaRef(mstr s, struct mstrKind *kind, int flagIdx);

size_t mstrlen(const mstr s);

/* return non-zero if metadata is attached to mstring   */
//  4 = 0100 ,倒数第3位为 isMeta
static inline int mstrIsMetaAttached(mstr s) { return s[-1] & MSTR_META_MASK; }

/* return whether if a specific flag-index is set */
static inline int mstrGetFlag(mstr s, int flagIdx) { return *mstrFlagsRef(s) & (1 << flagIdx); }

/* DEBUG */
void mstrPrint(mstr s, struct mstrKind *kind, int verbose);

/* See comment above about MSTR-ALIGNMENT(2) */
static_assert(sizeof(struct mstrhdr5 ) % 2 == 1, "must be odd");
static_assert(sizeof(struct mstrhdr8 ) % 2 == 1, "must be odd");
static_assert(sizeof(struct mstrhdr16 ) % 2 == 1, "must be odd");
static_assert(sizeof(struct mstrhdr64 ) % 2 == 1, "must be odd");
static_assert(sizeof(mstrFlags ) % 2 == 0, "must be even to keep mstr pointer odd");

#ifdef REDIS_TEST
int mstrTest(int argc, char *argv[], int flags);
#endif

#endif

/*
 * Index-based KV store implementation
 * This file implements a KV store comprised of an array of dicts (see dict.c)
 * The purpose of this KV store is to have easy access to all keys that belong
 * in the same dict (i.e. are in the same dict-index)
 *
 * For example, when Redis is running in cluster mode, we use kvstore to save
 * all keys that map to the same hash-slot in a separate dict within the kvstore
 * struct.
 * This enables us to easily access all keys that map to a specific hash-slot.
 *
 * Copyright (c) 2011-Present, Redis Ltd. and contributors.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

#include "fmacros.h"

#include <string.h>
#include <stddef.h>

#include "zmalloc.h"
#include "kvstore.h"
#include "redisassert.h"
#include "monotonic.h"

#define UNUSED(V) ((void) V)

struct _kvstore {
    int flags;  /* 控制是否立即创建字典、删除字典的行为、元数据类型  */
    dictType dtype; /* 字典创建、rehashing、hashing 的函数 */
    dict **dicts;
    long long num_dicts;
    long long num_dicts_bits;
    list *rehashing;                       /* List of dictionaries in this kvstore that are currently rehashing. */
    int resize_cursor;                     /* Cron job uses this cursor to gradually resize dictionaries (only used if num_dicts > 1). */
    int allocated_dicts;                   /* The number of allocated dicts. */
    int non_empty_dicts;                   /* The number of non-empty dicts. */
    unsigned long long key_count;          /* Total number of keys in this kvstore. */
    unsigned long long bucket_count;       /* Total number of buckets in this kvstore across dictionaries. */
    /*
     * 二进制索引树（Binary Indexed Tree，也称为 Fenwick Tree）的实现，
     * 用于高效维护和查询键值存储中各个字典的累积键频率
     */
    unsigned long long *dict_size_index;   /* Binary indexed tree (BIT) that describes cumulative key frequencies up until given dict-index. */
    // lut 就是 lookup table
    // 哈希表的核心结构是一个桶数组，每个桶(bucket)存储具有相同哈希值的键值对
    size_t overhead_hashtable_lut;         /* The overhead of all dictionaries. */
    size_t overhead_hashtable_rehashing;   /* The overhead of dictionaries rehashing. */
    void *metadata[];                      /* conditionally allocated based on "flags" */
};

/* Structure for kvstore iterator that allows iterating across multiple dicts. */
struct _kvstoreIterator {
    kvstore *kvs;
    long long didx;
    long long next_didx;
    dictIterator di;
};

/* Structure for kvstore dict iterator that allows iterating the corresponding dict. */
struct _kvstoreDictIterator {
    kvstore *kvs;
    long long didx;
    dictIterator di;
};

/* Basic metadata allocated per dict
 * note: 与 kvstoreMetadata 不同
 */
typedef struct {
    listNode *rehashing_node;   /* list node in rehashing list */
} kvstoreDictMetaBase;


/* Conditionally metadata allocated per dict (specifically for keysizes histogram) */
typedef struct {
    kvstoreDictMetaBase base; /* must be first in struct ! */
    /* External metadata */
    kvstoreDictMetadata meta;
} kvstoreDictMetaEx;

/**********************************/
/*** Helpers **********************/
/**********************************/

/* Get the dictionary pointer based on dict-index. */
static dict *kvstoreGetDict(kvstore *kvs, int didx) {
    return kvs->dicts[didx];
}

static dict **kvstoreGetDictRef(kvstore *kvs, int didx) {
    return &kvs->dicts[didx];
}

static int kvstoreDictIsRehashingPaused(kvstore *kvs, int didx)
{
    dict *d = kvstoreGetDict(kvs, didx);
    return d ? dictIsRehashingPaused(d) : 0;
}

/* Returns total (cumulative) number of keys up until given dict-index (inclusive).
 * Time complexity is O(log(kvs->num_dicts)).
 * 返回从第一个字典到指定字典索引的累积键数量
 */
static unsigned long long cumulativeKeyCountRead(kvstore *kvs, int didx) {
    if (kvs->num_dicts == 1) {
        assert(didx == 0);
        return kvstoreSize(kvs);
    }

    //  二进制索引数的索引从1开始
    int idx = didx + 1;
    unsigned long long sum = 0;
    while (idx > 0) {
        sum += kvs->dict_size_index[idx];
        idx -= (idx & -idx);
    }
    return sum;
}

// 将字典索引编码到游标中
static void addDictIndexToCursor(kvstore *kvs, int didx, unsigned long long *cursor) {
    if (kvs->num_dicts == 1)
        return;
    /* didx can be -1 when iteration is over and there are no more dicts to visit. */
    if (didx < 0)
        return;
    *cursor = (*cursor << kvs->num_dicts_bits) | didx;
}

// addDictIndexToCursor 是将 didx 保存到 cursor
// getAndClearDictIndexFromCursor 是取出 cursor
static int getAndClearDictIndexFromCursor(kvstore *kvs, unsigned long long *cursor) {
    if (kvs->num_dicts == 1)
        return 0;
    int didx = (int) (*cursor & (kvs->num_dicts-1));
    *cursor = *cursor >> kvs->num_dicts_bits;
    return didx;
}

/* Updates binary index tree (also known as Fenwick tree), increasing key count for a given dict.
 * You can read more about this data structure here https://en.wikipedia.org/wiki/Fenwick_tree
 * Time complexity is O(log(kvs->num_dicts)).
 *
 * 更新操作
 * delta表示键数的变化量（正数表示增加，负数表示减少）
 */
static void cumulativeKeyCountAdd(kvstore *kvs, int didx, long delta) {
    kvs->key_count += delta;

    // 从 dict 数组中获取第 didx 个字典
    dict *d = kvstoreGetDict(kvs, didx);
    // 获取键的数量
    size_t dsize = dictSize(d);
    /* Increment if dsize is 1 and delta is positive (first element inserted, dict becomes non-empty).
     * Decrement if dsize is 0 (dict becomes empty).
     * 在调用这个函数之前,对字典进行修改,键的数量发生变化
     * 之前调用 add , dsize==1 ,delta>0 => 有一个非空的字典产生
     * 之前调用 del,  dsize==0 => 有一个非空的字典减少
     * 其余情况不改变非空字典数量
     */
    int non_empty_dicts_delta = (dsize == 1 && delta > 0) ? 1 : (dsize == 0) ? -1 : 0;
    kvs->non_empty_dicts += non_empty_dicts_delta;

    /* BIT does not need to be calculated when there's only one dict. */
    if (kvs->num_dicts == 1)
        return;

    /* Update the BIT */
    int idx = didx + 1; /* Unlike dict indices, BIT is 1-based, so we need to add 1. */
    while (idx <= kvs->num_dicts) {
        if (delta < 0) {
            /*
             * kvs->dict_size_index[idx] - 这是 BIT 中索引为 idx 的节点值，表示累积的键数量
             * labs(delta) - 这是 delta 的绝对值（因为 delta 是 long 类型）
             * 断言条件 - 确保当前节点的值大于等于要减少的数量
             */
            assert(kvs->dict_size_index[idx] >= (unsigned long long)labs(delta));
        }
        kvs->dict_size_index[idx] += delta;
        /*
         * 用于获取 idx 二进制表示的最右边的1所代表的数值
         * idx = 12 = b1100
         * -idx = -12 = b0100  补码表示, 取反+1
         * idx & -idx = b0100  = 4
         */
        idx += (idx & -idx);
    }
}

/* Create the dict if it does not exist and return it. */
static dict *createDictIfNeeded(kvstore *kvs, int didx) {
    dict *d = kvstoreGetDict(kvs, didx);
    if (d) return d;

    // 创建一个 dict
    kvs->dicts[didx] = dictCreate(&kvs->dtype);
    kvs->allocated_dicts++;
    return kvs->dicts[didx];
}

/* Called when the dict will delete entries, the function will check
 * KVSTORE_FREE_EMPTY_DICTS to determine whether the empty dict needs
 * to be freed.
 *
 * Note that for rehashing dicts, that is, in the case of safe iterators
 * and Scan, we won't delete the dict. We will check whether it needs
 * to be deleted when we're releasing the iterator.
 *
 * 1. 没有设置 KVSTORE_FREE_EMPTY_DICTS
 * 2. didx 位置的字典为空
 * 3. didx 位置的字典有值
 * 4. didx 位置的字典暂停 rehashing
 * 以上条件直接返回
 * */
static void freeDictIfNeeded(kvstore *kvs, int didx) {
    if (!(kvs->flags & KVSTORE_FREE_EMPTY_DICTS) ||
        !kvstoreGetDict(kvs, didx) ||
        kvstoreDictSize(kvs, didx) != 0 ||
        kvstoreDictIsRehashingPaused(kvs, didx))
        return;
    dictRelease(kvs->dicts[didx]);
    kvs->dicts[didx] = NULL;
    kvs->allocated_dicts--;
}

/**********************************/
/*** dict callbacks ***************/
/**********************************/

/* Adds dictionary to the rehashing list, which allows us
 * to quickly find rehash targets during incremental rehashing.
 *
 * If there are multiple dicts, updates the bucket count for the given dictionary
 * in a DB, bucket count incremented with the new ht size during the rehashing phase.
 * If there's one dict, bucket count can be retrieved directly from single dict bucket. */
static void kvstoreDictRehashingStarted(dict *d) {
    kvstore *kvs = d->type->userdata;
    kvstoreDictMetaBase *metadata = (kvstoreDictMetaBase *)dictMetadata(d);
    // 添加正在 rehashing 的 dict 到 kvs->rehashing
    listAddNodeTail(kvs->rehashing, d);
    //在字典元数据中保存指向链表节点的引用，便于后续快速删除
    metadata->rehashing_node = listLast(kvs->rehashing);

    unsigned long long from, to;
    // from 表0 的 bucket 大小
    // to 表1 的 bucket 大小
    dictRehashingInfo(d, &from, &to);
    // 更新 kvstore 的桶计数：增加新哈希表的大小
    kvs->bucket_count += to; /* Started rehashing (Add the new ht size) */
    kvs->overhead_hashtable_lut += to;
    kvs->overhead_hashtable_rehashing += from;
}

/* Remove dictionary from the rehashing list.
 *
 * Updates the bucket count for the given dictionary in a DB. It removes
 * the old ht size of the dictionary from the total sum of buckets for a DB.  */
static void kvstoreDictRehashingCompleted(dict *d) {
    kvstore *kvs = d->type->userdata;
    kvstoreDictMetaBase *metadata = (kvstoreDictMetaBase *)dictMetadata(d);
    if (metadata->rehashing_node) {
        // 删除链表中指定的节点
        listDelNode(kvs->rehashing, metadata->rehashing_node);
        metadata->rehashing_node = NULL;
    }

    unsigned long long from, to;
    dictRehashingInfo(d, &from, &to);
    kvs->bucket_count -= from; /* Finished rehashing (Remove the old ht size) */
    kvs->overhead_hashtable_lut -= from;
    kvs->overhead_hashtable_rehashing -= from;
}

/* Returns the size of the DB dict base metadata in bytes. */
static size_t kvstoreDictMetaBaseSize(dict *d) {
    UNUSED(d);
    return sizeof(kvstoreDictMetaBase);
}
/* Returns the size of the DB dict extended metadata in bytes. */
static size_t kvstoreDictMetadataExtendSize(dict *d) {
    UNUSED(d);
    return sizeof(kvstoreDictMetaEx);
}

/**********************************/
/*** API **************************/
/**********************************/

/* Create an array of dictionaries
 * num_dicts_bits is the log2 of the amount of dictionaries needed (e.g. 0 for 1 dict,
 * 3 for 8 dicts, etc.) */
kvstore *kvstoreCreate(dictType *type, int num_dicts_bits, int flags) {
    /* We can't support more than 2^16 dicts because we want to save 48 bits
     * for the dict cursor, see kvstoreScan */
    assert(num_dicts_bits <= 16);

    /* Calc kvstore size */   
    size_t kvsize = sizeof(kvstore);
    /* Conditionally calc also histogram size */
    if (flags & KVSTORE_ALLOC_META_KEYS_HIST) 
        kvsize += sizeof(kvstoreMetadata);

    // 为 kvstore 分配内存
    kvstore *kvs = zcalloc(kvsize);
    memcpy(&kvs->dtype, type, sizeof(kvs->dtype));
    kvs->flags = flags;

    /* kvstore must be the one to set these callbacks, so we make sure the
     * caller didn't do it
     * kvstore 必须拥有设置这些回调函数的控制权, 确保调用者没有预先设置这些回调函数
     */
    assert(!type->userdata);
    assert(!type->dictMetadataBytes);
    assert(!type->rehashingStarted);
    assert(!type->rehashingCompleted);
    // dict 的 dtype 初始化
    kvs->dtype.userdata = kvs;
    // 设置元数据的大小
    if (flags & KVSTORE_ALLOC_META_KEYS_HIST)
        kvs->dtype.dictMetadataBytes = kvstoreDictMetadataExtendSize;
    else
        kvs->dtype.dictMetadataBytes = kvstoreDictMetaBaseSize;
    // rehashing 开始时执行调用函数
    kvs->dtype.rehashingStarted = kvstoreDictRehashingStarted;
    // rehashing 执行完成后调用函数
    kvs->dtype.rehashingCompleted = kvstoreDictRehashingCompleted;

    kvs->num_dicts_bits = num_dicts_bits;
    kvs->num_dicts = 1 << kvs->num_dicts_bits;
    kvs->dicts = zcalloc(sizeof(dict*) * kvs->num_dicts);
    // 如果没有设置 KVSTORE_ALLOCATE_DICTS_ON_DEMAND 立即创建所有字典
    // 设置了就 按需创建
    if (!(kvs->flags & KVSTORE_ALLOCATE_DICTS_ON_DEMAND)) {
        // 创建 num_dicts 个 dict
        for (int i = 0; i < kvs->num_dicts; i++)
            createDictIfNeeded(kvs, i);
    }

    kvs->rehashing = listCreate();
    kvs->key_count = 0;
    kvs->non_empty_dicts = 0;
    kvs->resize_cursor = 0;
    // 如果 kvs->num_dicts > 1, 创建一个 num_dicts+1 长度的 unsigned long long 型数组
    // 用于创建 Fenwick tree, 记录每个 dict 的数量
    kvs->dict_size_index = kvs->num_dicts > 1? zcalloc(sizeof(unsigned long long) * (kvs->num_dicts + 1)) : NULL;
    kvs->bucket_count = 0;
    kvs->overhead_hashtable_lut = 0;
    kvs->overhead_hashtable_rehashing = 0;
    return kvs;
}

void kvstoreEmpty(kvstore *kvs, void(callback)(dict*)) {
    for (int didx = 0; didx < kvs->num_dicts; didx++) {
        dict *d = kvstoreGetDict(kvs, didx);
        if (!d)
            continue;
        kvstoreDictMetaBase *metadata = (kvstoreDictMetaBase *)dictMetadata(d);
        if (metadata->rehashing_node)
            metadata->rehashing_node = NULL;
        if (kvs->flags & KVSTORE_ALLOC_META_KEYS_HIST) {
            kvstoreDictMetaEx *metaExt = (kvstoreDictMetaEx *) metadata;
            memset(&metaExt->meta.keysizes_hist, 0, sizeof(metaExt->meta.keysizes_hist));
        }
        // 清空 dict
        dictEmpty(d, callback);
        freeDictIfNeeded(kvs, didx);
    }

    if (kvs->flags & KVSTORE_ALLOC_META_KEYS_HIST)
        memset(kvstoreGetMetadata(kvs), 0, sizeof(kvstoreMetadata));

    listEmpty(kvs->rehashing);

    kvs->key_count = 0;
    kvs->non_empty_dicts = 0;
    kvs->resize_cursor = 0;
    kvs->bucket_count = 0;
    if (kvs->dict_size_index)
        memset(kvs->dict_size_index, 0, sizeof(unsigned long long) * (kvs->num_dicts + 1));
    kvs->overhead_hashtable_lut = 0;
    kvs->overhead_hashtable_rehashing = 0;
}

void kvstoreRelease(kvstore *kvs) {
    for (int didx = 0; didx < kvs->num_dicts; didx++) {
        dict *d = kvstoreGetDict(kvs, didx);
        if (!d)
            continue;
        kvstoreDictMetaBase *metadata = (kvstoreDictMetaBase *)dictMetadata(d);
        if (metadata->rehashing_node)
            metadata->rehashing_node = NULL;
        dictRelease(d);
    }
    zfree(kvs->dicts);

    listRelease(kvs->rehashing);
    if (kvs->dict_size_index)
        zfree(kvs->dict_size_index);

    zfree(kvs);
}

// NOTE: 这里是否可以添加 likely 来优化
// 返回字典键的数量
unsigned long long int kvstoreSize(kvstore *kvs) {
    if (kvs->num_dicts != 1) {
        return kvs->key_count;
    } else {
        // 字典数量为1时, 计算第 0 个字典的键数量
        return kvs->dicts[0]? dictSize(kvs->dicts[0]) : 0;
    }
}

/* This method provides the cumulative sum of all the dictionary buckets
 * across dictionaries in a database. */
unsigned long kvstoreBuckets(kvstore *kvs) {
    if (kvs->num_dicts != 1) {
        return kvs->bucket_count;
    } else {
        return kvs->dicts[0]? dictBuckets(kvs->dicts[0]) : 0;
    }
}

size_t kvstoreMemUsage(kvstore *kvs) {
    size_t mem = sizeof(*kvs);
    size_t metaSize = sizeof(kvstoreDictMetaBase);

    if (kvs->flags & KVSTORE_ALLOC_META_KEYS_HIST)
        metaSize = sizeof(kvstoreDictMetaEx);
    
    unsigned long long keys_count = kvstoreSize(kvs);
    // dictEntryMemUsage() 只统计了 dictEntry 结构体本身的大小
    mem += keys_count * dictEntryMemUsage() +
           kvstoreBuckets(kvs) * sizeof(dictEntry*) +
           kvs->allocated_dicts * (sizeof(dict) + metaSize);

    /* Values are dict* shared with kvs->dicts */
    mem += listLength(kvs->rehashing) * sizeof(listNode);

    if (kvs->dict_size_index)
        mem += sizeof(unsigned long long) * (kvs->num_dicts + 1);

    return mem;
}

/*
 * This method is used to iterate over the elements of the entire kvstore specifically across dicts.
 * It's a three pronged approach. 三个方面的措施
 *
 * 1. It uses the provided cursor `cursor` to retrieve the dict index from it.
 *  note: 这里的 dictScanValidFunction 应该是 kvstoreScanShouldSkipDict
 * 2. If the dictionary is in a valid state checked through the provided callback dictScanValidFunction``,
 *    it performs a dictScan over the appropriate `keyType` dictionary of `db`.
 * 3. If the dict is entirely scanned i.e. the cursor has reached 0, the next non empty dict is discovered.
 *    The dict information is embedded into the cursor and returned.
 *
 * 1. 使用提供的游标(cursor)来检索字典索引
 * 2.
 * 3. 如果 dict 遍历后, cursor 变为0 ,会获取下一个非空字典, 该字典的信息会嵌入游标并返回
 *
 * To restrict the scan to a single dict, pass a valid dict index as
 * 'onlydidx', otherwise pass -1.
 */
unsigned long long kvstoreScan(kvstore *kvs, unsigned long long cursor,
                               int onlydidx, dictScanFunction *scan_cb,
                               kvstoreScanShouldSkipDict *skip_cb,
                               void *privdata)
{
    unsigned long long _cursor = 0;
    /* During dictionary traversal, 48 upper bits in the cursor are used for positioning in the HT.
     * Following lower bits are used for the dict index number, ranging from 0 to 2^num_dicts_bits-1.
     * Dict index is always 0 at the start of iteration and can be incremented only if there are
     * multiple dicts.
     *
     * 游标被设计为包含两个部分：
     *   - 高位部分(48位): 用于哈希表内部定位
     *   - 低位部分: 用于存储字典索引信息
     * 当 onlydidx >= 0 时，表示只扫描指定索引的字典，而不扫描其他字典。
     */
    int didx = getAndClearDictIndexFromCursor(kvs, &cursor);
    if (onlydidx >= 0) {
        // 如果当前字典索引小于目标索引，会快速跳转到指定字典
        if (didx < onlydidx) {
            /* Fast-forward to onlydidx. */
            assert(onlydidx < kvs->num_dicts);
            didx = onlydidx;
            // 重置游标 cursor = 0，从该字典的开始位置扫描
            cursor = 0;
        } else if (didx > onlydidx) {
            /* The cursor is already past onlydidx. */
            return 0;
        }
    }

    dict *d = kvstoreGetDict(kvs, didx);

    /*
     * 确定是否应该跳过当前字典的扫描，有两种情况会跳过：
     * 1.!d: 字典指针为空，表示该字典不存在
     * 2.(skip_cb && skip_cb(d)): 提供了跳过回调函数且回调函数返回真值
     * 如 skip_cb= isExpiryDictValidForSamplingCb 时
     */
    int skip = !d || (skip_cb && skip_cb(d));
    if (!skip) {
        _cursor = dictScan(d, cursor, scan_cb, privdata);
        /* In dictScan, scan_cb may delete entries (e.g., in active expire case). */
        freeDictIfNeeded(kvs, didx);
    }
    /* scanning done for the current dictionary or if the scanning wasn't possible, move to the next dict index.
     * _cursor == 0 时说明当前字典遍历结束
     */
    if (_cursor == 0 || skip) {
        if (onlydidx >= 0)
            return 0;
        // 获取下一个非空字典的索引
        didx = kvstoreGetNextNonEmptyDictIndex(kvs, didx);
    }
    if (didx == -1) {
        return 0;
    }
    // _cursor !=0, 说明 didx 指向的字典还没有遍历结束
    // 但游标 _cursor 已经发生变化,下次遍历时用这个游标处理
    // 这里对游标和索引进行编码操作
    addDictIndexToCursor(kvs, didx, &_cursor);
    return _cursor;
}

/*
 * This functions increases size of kvstore to match desired number.
 * It resizes all individual dictionaries, unless skip_cb indicates otherwise.
 *
 * Based on the parameter `try_expand`, appropriate dict expand API is invoked.
 * if try_expand is set to 1, `dictTryExpand` is used else `dictExpand`.
 * The return code is either `DICT_OK`/`DICT_ERR` for both the API(s).
 * `DICT_OK` response is for successful expansion. However, `DICT_ERR` response signifies failure in allocation in
 * `dictTryExpand` call and in case of `dictExpand` call it signifies no expansion was performed.
 *
 * try_expand = 1: 使用 dictTryExpand
 *   尝试扩展字典，如果内存不足则返回失败
 *   不会阻塞，适用于对响应时间敏感的场景
 *  try_expand = 0: 使用 dictExpand
 *   强制扩展字典，可能会阻塞直到成功
 *   适用于后台操作或可以接受延迟的场景
 */
int kvstoreExpand(kvstore *kvs, uint64_t newsize, int try_expand, kvstoreExpandShouldSkipDictIndex *skip_cb) {
    for (int i = 0; i < kvs->num_dicts; i++) {
        dict *d = kvstoreGetDict(kvs, i);
        // skip_cb = dbExpandSkipSlot 判断指定的字典是否适合扩容
        // 如果字典不存在或者回调函数指示跳过该字典，则继续处理下一个字典。
        if (!d || (skip_cb && skip_cb(i)))
            continue;
        int result = try_expand ? dictTryExpand(d, newsize) : dictExpand(d, newsize);
        if (try_expand && result == DICT_ERR)
            return 0;
    }

    return 1;
}

/* Returns fair random dict index, probability of each dict being returned is proportional to the number of elements that dictionary holds.
 * This function guarantees that it returns a dict-index of a non-empty dict, unless the entire kvstore is empty.
 * Time complexity of this function is O(log(kvs->num_dicts)). */
int kvstoreGetFairRandomDictIndex(kvstore *kvs) {
    // 随机获取一个索引值
    unsigned long target = kvstoreSize(kvs) ? (randomULong() % kvstoreSize(kvs)) + 1 : 0;
    // 返回这个随机值所处字典的索引
    return kvstoreFindDictIndexByKeyIndex(kvs, target);
}

// 将 kvstore 下所有字典的统计信息写到 buf 中
void kvstoreGetStats(kvstore *kvs, char *buf, size_t bufsize, int full) {
    buf[0] = '\0';

    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;
    // 主 hash 表的统计信息
    dictStats *mainHtStats = NULL;
    // 1号 hash 表统计信息
    dictStats *rehashHtStats = NULL;
    dict *d;
    kvstoreIterator *kvs_it = kvstoreIteratorInit(kvs);
    // 遍历每个字典获取统计信息
    while ((d = kvstoreIteratorNextDict(kvs_it))) {
        // 获取指定 hash 表的统计信息
        dictStats *stats = dictGetStatsHt(d, 0, full);
        if (!mainHtStats) {
            mainHtStats = stats;
        } else {
            dictCombineStats(stats, mainHtStats);
            dictFreeStats(stats);
        }
        if (dictIsRehashing(d)) {
            // 如果正在 rehashing , 统计 1 号 hash 表的信息
             stats = dictGetStatsHt(d, 1, full);
            if (!rehashHtStats) {
                rehashHtStats = stats;
            } else {
                dictCombineStats(stats, rehashHtStats);
                dictFreeStats(stats);
            }
        }
    }
    kvstoreIteratorRelease(kvs_it);

    if (mainHtStats && bufsize > 0) {
        // 将统计信息写到 buf 中
        l = dictGetStatsMsg(buf, bufsize, mainHtStats, full);
        dictFreeStats(mainHtStats);
        buf += l;
        bufsize -= l;
    }

    if (rehashHtStats && bufsize > 0) {
        l = dictGetStatsMsg(buf, bufsize, rehashHtStats, full);
        dictFreeStats(rehashHtStats);
        buf += l;
        bufsize -= l;
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize - 1] = '\0';
}

/* Finds a dict containing target element in a key space ordered by dict index.
 * Consider this example. Dictionaries are represented by brackets and keys by dots:
 *  #0   #1   #2     #3    #4
 * [..][....][...][.......][.]
 *                    ^
 *                 target
 *
 * In this case dict #3 contains key that we are trying to find.
 *
 * The return value is 0 based dict-index, and the range of the target is [1..kvstoreSize], kvstoreSize inclusive.
 *
 * To find the dict, we start with the root node of the binary index tree and search through its children
 * from the highest index (2^num_dicts_bits in our case) to the lowest index. At each node, we check if the target
 * value is greater than the node's value. If it is, we remove the node's value from the target and recursively
 * search for the new target using the current node as the parent.
 * Time complexity of this function is O(log(kvs->num_dicts))
 *
 * 返回值是字典数组的索引(第几个字典)
 */
int kvstoreFindDictIndexByKeyIndex(kvstore *kvs, unsigned long target) {
    if (kvs->num_dicts == 1 || kvstoreSize(kvs) == 0)
        return 0;
    assert(target <= kvstoreSize(kvs));

    // 假设 num_dicts_bits = 4, target=255
    // 1000 -> 0100 -> 0010 -> 0001
    // [1(100)][2(200)][4(300)][8(800)]
    //      [1(100)] 表示1号节点的累积key 值为100
    //      [2(200)] 表示2号节点的累积key 值为200
    // 节点的二进制表示只有1个1时, 该节点存的累积值
    // 第一轮 i= 0b1000 target=255 result=0 current=8
    //           (255 > 800) = false
    // 第二轮 i=0b0100 target=255 result=0 current=4
    //           (255 > 300) = false
    // 第三轮 i=0b0010 target=255 result=0 current=2
    //           (255>200) = true => target=55, result = 2
    // 第四轮 i=0b0001 target=55 result=2 current=3
    //          如果 [3(60)]
    //              (55>60) = false 结束循环 最终 result=2
    //          如果 [3(40)]
    //              (55>40) = true
    //                target=15, result =3 结束循环 最终 result=3
    // 上面的例子中, 截止2号节点, key 数量为 200
    //    - 如果3号节点的键值数量为 60, 说明3号节点包含要查找的key, 应该是要返回3 的,
    //      但索引树是 1-base 的数组, 返回 result=2 刚好是 0-base 数组的索引
    //    - 如果3号节点的键值数量为 40, 说明4号节点包含要查找的key, 应该是要返回4 的,
    //      但索引树是 1-base 的数组, 返回 result=3 刚好是 0-base 数组的索引
    int result = 0, bit_mask = 1 << kvs->num_dicts_bits;
    for (int i = bit_mask; i != 0; i >>= 1) {
        int current = result + i;
        /* When the target index is greater than 'current' node value then we will update
         * the target and search in the 'current' node tree. */
        if (target > kvs->dict_size_index[current]) {
            target -= kvs->dict_size_index[current];
            result = current;
        }
    }
    /* Adjust the result to get the correct dict:
     * 1. result += 1;
     *    After the calculations, the index of target in dict_size_index should be the next one,
     *    so we should add 1.
     * 2. result -= 1;
     *    Unlike BIT(dict_size_index is 1-based), dict indices are 0-based, so we need to subtract 1.
     * As the addition and subtraction cancel each other out, we can simply return the result. */
    return result;
}

/* Wrapper for kvstoreFindDictIndexByKeyIndex to get the first non-empty dict index in the kvstore. */
int kvstoreGetFirstNonEmptyDictIndex(kvstore *kvs) {
    return kvstoreFindDictIndexByKeyIndex(kvs, 1);
}

/* Returns next non-empty dict index strictly after given one, or -1 if provided didx is the last one. */
int kvstoreGetNextNonEmptyDictIndex(kvstore *kvs, int didx) {
    if (kvs->num_dicts == 1) {
        assert(didx == 0);
        return -1;
    }
    // 获取前 didx 个字典的累积键的数量
    unsigned long long next_key = cumulativeKeyCountRead(kvs, didx) + 1;
    return next_key <= kvstoreSize(kvs) ? kvstoreFindDictIndexByKeyIndex(kvs, next_key) : -1;
}

int kvstoreNumNonEmptyDicts(kvstore *kvs) {
    return kvs->non_empty_dicts;
}

int kvstoreNumAllocatedDicts(kvstore *kvs) {
    return kvs->allocated_dicts;
}

int kvstoreNumDicts(kvstore *kvs) {
    return kvs->num_dicts;
}

/* Returns kvstore iterator that can be used to iterate through sub-dictionaries.
 *
 * The caller should free the resulting kvs_it with kvstoreIteratorRelease. */
kvstoreIterator *kvstoreIteratorInit(kvstore *kvs) {
    kvstoreIterator *kvs_it = zmalloc(sizeof(*kvs_it));
    kvs_it->kvs = kvs;
    kvs_it->didx = -1;
    kvs_it->next_didx = kvstoreGetFirstNonEmptyDictIndex(kvs_it->kvs); /* Finds first non-empty dict index. */
    dictInitSafeIterator(&kvs_it->di, NULL);
    return kvs_it;
}

/* Free the kvs_it returned by kvstoreIteratorInit. */
void kvstoreIteratorRelease(kvstoreIterator *kvs_it) {
    dictIterator *iter = &kvs_it->di;
    dictResetIterator(iter);
    /* In the safe iterator context, we may delete entries. */
    freeDictIfNeeded(kvs_it->kvs, kvs_it->didx);
    zfree(kvs_it);
}


/* Returns next dictionary from the iterator, or NULL if iteration is complete.
 *
 * - Takes care to reset the iter of the previous dict before moved to the next dict.
 */
dict *kvstoreIteratorNextDict(kvstoreIterator *kvs_it) {
    if (kvs_it->next_didx == -1)
        return NULL;

    /* The dict may be deleted during the iteration process, so here need to check for NULL. */
    if (kvs_it->didx != -1 && kvstoreGetDict(kvs_it->kvs, kvs_it->didx)) {
        /* Before we move to the next dict, reset the iter of the previous dict. */
        dictIterator *iter = &kvs_it->di;
        dictResetIterator(iter);
        /* In the safe iterator context, we may delete entries. */
        freeDictIfNeeded(kvs_it->kvs, kvs_it->didx);
    }

    kvs_it->didx = kvs_it->next_didx;
    // 获得下一个非空字典的索引
    kvs_it->next_didx = kvstoreGetNextNonEmptyDictIndex(kvs_it->kvs, kvs_it->didx);
    return kvs_it->kvs->dicts[kvs_it->didx];
}

// 根据 kvstoreIterator 迭代器获取当前字典的索引
int kvstoreIteratorGetCurrentDictIndex(kvstoreIterator *kvs_it) {
    assert(kvs_it->didx >= 0 && kvs_it->didx < kvs_it->kvs->num_dicts);
    return kvs_it->didx;
}

/* Returns next entry. */
dictEntry *kvstoreIteratorNext(kvstoreIterator *kvs_it) {
    dictEntry *de = kvs_it->di.d ? dictNext(&kvs_it->di) : NULL;
    // de 为空是才执行 if 语句
    if (!de) { /* No current dict or reached the end of the dictionary. */

        /* Before we move to the next dict, function kvstoreIteratorNextDict()
         * reset the iter of the previous dict & freeDictIfNeeded(). */
        dict *d = kvstoreIteratorNextDict(kvs_it);

        if (!d)
            return NULL;

        dictInitSafeIterator(&kvs_it->di, d);
        de = dictNext(&kvs_it->di);
    }
    return de;
}

/* This method traverses through kvstore dictionaries and triggers a resize.
 * It first tries to shrink if needed, and if it isn't, it tries to expand. */
void kvstoreTryResizeDicts(kvstore *kvs, int limit) {
    if (limit > kvs->num_dicts)
        limit = kvs->num_dicts;

    for (int i = 0; i < limit; i++) {
        int didx = kvs->resize_cursor;
        dict *d = kvstoreGetDict(kvs, didx);
        if (d && dictShrinkIfNeeded(d) == DICT_ERR) {
            dictExpandIfNeeded(d);
        }
        kvs->resize_cursor = (didx + 1) % kvs->num_dicts;
    }
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use threshold_us
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns the amount of microsecs spent if some rehashing was
 * performed, otherwise 0 is returned. */
uint64_t kvstoreIncrementallyRehash(kvstore *kvs, uint64_t threshold_us) {
    if (listLength(kvs->rehashing) == 0)
        return 0;

    /* Our goal is to rehash as many dictionaries as we can before reaching threshold_us,
     * after each dictionary completes rehashing, it removes itself from the list.
     *
     * 在 threshold_us 内尽可能多的进行 rehash
     * 字典完成重哈希后会自动从重哈希列表中移除(kvstoreDictRehashingCompleted)
     */
    listNode *node;
    monotime timer;
    uint64_t elapsed_us = 0;
    elapsedStart(&timer);
    // kvs->rehashing 就是 adlist,存储所有正在进行 rehashing 的 dict
    // 这里取出头节点, node->value 是 dict
    // 当字典完成重哈希后，会自动调用回调函数 kvstoreDictRehashingCompleted，
    // 从重哈希列表中移除该字典。因此下次循环时 listFirst 会返回下一个重哈希字典
    while ((node = listFirst(kvs->rehashing))) {
        dictRehashMicroseconds(listNodeValue(node), threshold_us - elapsed_us);

        elapsed_us = elapsedUs(timer);
        if (elapsed_us >= threshold_us) {
            break;  /* Reached the time limit. */
        }
    }
    return elapsed_us;
}

size_t kvstoreOverheadHashtableLut(kvstore *kvs) {
    return kvs->overhead_hashtable_lut * sizeof(dictEntry *);
}

size_t kvstoreOverheadHashtableRehashing(kvstore *kvs) {
    return kvs->overhead_hashtable_rehashing * sizeof(dictEntry *);
}

unsigned long kvstoreDictRehashingCount(kvstore *kvs) {
    return listLength(kvs->rehashing);
}

unsigned long kvstoreDictSize(kvstore *kvs, int didx)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return 0;
    return dictSize(d);
}

kvstoreDictIterator *kvstoreGetDictIterator(kvstore *kvs, int didx)
{
    kvstoreDictIterator *kvs_di = zmalloc(sizeof(*kvs_di));
    kvs_di->kvs = kvs;
    kvs_di->didx = didx;
    dictInitIterator(&kvs_di->di, kvstoreGetDict(kvs, didx));
    return kvs_di;
}

kvstoreDictIterator *kvstoreGetDictSafeIterator(kvstore *kvs, int didx)
{
    kvstoreDictIterator *kvs_di = zmalloc(sizeof(*kvs_di));
    kvs_di->kvs = kvs;
    kvs_di->didx = didx;
    dictInitSafeIterator(&kvs_di->di, kvstoreGetDict(kvs, didx));
    return kvs_di;
}

/* Free the kvs_di returned by kvstoreGetDictIterator and kvstoreGetDictSafeIterator. */
void kvstoreReleaseDictIterator(kvstoreDictIterator *kvs_di)
{
    /* The dict may be deleted during the iteration process, so here need to check for NULL. */
    if (kvstoreGetDict(kvs_di->kvs, kvs_di->didx)) {
        dictResetIterator(&kvs_di->di);
        /* In the safe iterator context, we may delete entries. */
        freeDictIfNeeded(kvs_di->kvs, kvs_di->didx);
    }

    zfree(kvs_di);
}

/* Get the next element of the dict through kvstoreDictIterator and dictNext. */
dictEntry *kvstoreDictIteratorNext(kvstoreDictIterator *kvs_di)
{
    /* The dict may be deleted during the iteration process, so here need to check for NULL. */
    dict *d = kvstoreGetDict(kvs_di->kvs, kvs_di->didx);
    if (!d) return NULL;

    return dictNext(&kvs_di->di);
}

dictEntry *kvstoreDictGetRandomKey(kvstore *kvs, int didx)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictGetRandomKey(d);
}

dictEntry *kvstoreDictGetFairRandomKey(kvstore *kvs, int didx)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictGetFairRandomKey(d);
}

dictEntry *kvstoreDictFindByHashAndPtr(kvstore *kvs, int didx, const void *oldptr, uint64_t hash)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictFindByHashAndPtr(d, oldptr, hash);
}

unsigned int kvstoreDictGetSomeKeys(kvstore *kvs, int didx, dictEntry **des, unsigned int count)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return 0;
    return dictGetSomeKeys(d, des, count);
}

int kvstoreDictExpand(kvstore *kvs, int didx, unsigned long size)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return DICT_ERR;
    return dictExpand(d, size);
}

unsigned long kvstoreDictScanDefrag(kvstore *kvs, int didx, unsigned long v, dictScanFunction *fn, dictDefragFunctions *defragfns, void *privdata)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return 0;
    return dictScanDefrag(d, v, fn, defragfns, privdata);
}

/* Unlike kvstoreDictScanDefrag(), this method doesn't defrag the data(keys and values)
 * within dict, it only reallocates the memory used by the dict structure itself using 
 * the provided allocation function. This feature was added for the active defrag feature.
 *
 * With 16k dictionaries for cluster mode with 1 shard, this operation may require substantial time
 * to execute.  A "cursor" is used to perform the operation iteratively.  When first called, a
 * cursor value of 0 should be provided.  The return value is an updated cursor which should be
 * provided on the next iteration.  The operation is complete when 0 is returned.
 *
 * The 'defragfn' callback is called with a reference to the dict that callback can reallocate.
 *
 * cursor 用来进行迭代操作, 第一次调用时初始化为0
 * 函数的返回值会更新 cursor, 作为下一次调用的参数
 */
unsigned long kvstoreDictLUTDefrag(kvstore *kvs, unsigned long cursor, kvstoreDictLUTDefragFunction *defragfn) {
    for (int didx = cursor; didx < kvs->num_dicts; didx++) {
        dict **d = kvstoreGetDictRef(kvs, didx), *newd;
        if (!*d)
            continue;
        if ((newd = defragfn(*d))) {
            *d = newd;

            /* After defragmenting the dict, update its corresponding
             * rehashing node in the kvstore's rehashing list. */
            kvstoreDictMetaBase *metadata = (kvstoreDictMetaBase *)dictMetadata(*d);
            if (metadata->rehashing_node)
                metadata->rehashing_node->value = *d;
        }
        return (didx + 1);
    }
    return 0;
}

uint64_t kvstoreGetHash(kvstore *kvs, const void *key)
{
    return kvs->dtype.hashFunction(key);
}

void *kvstoreDictFetchValue(kvstore *kvs, int didx, const void *key)
{
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictFetchValue(d, key);
}

dictEntry *kvstoreDictFind(kvstore *kvs, int didx, void *key) {
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictFind(d, key);
}


// 向指定的 dict 添加 key
// 并更新 key 数量的统计信息
dictEntry *kvstoreDictAddRaw(kvstore *kvs, int didx, void *key, dictEntry **existing) {
    // 创建对应索引的字典, 依据 kvs->dtype
    dict *d = createDictIfNeeded(kvs, didx);
    dictEntry *ret = dictAddRaw(d, key, existing);
    if (ret)
        cumulativeKeyCountAdd(kvs, didx, 1);
    return ret;
}

void kvstoreDictSetKey(kvstore *kvs, int didx, dictEntry* de, void *key) {
    dict *d = kvstoreGetDict(kvs, didx);
    dictSetKey(d, de, key);
}

void kvstoreDictSetVal(kvstore *kvs, int didx, dictEntry *de, void *val) {
    dict *d = kvstoreGetDict(kvs, didx);
    dictSetVal(d, de, val);
}

dictEntry *kvstoreDictTwoPhaseUnlinkFind(kvstore *kvs, int didx, const void *key, dictEntry ***plink, int *table_index) {
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return NULL;
    return dictTwoPhaseUnlinkFind(kvstoreGetDict(kvs, didx), key, plink, table_index);
}

void kvstoreDictTwoPhaseUnlinkFree(kvstore *kvs, int didx, dictEntry *he, dictEntry **plink, int table_index) {
    dict *d = kvstoreGetDict(kvs, didx);
    dictTwoPhaseUnlinkFree(d, he, plink, table_index);
    cumulativeKeyCountAdd(kvs, didx, -1);
    freeDictIfNeeded(kvs, didx);
}

int kvstoreDictDelete(kvstore *kvs, int didx, const void *key) {
    dict *d = kvstoreGetDict(kvs, didx);
    if (!d)
        return DICT_ERR;
    int ret = dictDelete(d, key);
    if (ret == DICT_OK) {
        cumulativeKeyCountAdd(kvs, didx, -1);
        freeDictIfNeeded(kvs, didx);
    }
    return ret;
}

kvstoreDictMetadata *kvstoreGetDictMetadata(kvstore *kvs, int didx) {
    dict *d = kvstoreGetDict(kvs, didx);
    if ((!d) || (!(kvs->flags & KVSTORE_ALLOC_META_KEYS_HIST)))
        return NULL;
    
    kvstoreDictMetaEx *metadata = (kvstoreDictMetaEx *)dictMetadata(d);
    return &(metadata->meta);
}

kvstoreMetadata *kvstoreGetMetadata(kvstore *kvs) {
    return (kvstoreMetadata *) &kvs->metadata;
}

#ifdef REDIS_TEST
#include <stdio.h>
#include "testhelp.h"

#define TEST(name) printf("test — %s\n", name);

uint64_t hashTestCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

void freeTestCallback(dict *d, void *val) {
    UNUSED(d);
    zfree(val);
}

void *defragAllocTest(void *ptr) {
    size_t size = zmalloc_usable_size(ptr);
    void *newptr = zmalloc(size);
    memcpy(newptr, ptr, size);
    zfree(ptr);
    return newptr;
}

dict *defragLUTTestCallback(dict *d) {
    /* handle the dict struct */
    d = defragAllocTest(d);
    /* handle the first hash table */
    d->ht_table[0] = defragAllocTest(d->ht_table[0]);
    /* handle the second hash table */
    if (d->ht_table[1])
        d->ht_table[1] = defragAllocTest(d->ht_table[1]);
    return d; 
}

dictType KvstoreDictTestType = {
    hashTestCallback,
    NULL,
    NULL,
    NULL,
    freeTestCallback,
    NULL,
    NULL
};

char *stringFromInt(int value) {
    char buf[32];
    int len;
    char *s;

    len = snprintf(buf, sizeof(buf), "%d",value);
    s = zmalloc(len+1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

/* ./redis-server test kvstore */
int kvstoreTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int i;
    void *key;
    dictEntry *de;
    kvstoreIterator *kvs_it;
    kvstoreDictIterator *kvs_di;

    int didx = 0;
    int curr_slot = 0;
    kvstore *kvs1 = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
    kvstore *kvs2 = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND | KVSTORE_FREE_EMPTY_DICTS);

    TEST("Add 16 keys") {
        for (i = 0; i < 16; i++) {
            de = kvstoreDictAddRaw(kvs1, didx, stringFromInt(i), NULL);
            assert(de != NULL);
            de = kvstoreDictAddRaw(kvs2, didx, stringFromInt(i), NULL);
            assert(de != NULL);
        }
        assert(kvstoreDictSize(kvs1, didx) == 16);
        assert(kvstoreSize(kvs1) == 16);
        assert(kvstoreDictSize(kvs2, didx) == 16);
        assert(kvstoreSize(kvs2) == 16);
    }

    TEST("kvstoreIterator case 1: removing all keys does not delete the empty dict") {
        kvs_it = kvstoreIteratorInit(kvs1);
        // 这里返回的是一个字典实例
        while((de = kvstoreIteratorNext(kvs_it)) != NULL) {
            curr_slot = kvstoreIteratorGetCurrentDictIndex(kvs_it);
            key = dictGetKey(de);
            assert(kvstoreDictDelete(kvs1, curr_slot, key) == DICT_OK);
        }
        kvstoreIteratorRelease(kvs_it);

        dict *d = kvstoreGetDict(kvs1, didx);
        assert(d != NULL);
        assert(kvstoreDictSize(kvs1, didx) == 0);
        assert(kvstoreSize(kvs1) == 0);
    }

    TEST("kvstoreIterator case 2: removing all keys will delete the empty dict") {
        kvs_it = kvstoreIteratorInit(kvs2);
        while((de = kvstoreIteratorNext(kvs_it)) != NULL) {
            curr_slot = kvstoreIteratorGetCurrentDictIndex(kvs_it);
            key = dictGetKey(de);
            assert(kvstoreDictDelete(kvs2, curr_slot, key) == DICT_OK);
        }
        kvstoreIteratorRelease(kvs_it);

        /* Make sure the dict was removed from the rehashing list. */
        while (kvstoreIncrementallyRehash(kvs2, 1000)) {}

        dict *d = kvstoreGetDict(kvs2, didx);
        assert(d == NULL);
        assert(kvstoreDictSize(kvs2, didx) == 0);
        assert(kvstoreSize(kvs2) == 0);
    }

    TEST("Add 16 keys again") {
        for (i = 0; i < 16; i++) {
            de = kvstoreDictAddRaw(kvs1, didx, stringFromInt(i), NULL);
            assert(de != NULL);
            de = kvstoreDictAddRaw(kvs2, didx, stringFromInt(i), NULL);
            assert(de != NULL);
        }
        assert(kvstoreDictSize(kvs1, didx) == 16);
        assert(kvstoreSize(kvs1) == 16);
        assert(kvstoreDictSize(kvs2, didx) == 16);
        assert(kvstoreSize(kvs2) == 16);
    }

    TEST("kvstoreDictIterator case 1: removing all keys does not delete the empty dict") {
        // 获取指定索引字典的迭代器
        kvs_di = kvstoreGetDictSafeIterator(kvs1, didx);
        // 针对单个字典进行迭代
        while((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
            key = dictGetKey(de);
            assert(kvstoreDictDelete(kvs1, didx, key) == DICT_OK);
        }
        kvstoreReleaseDictIterator(kvs_di);

        dict *d = kvstoreGetDict(kvs1, didx);
        assert(d != NULL);
        assert(kvstoreDictSize(kvs1, didx) == 0);
        assert(kvstoreSize(kvs1) == 0);
    }

    TEST("kvstoreDictIterator case 2: removing all keys will delete the empty dict") {
        kvs_di = kvstoreGetDictSafeIterator(kvs2, didx);
        while((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
            key = dictGetKey(de);
            assert(kvstoreDictDelete(kvs2, didx, key) == DICT_OK);
        }
        kvstoreReleaseDictIterator(kvs_di);

        dict *d = kvstoreGetDict(kvs2, didx);
        assert(d == NULL);
        assert(kvstoreDictSize(kvs2, didx) == 0);
        assert(kvstoreSize(kvs2) == 0);
    }

    TEST("Verify that a rehashing dict's node in the rehashing list is correctly updated after defragmentation") {
        unsigned long cursor = 0;
        kvstore *kvs = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
        for (i = 0; i < 256; i++) {
            de = kvstoreDictAddRaw(kvs, 0, stringFromInt(i), NULL);
            //进行 rehashing 的字典数量大于1时就跳出
            if (listLength(kvs->rehashing)) break;
        }
        assert(listLength(kvs->rehashing));
        while ((cursor = kvstoreDictLUTDefrag(kvs, cursor, defragLUTTestCallback)) != 0) {}
        while (kvstoreIncrementallyRehash(kvs, 1000)) {}
        kvstoreRelease(kvs);
    }

    TEST("Verify non-empty dict count is correctly updated") {
        kvstore *kvs = kvstoreCreate(&KvstoreDictTestType, 2, 
                            KVSTORE_ALLOCATE_DICTS_ON_DEMAND | KVSTORE_ALLOC_META_KEYS_HIST);
        for (int idx = 0; idx < 4; idx++) {
            for (i = 0; i < 16; i++) {
                de = kvstoreDictAddRaw(kvs, idx, stringFromInt(i), NULL);
                assert(de != NULL);
                /* When the first element is inserted, the number of non-empty dictionaries is increased by 1. */
                if (i == 0) assert(kvstoreNumNonEmptyDicts(kvs) == idx + 1);
            }
        }

        /* Step by step, clear all dictionaries and ensure non-empty dict count is updated */
        for (int idx = 0; idx < 4; idx++) {
            kvs_di = kvstoreGetDictSafeIterator(kvs, idx);
            while((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
                key = dictGetKey(de);
                assert(kvstoreDictDelete(kvs, idx, key) == DICT_OK);
                /* When the dictionary is emptied, the number of non-empty dictionaries is reduced by 1. */
                if (kvstoreDictSize(kvs, idx) == 0) assert(kvstoreNumNonEmptyDicts(kvs) == 3 - idx);
            }
            kvstoreReleaseDictIterator(kvs_di);
        }
        kvstoreRelease(kvs);
    }

    kvstoreRelease(kvs1);
    kvstoreRelease(kvs2);
    return 0;
}
#endif

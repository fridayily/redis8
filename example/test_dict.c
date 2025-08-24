#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "dict.h"
#include "monotonic.h"

// 添加 _serverAssert 函数定义
void _serverAssert(const char *filename, int linenum, const char *condition) {
    printf("Assertion failed at %s:%d: %s\n", filename, linenum, condition);
    abort();
}

// 测试用的简单哈希函数
uint64_t testHashFunction(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

// 测试用的键比较函数
int testKeyCompare(dict *d, const void *key1, const void *key2) {
    return strcmp((char*)key1, (char*)key2) == 0;
}

// 测试用的键复制函数
void *testKeyDup(dict *d, const void *key) {
    return strdup((char*)key);
}

// 测试用的值复制函数
void *testValDup(dict *d, const void *obj) {
    return strdup((char*)obj);
}

// 测试用的键析构函数
void testKeyDestructor(dict *d, void *key) {
    free(key);
}

// 测试用的值析构函数
void testValDestructor(dict *d, void *obj) {
    free(obj);
}

// 字典类型定义
dictType testDictType = {
    .hashFunction = testHashFunction,
    .keyDup = testKeyDup,
    .valDup = testValDup,
    .keyCompare = testKeyCompare,
    .keyDestructor = NULL,
    .valDestructor = NULL,
    .no_value = 0,
    .keys_are_odd = 0,
    .force_full_rehash = 0,
    .storedHashFunction = NULL,
    .storedKeyCompare = NULL,
    .onDictRelease = NULL,
    .keyLen = NULL,
    .keyCompareWithLen = NULL,
    .resizeAllowed = NULL,
    .rehashingStarted = NULL,
    .rehashingCompleted = NULL,
    .dictMetadataBytes = NULL,
    .userdata = NULL
};

/* 用于收集扫描结果的数据结构 */
typedef struct {
    char **keys;
    int count;
    int capacity;
} ScanResult;

/* 扫描回调函数 */
void scanCallback(void *privdata, const dictEntry *de) {
    ScanResult *result = (ScanResult*)privdata;

    if (result->count >= result->capacity) {
        result->capacity *= 2;
        result->keys = realloc(result->keys, result->capacity * sizeof(char*));
    }

    char *key = (char*)dictGetKey(de);
    result->keys[result->count] = strdup(key);
    result->count++;
}

// 测试辅助函数
void assert_true(int condition, const char *msg) {
    if (!condition) {
        fprintf(stderr, "Assertion failed: %s\n", msg);
        exit(1);
    }
}

void assert_equal_int(long long expected, long long actual, const char *msg) {
    if (expected != actual) {
        fprintf(stderr, "Assertion failed: %s. Expected: %lld, Actual: %lld\n", 
                msg, expected, actual);
        exit(1);
    }
}

void assert_equal_ptr(void *expected, void *actual, const char *msg) {
    if (expected != actual) {
        fprintf(stderr, "Assertion failed: %s. Expected: %p, Actual: %p\n", 
                msg, expected, actual);
        exit(1);
    }
}

void assert_str_equal(const char *expected, const char *actual, const char *msg) {
    if (strcmp(expected, actual) != 0) {
        fprintf(stderr, "Assertion failed: %s. Expected: '%s', Actual: '%s'\n", 
                msg, expected, actual);
        exit(1);
    }
}

// 使用一级指针交换两个整数值
void swap_values_with_pointers(int *a, int *b) {
    int temp = *a;
    *a = *b;
    *b = temp;
}

void swap_char_values(char *ptr1,char *ptr2)
{
    char temp = *ptr1;
    *ptr1 = *ptr2;
    *ptr2 = temp;
}

// 使用二级指针交换两个指针
void swap_char_pointers(char **ptr1, char **ptr2) {
    char *temp = *ptr1;
    *ptr1 = *ptr2;
    *ptr2 = temp;
}

void test_switch_point()
{
    int x = 10, y = 20;
    printf("Before swap_values_with_pointers: x = %d, y = %d\n", x, y);
    swap_values_with_pointers(&x, &y);
    printf("After swap_values_with_pointers: x = %d, y = %d\n", x, y);

    char a = 'A';
    char b = 'B';
    printf("Before swap_char_values: a = %d, b = %d\n", a, b);
    swap_char_values(&a, &b);
    printf("After swap_char_values: a = %d, b = %d\n", a, b);
    printf("\n");

    char *first = "First String";
    char *second = "Second String";

    printf("Before swap: first = %s second = %s\n", first,second);
    swap_char_pointers(&first, &second);
    printf("After swap: first = %s second = %s\n", first,second);

    printf("\n");
}



// 测试1: 基本创建和释放
void test_dict_create_and_release() {
    printf("Testing dict create and release...\n");
    
    dict *d = dictCreate(&testDictType);
    assert_true(d != NULL, "Dict should be created successfully");
    assert_equal_int(0, dictSize(d), "New dict should be empty");
    assert_true(dictIsEmpty(d), "New dict should be empty");
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试2: 添加和查找元素
void test_dict_add_and_find() {
    printf("Testing dict add and find...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 添加元素
    int result = dictAdd(d, "key1", "value1");
    assert_equal_int(DICT_OK, result, "Adding first element should succeed");
    assert_equal_int(1, dictSize(d), "Dict size should be 1");
    
    result = dictAdd(d, "key2", "value2");
    assert_equal_int(DICT_OK, result, "Adding second element should succeed");
    assert_equal_int(2, dictSize(d), "Dict size should be 2");
    
    // 查找元素
    void *value = dictFetchValue(d, "key1");
    assert_str_equal("value1", (char*)value, "Should find value for key1");
    
    value = dictFetchValue(d, "key2");
    assert_str_equal("value2", (char*)value, "Should find value for key2");
    
    // 查找不存在的键
    value = dictFetchValue(d, "nonexistent");
    assert_true(value == NULL, "Should return NULL for nonexistent key");
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试3: 重复键处理
void test_dict_duplicate_keys() {
    printf("Testing dict duplicate keys...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 添加第一个键值对
    int result = dictAdd(d, "key", "value1");
    assert_equal_int(DICT_OK, result, "First add should succeed");
    
    // 尝试添加相同键
    result = dictAdd(d, "key", "value2");
    assert_equal_int(DICT_ERR, result, "Adding duplicate key should fail");
    
    // 验证值没有改变
    void *value = dictFetchValue(d, "key");
    assert_str_equal("value1", (char*)value, "Value should remain unchanged");
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试4: 替换元素
void test_dict_replace() {
    printf("Testing dict replace...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 添加元素
    int result = dictAdd(d, "key", "old_value");
    assert_equal_int(DICT_OK, result, "Adding element should succeed");
    
    // 替换元素
    result = dictReplace(d, "key", "new_value");
    assert_equal_int(0, result, "Replace should indicate existing key was replaced");
    
    // 验证值已更新
    void *value = dictFetchValue(d, "key");
    assert_str_equal("new_value", (char*)value, "Value should be updated");
    
    // 替换不存在的键
    result = dictReplace(d, "new_key", "new_value2");
    assert_equal_int(1, result, "Replace should indicate new key was added");
    
    value = dictFetchValue(d, "new_key");
    assert_str_equal("new_value2", (char*)value, "New key should have correct value");
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试5: 删除元素
void test_dict_delete() {
    printf("Testing dict delete...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 添加元素
    dictAdd(d, "key1", "value1");
    dictAdd(d, "key2", "value2");
    dictAdd(d, "key3", "value3");
    assert_equal_int(3, dictSize(d), "Dict should have 3 elements");

    // 删除元素
    int result = dictDelete(d, "key2");
    assert_equal_int(DICT_OK, result, "Deleting existing key should succeed");
    assert_equal_int(2, dictSize(d), "Dict size should be 2 after deletion");
    
    // 验证元素已被删除
    void *value = dictFetchValue(d, "key2");
    assert_true(value == NULL, "Deleted key should not be found");
    
    // 删除不存在的键
    result = dictDelete(d, "nonexistent");
    assert_equal_int(DICT_ERR, result, "Deleting nonexistent key should fail");
    
    // 删除剩余元素
    dictDelete(d, "key1");
    dictDelete(d, "key3");
    assert_true(dictIsEmpty(d), "Dict should be empty after all deletions");
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试6: 字典扩展和收缩
void test_dict_expand_shrink() {
    printf("Testing dict expand and shrink...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 初始状态
    unsigned long initial_buckets = dictBuckets(d);
    assert_equal_int(0, initial_buckets, "Initial bucket count should match");
    
    // 扩展字典
    int result = dictExpand(d, 100);
    assert_equal_int(DICT_OK, result, "Expanding dict should succeed");
    
    // 添加大量元素触发自动扩展
    for (int i = 0; i < 100; i++) {
        char key[32], value[32];
        sprintf(key, "key_%d", i);
        sprintf(value, "value_%d", i);
        dictAdd(d, strdup(key), strdup(value));
    }
    
    assert_equal_int(100, dictSize(d), "Dict should have 100 elements");
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试7: 迭代器
void test_dict_iterator() {
    printf("Testing dict iterator...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 添加测试数据
    dictAdd(d, "apple", "fruit");
    dictAdd(d, "carrot", "vegetable");
    dictAdd(d, "banana", "fruit");
    dictAdd(d, "broccoli", "vegetable");
    
    // 使用迭代器遍历
    dictIterator *iter = dictGetIterator(d);
    dictEntry *entry;
    int count = 0;
    
    while ((entry = dictNext(iter)) != NULL) {
        const char *key = (const char*)dictGetKey(entry);
        const char *value = (const char*)dictGetVal(entry);
        assert_true(key != NULL, "Key should not be NULL");
        assert_true(value != NULL, "Value should not be NULL");
        count++;
    }
    
    assert_equal_int(4, count, "Iterator should visit all 4 elements");
    dictReleaseIterator(iter);
    
    // 测试安全迭代器
    iter = dictGetSafeIterator(d);
    count = 0;
    
    while ((entry = dictNext(iter)) != NULL) {
        count++;
        // 在安全迭代器中可以修改字典
        if (count == 2) {
            // 可以在这里进行删除等操作
        }
    }
    
    assert_equal_int(4, count, "Safe iterator should also visit all elements");
    dictReleaseIterator(iter);
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试8: 随机键获取
void test_dict_random_key() {
    printf("Testing dict random key...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 添加测试数据
    dictAdd(d, "key1", "value1");
    dictAdd(d, "key2", "value2");
    dictAdd(d, "key3", "value3");
    
    // 获取随机键
    dictEntry *entry = dictGetRandomKey(d);
    assert_true(entry != NULL, "Should get a random entry");
    
    const char *key = (const char*)dictGetKey(entry);
    const char *value = (const char*)dictGetVal(entry);
    assert_true(key != NULL, "Random key should not be NULL");
    assert_true(value != NULL, "Random value should not be NULL");
    
    // 获取公平随机键
    entry = dictGetFairRandomKey(d);
    assert_true(entry != NULL, "Should get a fair random entry");
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试9: 整数值得操作
void test_dict_integer_values() {
    printf("Testing dict integer values...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 添加整数类型的值
    dictEntry *entry = dictAddRaw(d, "counter", NULL);
    dictSetSignedIntegerVal(entry, 42);
    
    // 验证值
    char *key1 = strdup("counter");
    entry = dictFind(d, key1);
    assert_true(entry != NULL, "Should find the entry");
    assert_equal_int(42, dictGetSignedIntegerVal(entry), "Should get correct integer value");
    
    // 增加值
    dictIncrSignedIntegerVal(entry, 8);
    assert_equal_int(50, dictGetSignedIntegerVal(entry), "Should get incremented value");
    
    // 测试无符号整数
    char *key2 = strdup("unsigned_counter");
    entry = dictAddRaw(d, key2, NULL);
    dictSetUnsignedIntegerVal(entry, 1000ULL);
    assert_equal_int(1000, dictGetUnsignedIntegerVal(entry), "Should get correct unsigned value");
    
    dictIncrUnsignedIntegerVal(entry, 500ULL);
    assert_equal_int(1500, dictGetUnsignedIntegerVal(entry), "Should get incremented unsigned value");
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试10: 浮点值操作
void test_dict_double_values() {
    printf("Testing dict double values...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 添加浮点类型的值
    dictEntry *entry = dictAddRaw(d, "score", NULL);
    dictSetDoubleVal(entry, 3.14159);
    
    // 验证值
    entry = dictFind(d, "score");
    assert_true(entry != NULL, "Should find the entry");
    
    double value = dictGetDoubleVal(entry);
    assert_true(value > 3.14158 && value < 3.14160, "Should get correct double value");
    
    // 增加值
    dictIncrDoubleVal(entry, 0.1);
    value = dictGetDoubleVal(entry);
    assert_true(value > 3.24158 && value < 3.24160, "Should get incremented double value");
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试11: Rehash功能
void test_dict_rehash() {
    printf("Testing dict rehash...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 添加足够多的元素触发rehash
    for (int i = 0; i < 1000; i++) {
        char key[32], value[32];
        sprintf(key, "key_%d", i);
        sprintf(value, "value_%d", i);
        dictAdd(d, strdup(key), strdup(value));
    }
    
    // 执行一些rehash步骤
    int steps = dictRehash(d, 100);
    assert_true(steps >= 0, "Rehash should return non-negative steps");
    
    // 执行微秒级rehash
    const char *monotonic_info = monotonicInit();
    printf("Monotonic clock initialized: %s\n", monotonic_info);
    steps = dictRehashMicroseconds(d, 1000); // 1ms
    assert_true(steps >= 0, "Microsecond rehash should return non-negative steps");
    
    assert_equal_int(1000, dictSize(d), "Dict should still have 1000 elements");
    
    dictRelease(d);
    printf("PASSED\n");
}

// 测试12: 统计信息
void test_dict_stats() {
    printf("Testing dict stats...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 添加测试数据
    for (int i = 0; i < 50; i++) {
        char key[32], value[32];
        sprintf(key, "key_%d", i);
        sprintf(value, "value_%d", i);
        dictAdd(d, strdup(key), strdup(value));
    }
    
    // 获取统计信息
    char buf[1024];
    dictGetStats(buf, sizeof(buf), d, 1);
    printf("dict stats %s\n",buf);
    assert_true(strlen(buf) > 0, "Stats buffer should not be empty");
    
    // 获取哈希表统计
    dictStats *stats = dictGetStatsHt(d, 0, 1);
    assert_true(stats != NULL, "Should get stats for ht 0");
    assert_true(stats->htUsed > 0, "Used buckets should be greater than 0");
    
    dictFreeStats(stats);
    dictRelease(d);
    printf("PASSED\n");
}

// 测试13: 特殊功能标志
void test_dict_flags() {
    printf("Testing dict flags...\n");
    
    dict *d = dictCreate(&testDictType);
    
    // 测试暂停和恢复rehash
    dictPauseRehashing(d);
    assert_true(dictIsRehashingPaused(d), "Rehashing should be paused");
    
    dictResumeRehashing(d);
    assert_true(!dictIsRehashingPaused(d), "Rehashing should be resumed");
    
    // 测试暂停和恢复自动调整大小
    dictPauseAutoResize(d);
    assert_true(d->pauseAutoResize > 0, "Auto resize should be paused");
    
    dictResumeAutoResize(d);
    assert_true(d->pauseAutoResize == 0, "Auto resize should be resumed");
    
    dictRelease(d);
    printf("PASSED\n");
}


static unsigned long simple_rev(unsigned long v) {
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0UL;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

unsigned long update_cursor(unsigned long v, unsigned long mask) {
    v |= ~mask;
    v = simple_rev(v);  // 假设4位
    v++;
    v = simple_rev(v);
    return v;
}
// 打印二进制形式的函数
void print_binary(unsigned long value, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
        printf("%lu", (value >> i) & 1);
        if (i % 4 == 0 && i > 0) printf(" "); // 每4位添加一个空格便于阅读
    }
}

void test_update_cursor()
{

    unsigned long cursor = 0;
    unsigned long mask = 7;  // 对应大小为8的表 (111)

    printf("mask 111 迭代过程:\n");
    for (int i = 0; i < 8; i++) {
        unsigned long bucket = cursor & mask;
        printf("游标: %04lu bin: ", cursor);
        print_binary(cursor,8);
        printf(" bucket: %lu\n",bucket);
        cursor = update_cursor(cursor, mask);
        if (cursor == 0) break;  // 完成一轮迭代
    }

    cursor = 0;
    mask = 15;  // 对应大小为16的表 (1111)

    printf("mask 1111 迭代过程:\n");
    for (int i = 0; i < 16; i++) {
        unsigned long bucket = cursor & mask;
        printf("游标: %04lu bin: ", cursor);
        print_binary(cursor,8);
        printf(" bucket: %lu\n",bucket);
        cursor = update_cursor(cursor, mask);
        if (cursor == 0) break;  // 完成一轮迭代
    }
}

void test_dict_scan_basic() {
    printf("测试 dictScan 基本功能...\n");

    dict *d = dictCreate(&testDictType);

    /* 添加一些测试数据 */
    dictAdd(d, "key1", "value1");
    dictAdd(d, "key2", "value2");
    dictAdd(d, "key3", "value3");
    dictAdd(d, "key4", "value4");
    dictAdd(d, "key5", "value5");

    /* 准备扫描结果收集器 */
    ScanResult result = {0};
    result.capacity = 10;
    result.keys = malloc(result.capacity * sizeof(char*));

    /* 执行扫描 */
    unsigned long cursor = 0;
    int totalScanned = 0;
    do {
        cursor = dictScan(d, cursor, scanCallback, &result);
        totalScanned += result.count;
    } while (cursor != 0);

    /* 验证结果 */
    assert(result.count == 5);
    printf("扫描到 %d 个条目\n", result.count);

    /* 清理资源 */
    for (int i = 0; i < result.count; i++) {
        free(result.keys[i]);
    }
    free(result.keys);
    dictRelease(d);

    printf("测试 dictScan 基本功能通过\n\n");
}

/* 空字典扫描 */
void test_dict_scan_empty() {
    printf("测试 dictScan 空字典...\n");

    dict *d = dictCreate(&testDictType);

    ScanResult result = {0};
    result.capacity = 10;
    result.keys = malloc(result.capacity * sizeof(char*));

    /* 扫描空字典 */
    unsigned long cursor = dictScan(d, 0, scanCallback, &result);

    /* 验证结果 */
    assert(cursor == 0);
    assert(result.count == 0);
    printf("空字典扫描返回 cursor = %lu, 扫描到 %d 个条目\n", cursor, result.count);

    /* 清理资源 */
    free(result.keys);
    dictRelease(d);

    printf("测试 dictScan 空字典通过\n\n");
}

/* 大数据量扫描 */
void test_dict_scan_large() {
    printf("测试 dictScan 大数据量...\n");

    dict *d = dictCreate(&testDictType);

    /* 添加大量数据 */
    const int numEntries = 1000;
    for (int i = 0; i < numEntries; i++) {
        char key[32], value[32];
        snprintf(key, sizeof(key), "key_%d", i);
        snprintf(value, sizeof(value), "value_%d", i);
        dictAdd(d, strdup(key), strdup(value));
    }

    /* 准备扫描结果收集器 */
    ScanResult result = {0};
    result.capacity = numEntries;
    result.keys = malloc(result.capacity * sizeof(char*));

    /* 执行扫描并统计 */
    unsigned long cursor = 0;
    int scanCalls = 0;
    int totalEntries = 0;
    do {
        int beforeCount = result.count;
        cursor = dictScan(d, cursor, scanCallback, &result);
        int scannedInThisCall = result.count - beforeCount;
        scanCalls++;
        totalEntries = result.count;
        printf("第 %d 次扫描，cursor = %lu，本次扫描到 %d 个条目\n",
               scanCalls, cursor, scannedInThisCall);
    } while (cursor != 0);

    /* 验证结果 */
    assert(totalEntries == numEntries);
    printf("总共扫描 %d 次，获得 %d 个条目\n", scanCalls, totalEntries);

    /* 验证没有重复条目 */
    for (int i = 0; i < result.count; i++) {
        for (int j = i + 1; j < result.count; j++) {
            if (strcmp(result.keys[i], result.keys[j]) == 0) {
                printf("警告：发现重复条目 %s\n", result.keys[i]);
            }
        }
    }

    /* 清理资源 */
    for (int i = 0; i < result.count; i++) {
        free(result.keys[i]);
    }
    free(result.keys);
    dictRelease(d);

    printf("测试 dictScan 大数据量通过\n\n");
}

/* rehashing期间扫描 */
void test_dict_scan_during_rehash() {
    printf("测试 dictScan 在rehashing期间...\n");

    dict *d = dictCreate(&testDictType);

    /* 添加一些数据 */
    for (int i = 0; i < 50; i++) {
        char key[32], value[32];
        snprintf(key, sizeof(key), "key_%d", i);
        snprintf(value, sizeof(value), "value_%d", i);
        dictAdd(d, strdup(key), strdup(value));
    }

    /* 触发rehash */
    dictExpand(d, 128);
    assert(dictIsRehashing(d));
    printf("字典正在rehash中...\n");

    /* 准备扫描结果收集器 */
    ScanResult result = {0};
    result.capacity = 100;
    result.keys = malloc(result.capacity * sizeof(char*));

    /* 执行扫描 */
    unsigned long cursor = 0;
    int totalEntries = 0;
    do {
        int beforeCount = result.count;
        cursor = dictScan(d, cursor, scanCallback, &result);
        int scannedInThisCall = result.count - beforeCount;
        totalEntries = result.count;
        printf("cursor = %lu，本次扫描到 %d 个条目\n", cursor, scannedInThisCall);
    } while (cursor != 0);

    /* 验证结果 */
    assert(totalEntries == 50);
    printf("rehashing期间扫描到 %d 个条目\n", totalEntries);

    /* 清理资源 */
    for (int i = 0; i < result.count; i++) {
        free(result.keys[i]);
    }
    free(result.keys);
    dictRelease(d);

    printf("测试 dictScan 在rehashing期间通过\n\n");
}

// 主测试函数
int main() {
    printf("Starting Redis dict unit tests...\n\n");
    //
    // test_dict_create_and_release();
    // test_dict_add_and_find();
    // test_dict_duplicate_keys();
    // test_dict_replace();
    // test_dict_delete();
    // test_dict_expand_shrink();
    // test_dict_iterator();
    // test_dict_random_key();
    // test_dict_integer_values();
    // test_dict_double_values();
    // test_dict_rehash();
    // test_dict_stats();
    // test_dict_flags();
    test_update_cursor();
    // test_dict_scan_basic();
    // test_dict_scan_empty();
    // test_dict_scan_large();
    // test_dict_scan_during_rehash();
    
    printf("\nAll tests passed!\n");
    return 0;
}
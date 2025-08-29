#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 定义 UNUSED 和 TEST_ASSERT 宏
#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

#define TEST_ASSERT(condition) do { \
    if (!(condition)) { \
        printf("Test failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while(0)

// 添加 _serverAssert 函数定义
void _serverAssert(const char *filename, int linenum, const char *condition) {
    printf("Assertion failed at %s:%d: %s\n", filename, linenum, condition);
    abort();
}

#include "../src/sds.h"
// #include "test_help.h"


int test_sds_new_and_free(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    // Test sdsnew
    sds s = sdsnew("hello");
    TEST_ASSERT(strcmp(s, "hello") == 0);
    TEST_ASSERT(sdslen(s) == 5);

    // Test sdsfree
    sdsfree(s);
    
    // Test sdsnewlen
    s = sdsnewlen("hello\0world", 11);
    TEST_ASSERT(sdslen(s) == 11);
    TEST_ASSERT(memcmp(s, "hello\0world", 11) == 0);
    sdsfree(s);

    return 0;
}

int test_sds_grow_shrink(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    sds s = sdsnew("hello");
    
    // Test sdsMakeRoomFor
    size_t old_capacity = sdsavail(s);
    printf("Old capacity: %zu\n", old_capacity);
    s = sdsMakeRoomFor(s, 100);
    TEST_ASSERT(sdsavail(s) >= 100);
    TEST_ASSERT(strcmp(s, "hello") == 0);
    
    // Test sdsgrowzero
    size_t old_len = sdslen(s);
    s = sdsgrowzero(s, 20);
    TEST_ASSERT(sdslen(s) == 20);
    TEST_ASSERT(strncmp(s, "hello", old_len) == 0);
    
    // Test sdstrim
    sds s2 = sdsnew("xyhelloxy");
    s2 = sdstrim(s2, "xy");
    TEST_ASSERT(strcmp(s2, "hello") == 0);
    sdsfree(s2);
    
    sdsfree(s);
    return 0;
}

int test_sds_append(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    sds s = sdsnew("hello");
    
    // Test sdscat
    s = sdscat(s, " world");
    TEST_ASSERT(strcmp(s, "hello world") == 0);
    TEST_ASSERT(sdslen(s) == 11);
    
    // Test sdscatlen
    s = sdscatlen(s, "!", 1);
    TEST_ASSERT(strcmp(s, "hello world!") == 0);
    TEST_ASSERT(sdslen(s) == 12);
    
    // Test sdscpy
    s = sdscpy(s, "new string");
    TEST_ASSERT(strcmp(s, "new string") == 0);
    TEST_ASSERT(sdslen(s) == 10);
    
    sdsfree(s);
    return 0;
}

int test_sds_format(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    // Test sdscatprintf
    sds s = sdsempty();
    s = sdscatprintf(s, "Hello %s! You have %d messages.", "Alice", 5);
    TEST_ASSERT(strcmp(s, "Hello Alice! You have 5 messages.") == 0);
    
    // Test sdscatfmt (simple version)
    sdsfree(s);
    s = sdsempty();
    s = sdscatfmt(s, "Value: %i, String: %s", 42, "test");
    TEST_ASSERT(strcmp(s, "Value: 42, String: test") == 0);
    
    sdsfree(s);
    return 0;
}

int test_sds_misc_operations(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    // Test sdssplitlen
    sds s = sdsnew("hello,world,redis");
    int count;
    sds *tokens = sdssplitlen(s, sdslen(s), ",", 1, &count);
    TEST_ASSERT(count == 3);
    TEST_ASSERT(strcmp(tokens[0], "hello") == 0);
    TEST_ASSERT(strcmp(tokens[1], "world") == 0);
    TEST_ASSERT(strcmp(tokens[2], "redis") == 0);
    
    sdsfreesplitres(tokens, count);
    sdsfree(s);
    
    // Test sdsjoin
    sds parts[3];
    parts[0] = sdsnew("hello");
    parts[1] = sdsnew("world");
    parts[2] = sdsnew("redis");
    
    sds joined = sdsjoin(parts, 3, "-");
    TEST_ASSERT(strcmp(joined, "hello-world-redis") == 0);
    
    sdsfree(parts[0]);
    sdsfree(parts[1]);
    sdsfree(parts[2]);
    sdsfree(joined);
    
    return 0;
}

int test_sds_range_operations(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    sds s = sdsnew("hello world");
    
    // Test sdsrange
    sdsrange(s, 6, -1);
    TEST_ASSERT(strcmp(s, "world") == 0);
    
    sdsfree(s);
    s = sdsnew("hello world");
    sdsrange(s, 0, 4);
    TEST_ASSERT(strcmp(s, "hello") == 0);
    
    sdsfree(s);
    return 0;
}

int test_sds_buffer_access(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    sds s = sdsnew("hello");
    
    // Test direct buffer access
    TEST_ASSERT(strlen(s) == sdslen(s));
    TEST_ASSERT(s[sdslen(s)] == '\0');
    
    // Test sdsdup
    sds copy = sdsdup(s);
    TEST_ASSERT(strcmp(copy, s) == 0);
    TEST_ASSERT(sdslen(copy) == sdslen(s));
    
    sdsfree(s);
    sdsfree(copy);
    return 0;
}

int test_sds_empty_and_clear(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    // Test sdsempty
    sds s = sdsempty();
    TEST_ASSERT(sdslen(s) == 0);
    TEST_ASSERT(strcmp(s, "") == 0);
    
    // Test sdsclear
    s = sdscat(s, "hello world");
    TEST_ASSERT(sdslen(s) == 11);
    sdsclear(s);
    TEST_ASSERT(sdslen(s) == 0);
    TEST_ASSERT(strcmp(s, "") == 0);
    
    sdsfree(s);
    return 0;
}

int test_sds_case_operations(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    // Test sdstoupper
    sds s = sdsnew("Hello World");
    sdstoupper(s);
    TEST_ASSERT(strcmp(s, "HELLO WORLD") == 0);
    
    // Test sdstolower
    sdstolower(s);
    TEST_ASSERT(strcmp(s, "hello world") == 0);
    
    sdsfree(s);
    return 0;
}

int test_sds_scan_operations(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    // Test sdssplitargs
    sds line = sdsnew("GET key \"hello world\"");
    int argc_new;
    sds *argv_new = sdssplitargs(line, &argc_new);
    TEST_ASSERT(argc_new == 3);
    TEST_ASSERT(strcmp(argv_new[0], "GET") == 0);
    TEST_ASSERT(strcmp(argv_new[1], "key") == 0);
    TEST_ASSERT(strcmp(argv_new[2], "hello world") == 0);
    
    sdsfreesplitres(argv_new, argc_new);
    sdsfree(line);
    return 0;
}

int sum(int count,...){
    va_list args;
    // 初始化 args，count 是最后一个固定参数
    va_start(args,count);
    int total = 0;
    for (int i=0;i<count;i++){
        // 获取下一个 int 类型参数
        int value = va_arg(args,int); 
        total +=value;
    }
    va_end(args);
    return total;
}

// 正确的 vsnprintf 使用方式
int my_vsnprintf(const char* format, ...) {
    char buffer[256];
    va_list args;
    
    // 初始化参数列表
    va_start(args, format);
    
    // 使用 vsnprintf 进行格式化
    int result = vsnprintf(buffer, sizeof(buffer), format, args);
    
    // 清理参数列表
    va_end(args);
    
    // 输出结果
    printf("Formatted string: %s\n", buffer);
    printf("Characters that would have been written: %d\n", result);
    
    return result;
}

void test_sum(){
    int result = sum(5,10,20,30,40,50);
    printf("Sum: %d\n",result);
}

void test_vsnprintf(){
    my_vsnprintf("Name: %s, Age: %d, Height: %.1f cm", "Alice", 25, 165.5);
}

// 主测试函数
int main(int argc, char *argv[]) {
    printf("Starting SDS tests...\n");

    test_sum();
    test_vsnprintf();
    
    test_sds_new_and_free(argc, argv);
    printf("test_sds_new_and_free passed\n");
    
    test_sds_grow_shrink(argc, argv);
    printf("test_sds_grow_shrink passed\n");
    
    test_sds_append(argc, argv);
    printf("test_sds_append passed\n");
    
    test_sds_format(argc, argv);
    printf("test_sds_format passed\n");
    
    test_sds_misc_operations(argc, argv);
    printf("test_sds_misc_operations passed\n");
    
    test_sds_range_operations(argc, argv);
    printf("test_sds_range_operations passed\n");
    
    test_sds_buffer_access(argc, argv);
    printf("test_sds_buffer_access passed\n");
    
    test_sds_empty_and_clear(argc, argv);
    printf("test_sds_empty_and_clear passed\n");
    
    test_sds_case_operations(argc, argv);
    printf("test_sds_case_operations passed\n");
    
    test_sds_scan_operations(argc, argv);
    printf("test_sds_scan_operations passed\n");
    
    printf("All SDS tests passed!\n");
    return 0;
}
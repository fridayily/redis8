#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "test_common.h"

#include "quicklist.h"
#include "zmalloc.h"

void _serverAssert(const char* filename, int linenum, const char* condition)
{
    printf("Assertion failed at %s:%d: %s\n", filename, linenum, condition);
    abort();
}


static void test_quicklist_new_and_release()
{
    quicklist* ql = quicklistNew(2, 3);
    quicklistRelease(ql);
}

static void test_quicklist_add_iterm()
{
    printf("Testing quicklist add iterm...\n");

    // 每个节点的 listpack 2个元素
    quicklist* ql = quicklistNew(2, 1);
    quicklistPushTail(ql, "a1", 2);
    quicklistPushHead(ql, "a2", 2);
    quicklistPushHead(ql, "a3", 2);
    quicklistPushHead(ql, "a4", 2);
    quicklistPushHead(ql, "a5", 2);
    // NOTE  quicklist 布局   [a5]<->[a4,a3]<->[a2,a1]
    // 插入 a5 时 触发压缩 [a4,a3], 但是 这个节点的长度小于 MIN_COMPRESS_BYTES ,不会压缩
    // 3 个节点
    assert_true(ql->len == 3, "ql len should 3");
    // 5 个元素
    assert_true(ql->count == 5, "ql count should 5");
    assert_equal_int(ql->head->count, 1, "ql head count should 1");
    assert_equal_int(ql->tail->count, 2, "ql tail count should 2");
    quicklistEntry entry;

    // 测试反向遍历 (从尾到头)
    quicklistIter* iter = quicklistGetIterator(ql, AL_START_TAIL);
    int index = 0;
    char* expected_values_reverse[] = {"a1", "a2", "a3", "a4", "a5"};
    while (quicklistNext(iter, &entry) != 0)
    {
        assert(strncmp((char*)entry.value, expected_values_reverse[index], 2) == 0);
        index++;
    }
    assert_true(index == 5, "should have iterated 5 times");
    quicklistReleaseIterator(iter);

    // 测试正向遍历 (从头到尾)
    iter = quicklistGetIterator(ql, AL_START_HEAD);
    while (quicklistNext(iter, &entry) != 0)
    {
        index--;
        assert(strncmp((char*)entry.value, expected_values_reverse[index], 2) == 0);
    }
    assert_true(index == 0, "should have iterated 5 times");
    quicklistReleaseIterator(iter);

    quicklistRelease(ql);
    printf("Testing quicklist add iterm success...\n");
}




static void test_quicklist_iterator()
{
    printf("Testing quicklist iterator ...\n");

    // 每个节点的 listpack 2个元素
    quicklist* ql = quicklistNew(2, 1);
    quicklistPushTail(ql, "a1", 2);
    quicklistPushHead(ql, "a2", 2);
    quicklistPushHead(ql, "a3", 2);
    quicklistPushHead(ql, "a4", 2);
    quicklistPushHead(ql, "a5", 2);
    assert_true(ql->len == 3, "ql len should 3");
    assert_true(ql->count == 5, "ql count should 5");
    assert_equal_int(ql->head->count, 1, "ql head count should 1");
    assert_equal_int(ql->tail->count, 2, "ql tail count should 2");
    // a5 a4,a3  a2,a1
    quicklistEntry entry;
    quicklistIter* iter = quicklistGetIterator(ql, AL_START_TAIL);
    int index = 0;
    char* expected_values_reverse[] = {"a1", "a2", "a3", "a4", "a5"};
    while (quicklistNext(iter, &entry) != 0)
    {
        assert(strncmp((char*)entry.value, expected_values_reverse[index], 2) == 0);
        index++;
    }
    assert_true(index == 5, "should have iterated 5 times");
    quicklistReleaseIterator(iter);

    iter = quicklistGetIteratorEntryAtIdx(ql, 0, &entry);

    assert(strncmp((char*)entry.value,"a5",2)==0);
    assert_equal_ptr(iter->current,ql->head,"should be equal");

    iter = quicklistGetIteratorEntryAtIdx(ql, 2, &entry);
    assert(strncmp((char*)entry.value,"a3",2)==0);

    quicklistReleaseIterator(iter);
    // a5  a4,a3  a2,a1
    quicklistDelRange(ql, -3, 2);


    quicklistRelease(ql);
    printf("Testing quicklist iterator success...\n");

}


static void  test_quicklist_compress()
{
    {
        printf("Testing quicklist add iterm...\n");

        // 每个节点的 listpack 2个元素
        quicklist* ql = quicklistNew(2, 1);
        quicklistPushTail(ql, "aaabbbcccdddeeefff123001", 24);
        quicklistPushHead(ql, "aaabbbcccdddeeefff123002", 24);
        quicklistPushHead(ql, "aaabbbcccdddeeefff123003", 24);
        quicklistPushHead(ql, "aaabbbcccdddeeefff123004", 24);
        quicklistPushHead(ql, "aaabbbcccdddeeefff123005", 24);
        assert(ql->len == 3);
        assert(ql->count == 5);
        quicklistRelease(ql);
    }
}

static void test_quicklist_pop_basic(void) {
    printf("Testing basic quicklistPop functionality...\n");

    quicklist *ql = quicklistNew(-2, 0);

    // 添加一些测试数据
    quicklistPushHead(ql, "item1", 6);
    quicklistPushHead(ql, "item2", 6);
    quicklistPushHead(ql, "item3", 6);

    assert(ql->count == 3);

    // 从头部弹出元素
    unsigned char *data;
    size_t sz;
    long long longval;

    // 弹出第一个元素
    int result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
    assert(result == 1);
    assert(data != NULL);
    assert(sz == 6);
    assert(strncmp((char*)data, "item3", 5) == 0);
    zfree(data);
    assert(ql->count == 2);

    // 弹出第二个元素
    result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
    assert(result == 1);
    assert(data != NULL);
    assert(sz == 6);
    assert(strncmp((char*)data, "item2", 5) == 0);
    zfree(data);
    assert(ql->count == 1);

    // 弹出第三个元素
    result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
    assert(result == 1);
    assert(data != NULL);
    assert(sz == 6);
    assert(strncmp((char*)data, "item1", 5) == 0);
    zfree(data);
    assert(ql->count == 0);

    // 尝试从空列表弹出
    result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
    assert(result == 0);

    quicklistRelease(ql);
    printf("PASSED: Basic quicklistPop functionality\n");
}

static void test_quicklist_pop_tail(void) {
    printf("Testing quicklistPop from tail...\n");

    quicklist *ql = quicklistNew(-2, 0);

    // 添加一些测试数据
    quicklistPushHead(ql, "item1", 6);
    quicklistPushHead(ql, "item2", 6);
    quicklistPushHead(ql, "item3", 6);

    assert(ql->count == 3);

    // 从尾部弹出元素
    unsigned char *data;
    size_t sz;
    long long longval;

    // 弹出第一个元素 (item1)
    int result = quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &longval);
    assert(result == 1);
    assert(data != NULL);
    assert(sz == 6);
    assert(strncmp((char*)data, "item1", 5) == 0);
    zfree(data);
    assert(ql->count == 2);

    // 弹出第二个元素 (item2)
    result = quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &longval);
    assert(result == 1);
    assert(data != NULL);
    assert(sz == 6);
    assert(strncmp((char*)data, "item2", 5) == 0);
    zfree(data);
    assert(ql->count == 1);

    // 弹出第三个元素 (item3)
    result = quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &longval);
    assert(result == 1);
    assert(data != NULL);
    assert(sz == 6);
    assert(strncmp((char*)data, "item3", 5) == 0);
    zfree(data);
    assert(ql->count == 0);

    // 尝试从空列表弹出
    result = quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &longval);
    assert(result == 0);

    quicklistRelease(ql);
    printf("PASSED: quicklistPop from tail\n");
}

static void test_quicklist_pop_numbers(void) {
    printf("Testing quicklistPop with numbers...\n");

    quicklist *ql = quicklistNew(-2, 0);

    // 添加数字字符串
    quicklistPushHead(ql, "100", 3);
    quicklistPushHead(ql, "200", 3);
    quicklistPushHead(ql, "-50", 3);

    assert(ql->count == 3);

    unsigned char *data;
    size_t sz;
    long long longval;

    // 弹出头部元素 (-50)
    int result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
    assert(result == 1);
    assert(data == NULL);
    assert(longval == -50);
    assert(ql->count == 2);

    // 弹出头部元素 (200)
    result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
    assert(result == 1);
    assert(data == NULL);
    assert(longval == 200);
    assert(ql->count == 1);

    // 弹出最后一个元素 (100)
    result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
    assert(result == 1);
    assert(data == NULL);
    assert(longval == 100);
    assert(ql->count == 0);

    quicklistRelease(ql);
    printf("PASSED: quicklistPop with numbers\n");
}

static void test_quicklist_pop_partial_params(void) {
    printf("Testing quicklistPop with partial parameters...\n");

    quicklist *ql = quicklistNew(-2, 0);

    // 添加测试数据
    quicklistPushHead(ql, "test_data", 9);

    unsigned char *data;
    size_t sz;
    long long longval;

    // 只获取数据和大小
    int result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, NULL);
    assert(result == 1);
    assert(data != NULL);
    assert(sz == 9);
    assert(strncmp((char*)data, "test_data", 9) == 0);
    zfree(data);

    // 重新添加数据
    quicklistPushHead(ql, "12345", 5);

    // 只获取长整型值
    result = quicklistPop(ql, QUICKLIST_HEAD, NULL, NULL, &longval);
    assert(result == 1);
    assert(longval == 12345);

    quicklistRelease(ql);
    printf("PASSED: quicklistPop with partial parameters\n");
}

void print_buffer(const char* buffer, int size) {
    for(int i = 0; i < size; i++) {
        printf("%c ", buffer[i]);
    }
    printf("\n");
}

static void test_quicklist_memmove(void)
{

    printf("=== 示例1: 基本内存移动 ===\n");
    char buffer1[] = "abcdefgh";
    printf("原始: %s\n", buffer1);
    /*
     * 原始: abcdefgh
     * 移动后: abcabcgh
     */

    // 将前3个字符移动到第4个位置
    memmove(buffer1 + 3, buffer1, 3);
    printf("移动后: %s\n", buffer1);  // 输出: abcabcgh

    printf("\n=== 示例2: 向前移动（数据压缩）===\n");
    char buffer2[] = "hello....world";  // '.' 代表空闲空间
    printf("原始: %s\n", buffer2);


    // 将 "world" 向前移动，覆盖空闲空间
    memmove(buffer2 + 5, buffer2 + 9, 6);  // 6包括'\0'
    printf("移动后: %s\n", buffer2);  // 输出: hello\0orld (实际显示为 "hello")
    /* 原始: hello....world
     * 移动后: helloworld
     */

    printf("\n=== 示例3: 向前移动（数据压缩）===\n");
    char buffer3[] = "hello....world";  // '.' 代表空闲空间
    printf("原始: %s\n", buffer3);

    // 将 "wor" 向前移动，覆盖空闲空间
    memmove(buffer3 + 5, buffer3 + 9, 3);  // 6包括'\0'
    printf("移动后: %s\n", buffer3);  // 输出: hello\0orld (实际显示为 "hello")

    char c_array[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    printf("移动前: %s\n", c_array); // 0123456789
    memcpy((void *)&c_array[5], (void *)&c_array[3], 5);
    printf("移动后: %s\n", c_array); // 0123434565
    // 第1步: c_array[5] = c_array[3]  →  '5' = '3'  →  0123436789
    // 第2步: c_array[6] = c_array[4]  →  '6' = '4'  →  0123434789
    // 第3步: c_array[7] = c_array[5]  →  '7' = '3'  →  0123434389
    // 第4步: c_array[8] = c_array[6]  →  '8' = '4'  →  0123434349
    // 第5步: c_array[9] = c_array[7]  →  '9' = '3'  →  0123434343


}

int main(void)
{
    printf("Starting quicklistPop tests...\n\n");

    // test_quicklist_add_iterm();
    test_quicklist_compress();
    // test_quicklist_iterator();
    // test_quicklist_pop_basic();
    // test_quicklist_pop_tail();
    // test_quicklist_pop_numbers();
    // test_quicklist_pop_partial_params();
    // test_quicklist_memmove();

    printf("\nAll quicklistPop tests passed!\n");
    return 0;
}

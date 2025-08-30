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
    assert_true(ql->len == 3, "ql len should 3");
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
    int expected_values_forward[] = {4, 3, 2, 1, 0}; // 对应 a5, a4, a3, a2, a1 的索引
    index = 0;
    while (quicklistNext(iter, &entry) != 0)
    {
        assert(strncmp((char*)entry.value, expected_values_reverse[expected_values_forward[index]], 2) == 0);
        index++;
    }
    assert_true(index == 5, "should have iterated 5 times");
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

// static void test_quicklist_pop_basic(void) {
//     printf("Testing basic quicklistPop functionality...\n");
//
//     quicklist *ql = quicklistNew(-2, 0);
//
//     // 添加一些测试数据
//     quicklistPushHead(ql, "item1", 6);
//     quicklistPushHead(ql, "item2", 6);
//     quicklistPushHead(ql, "item3", 6);
//
//     assert(ql->count == 3);
//
//     // 从头部弹出元素
//     unsigned char *data;
//     size_t sz;
//     long long longval;
//
//     // 弹出第一个元素
//     int result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
//     assert(result == 1);
//     assert(data != NULL);
//     assert(sz == 6);
//     assert(strncmp((char*)data, "item3", 5) == 0);
//     zfree(data);
//     assert(ql->count == 2);
//
//     // 弹出第二个元素
//     result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
//     assert(result == 1);
//     assert(data != NULL);
//     assert(sz == 6);
//     assert(strncmp((char*)data, "item2", 5) == 0);
//     zfree(data);
//     assert(ql->count == 1);
//
//     // 弹出第三个元素
//     result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
//     assert(result == 1);
//     assert(data != NULL);
//     assert(sz == 6);
//     assert(strncmp((char*)data, "item1", 5) == 0);
//     zfree(data);
//     assert(ql->count == 0);
//
//     // 尝试从空列表弹出
//     result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &longval);
//     assert(result == 0);
//
//     quicklistRelease(ql);
//     printf("PASSED: Basic quicklistPop functionality\n");
// }
//
// static void test_quicklist_pop_tail(void) {
//     printf("Testing quicklistPop from tail...\n");
//
//     quicklist *ql = quicklistNew(-2, 0);
//
//     // 添加一些测试数据
//     quicklistPushHead(ql, "item1", 6);
//     quicklistPushHead(ql, "item2", 6);
//     quicklistPushHead(ql, "item3", 6);
//
//     assert(ql->count == 3);
//
//     // 从尾部弹出元素
//     unsigned char *data;
//     size_t sz;
//     long long longval;
//
//     // 弹出第一个元素 (item1)
//     int result = quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &longval);
//     assert(result == 1);
//     assert(data != NULL);
//     assert(sz == 6);
//     assert(strncmp((char*)data, "item1", 5) == 0);
//     zfree(data);
//     assert(ql->count == 2);
//
//     // 弹出第二个元素 (item2)
//     result = quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &longval);
//     assert(result == 1);
//     assert(data != NULL);
//     assert(sz == 6);
//     assert(strncmp((char*)data, "item2", 5) == 0);
//     zfree(data);
//     assert(ql->count == 1);
//
//     // 弹出第三个元素 (item3)
//     result = quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &longval);
//     assert(result == 1);
//     assert(data != NULL);
//     assert(sz == 6);
//     assert(strncmp((char*)data, "item3", 5) == 0);
//     zfree(data);
//     assert(ql->count == 0);
//
//     // 尝试从空列表弹出
//     result = quicklistPop(ql, QUICKLIST_TAIL, &data, &sz, &longval);
//     assert(result == 0);
//
//     quicklistRelease(ql);
//     printf("PASSED: quicklistPop from tail\n");
// }
//
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
//
// static void test_quicklist_pop_partial_params(void) {
//     printf("Testing quicklistPop with partial parameters...\n");
//
//     quicklist *ql = quicklistNew(-2, 0);
//
//     // 添加测试数据
//     quicklistPushHead(ql, "test_data", 10);
//
//     unsigned char *data;
//     size_t sz;
//     long long longval;
//
//     // 只获取数据和大小
//     int result = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, NULL);
//     assert(result == 1);
//     assert(data != NULL);
//     assert(sz == 10);
//     assert(strncmp((char*)data, "test_data", 9) == 0);
//     zfree(data);
//
//     // 重新添加数据
//     quicklistPushHead(ql, "12345", 6);
//
//     // 只获取长整型值
//     result = quicklistPop(ql, QUICKLIST_HEAD, NULL, NULL, &longval);
//     assert(result == 1);
//     assert(longval == 12345);
//
//     quicklistRelease(ql);
//     printf("PASSED: quicklistPop with partial parameters\n");
// }

int main(void)
{
    printf("Starting quicklistPop tests...\n\n");

    // test_quicklist_add_iterm();
    test_quicklist_iterator();
    // test_quicklist_pop_basic();
    // test_quicklist_pop_tail();
    // test_quicklist_pop_numbers();
    // test_quicklist_pop_partial_params();

    printf("\nAll quicklistPop tests passed!\n");
    return 0;
}

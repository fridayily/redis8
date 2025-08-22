#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adlist.h"
#include "zmalloc.h"

// 自定义值复制函数（可选）
void *dupStringValue(void *value)
{
    char *str = (char *)value;
    char *copy = strdup(str);
    return copy;
}

// 自定义值释放函数（可选）
void freeStringValue(void *value)
{
    free(value);
}

// 自定义匹配函数（可选）
int matchStringValue(void *a, void *b)
{
    return strcmp((char *)a, (char *)b) == 0;
}

void printList(list *list)
{
    listIter iter;
    listRewind(list, &iter);
    listNode *node;
    printf("List contents: ");
    while ((node = listNext(&iter)) != NULL)
    {
        printf("%s->", (char *)listNodeValue(node));
    }
    printf("NULL\n");
}

// 简单的断言宏替代 cmocka
#define assert(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while(0)

#define assert_equal(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "Assertion failed at %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
        exit(1); \
    } \
} while(0)

#define assert_string_equal(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "Assertion failed at %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
        exit(1); \
    } \
} while(0)

// 测试创建和释放列表
static void test_list_create_and_release(void)
{
    list *myList = listCreate();
    assert(myList != NULL);
    assert_equal(listLength(myList), 0);

    listRelease(myList);
}

// 测试在列表头部添加节点
static void test_list_add_head(void)
{
    list *myList = listCreate();
    assert(myList != NULL);

    listAddNodeHead(myList, "a");
    listAddNodeHead(myList, "b");

    assert_equal(listLength(myList), 2);
    assert_string_equal((char *)listNodeValue(listFirst(myList)), "b");
    assert_string_equal((char *)listNodeValue(listLast(myList)), "a");

    listRelease(myList);
}

// 测试在列表尾部添加节点
static void test_list_add_tail(void)
{
    list *myList = listCreate();
    assert(myList != NULL);

    listAddNodeTail(myList, "a");
    listAddNodeTail(myList, "b");

    assert_equal(listLength(myList), 2);
    assert_string_equal((char *)listNodeValue(listFirst(myList)), "a");
    assert_string_equal((char *)listNodeValue(listLast(myList)), "b");

    listRelease(myList);
}

// 测试在指定节点前后插入新节点
static void test_list_insert_node(void)
{
    list *myList = listCreate();
    assert(myList != NULL);

    listAddNodeTail(myList, "a");
    listAddNodeTail(myList, "c");

    listNode *node_c = listSearchKey(myList, "c");
    listInsertNode(myList, node_c, "b", 0); // 在"c"之前插入"b"

    listNode *node_a = listSearchKey(myList, "a");
    listInsertNode(myList, node_a, "before", 1); // 在"a"之后插入"before"

    listInsertNode(myList, listFirst(myList), "after", 0); // 在第一个节点之前插入"after"

    assert_equal(listLength(myList), 5);
    assert_string_equal((char *)listNodeValue(listFirst(myList)), "after");
    assert_string_equal((char *)listNodeValue(listLast(myList)), "c");

    listRelease(myList);
}

// 测试删除节点
static void test_list_delete_node(void)
{
    list *myList = listCreate();
    assert(myList != NULL);

    listAddNodeTail(myList, "a");
    listAddNodeTail(myList, "b");
    listAddNodeTail(myList, "c");

    listNode *node_b = listSearchKey(myList, "b");
    listDelNode(myList, node_b);

    assert_equal(listLength(myList), 2);
    assert_string_equal((char *)listNodeValue(listFirst(myList)), "a");
    assert_string_equal((char *)listNodeValue(listLast(myList)), "c");

    listRelease(myList);
}

// 测试查找节点
static void test_list_search_key(void)
{
    list *myList = listCreate();
    assert(myList != NULL);

    listAddNodeTail(myList, "a");
    listAddNodeTail(myList, "b");
    listAddNodeTail(myList, "c");

    listNode *node = listSearchKey(myList, "b");
    assert(node != NULL);
    assert_string_equal((char *)listNodeValue(node), "b");

    node = listSearchKey(myList, "nonexistent");
    assert(node == NULL);

    listRelease(myList);
}

// 测试按索引查找节点
static void test_list_index(void)
{
    list *myList = listCreate();
    assert(myList != NULL);

    listAddNodeTail(myList, "a");
    listAddNodeTail(myList, "b");
    listAddNodeTail(myList, "c");

    listNode *node = listIndex(myList, 0); // 第一个节点
    assert(node != NULL);
    assert_string_equal((char *)listNodeValue(node), "a");

    node = listIndex(myList, -1); // 最后一个节点
    assert(node != NULL);
    assert_string_equal((char *)listNodeValue(node), "c");

    node = listIndex(myList, 10); // 超出范围的索引
    assert(node == NULL);

    listRelease(myList);
}

// 测试迭代器
static void test_list_iterator(void)
{
    list *myList = listCreate();
    assert(myList != NULL);

    listAddNodeTail(myList, "a");
    listAddNodeTail(myList, "b");
    listAddNodeTail(myList, "c");

    // 正向迭代
    listIter iter;
    listRewind(myList, &iter);
    listNode *node;
    int count = 0;
    const char *expected[] = {"a", "b", "c"};

    while ((node = listNext(&iter)) != NULL) {
        assert_string_equal((char *)listNodeValue(node), expected[count]);
        count++;
    }
    assert_equal(count, 3);

    // 反向迭代
    listRewindTail(myList, &iter);
    const char *expected_reverse[] = {"c", "b", "a"};
    count = 0;

    while ((node = listNext(&iter)) != NULL) {
        assert_string_equal((char *)listNodeValue(node), expected_reverse[count]);
        count++;
    }
    assert_equal(count, 3);

    listRelease(myList);
}

// 测试复制列表
static void test_list_duplicate(void)
{
    list *myList = listCreate();
    assert(myList != NULL);

    listSetDupMethod(myList, dupStringValue);
    listSetFreeMethod(myList, freeStringValue);
    listSetMatchMethod(myList, matchStringValue);

    char *str1 = strdup("a");
    char *str2 = strdup("b");
    char *str3 = strdup("c");
    // 在 ubuntu 中  strdup("a") 作为参数,测试用例成功,在 mac 中失败
    // listAddNodeTail(myList, strdup("a"))
    listAddNodeTail(myList, str1);
    listAddNodeTail(myList, str2);
    listAddNodeTail(myList, str3);

    list *copyList = listDup(myList);
    assert(copyList != NULL);
    assert_equal(listLength(copyList), 3);
    assert_string_equal((char *)listNodeValue(listFirst(copyList)), "a");
    assert_string_equal((char *)listNodeValue(listLast(copyList)), "c");

    // 修改原列表不应影响复制的列表
    char *str4 = strdup("d");
    listAddNodeTail(myList, str4);
    assert_equal(listLength(myList), 4);
    assert_equal(listLength(copyList), 3);

    listRelease(myList);
    listRelease(copyList);
}

// 测试 listSetFreeMethod 函数
static void test_list_set_free_method(void)
{
    list *myList = listCreate();
    assert(myList != NULL);

    // 设置自定义释放函数
    listSetFreeMethod(myList, freeStringValue);

    // 添加一些动态分配的字符串
    char *str1 = strdup("first");
    char *str2 = strdup("second");
    char *str3 = strdup("third");

    listAddNodeTail(myList, str1);
    listAddNodeTail(myList, str2);
    listAddNodeTail(myList, str3);

    assert_equal(listLength(myList), 3);
    assert_string_equal((char *)listNodeValue(listFirst(myList)), "first");
    assert_string_equal((char *)listNodeValue(listLast(myList)), "third");

    // 删除一个节点，验证自定义释放函数被调用
    listNode *node = listSearchKey(myList, str2);
    assert(node != NULL);

    listDelNode(myList, node);  // 这应该调用 freeStringValue 来释放 str2

    assert_equal(listLength(myList), 2);

    // 释放整个列表，验证所有节点值都被正确释放
    listRelease(myList);  // 这应该调用 freeStringValue 来释放 str1 和 str3
}

// 测试连接两个列表
static void test_list_join(void)
{
    list *list1 = listCreate();
    list *list2 = listCreate();

    listAddNodeTail(list1, "a");
    listAddNodeTail(list1, "b");

    listAddNodeTail(list2, "c");
    listAddNodeTail(list2, "d");

    listJoin(list1, list2);

    assert_equal(listLength(list1), 4);
    assert_string_equal((char *)listNodeValue(listFirst(list1)), "a");
    assert_string_equal((char *)listNodeValue(listLast(list1)), "d");

    // list2 应该已经被清空
    assert_equal(listLength(list2), 0);
    assert(listFirst(list2) == NULL);
    assert(listLast(list2) == NULL);

    listRelease(list1);
    listRelease(list2);
}

// 测试空列表操作
static void test_empty_list_operations(void)
{
    list *myList = listCreate();

    assert_equal(listLength(myList), 0);
    assert(listFirst(myList) == NULL);
    assert(listLast(myList) == NULL);
    assert(listIndex(myList, 0) == NULL);
    assert(listSearchKey(myList, "any") == NULL);

    listRelease(myList);
}

int main(void)
{
    printf("Running adlist tests...\n");

    test_list_create_and_release();
    printf("test_list_create_and_release passed\n");

    test_list_add_head();
    printf("test_list_add_head passed\n");

    test_list_add_tail();
    printf("test_list_add_tail passed\n");

    test_list_insert_node();
    printf("test_list_insert_node passed\n");

    test_list_delete_node();
    printf("test_list_delete_node passed\n");

    test_list_search_key();
    printf("test_list_search_key passed\n");

    test_list_index();
    printf("test_list_index passed\n");

    test_list_iterator();
    printf("test_list_iterator passed\n");

    test_list_duplicate();
    printf("test_list_duplicate passed\n");

    test_list_set_free_method();
    printf("test_list_set_free_method passed\n");


    test_list_join();
    printf("test_list_join passed\n");

    test_empty_list_operations();
    printf("test_empty_list_operations passed\n");

    printf("All tests passed!\n");
    return 0;
}

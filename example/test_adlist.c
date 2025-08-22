#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adlist.h"
#include "../tests/example/c/test_common.h"

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

// 测试创建和释放列表
static void test_list_create_and_release(void **state)
{
    UNUSED(state); // 未使用的参数
    
    list *myList = listCreate();
    assert_non_null(myList);
    assert_int_equal(listLength(myList), 0);
    
    listRelease(myList);
}

// 测试在头部添加节点
static void test_list_add_head(void **state)
{
    UNUSED(state);
    
    list *myList = listCreate();
    assert_non_null(myList);
    
    // 添加节点
    listAddNodeHead(myList, strdup("a"));
    listAddNodeHead(myList, strdup("b"));
    
    assert_int_equal(listLength(myList), 2);
    assert_string_equal((char *)listNodeValue(listFirst(myList)), "b");
    assert_string_equal((char *)listNodeValue(listLast(myList)), "a");
    
    listRelease(myList);
}

// 测试在尾部添加节点
static void test_list_add_tail(void **state)
{
    UNUSED(state);
    
    list *myList = listCreate();
    assert_non_null(myList);
    
    // 添加节点
    listAddNodeTail(myList, strdup("a"));
    listAddNodeTail(myList, strdup("b"));
    
    assert_int_equal(listLength(myList), 2);
    assert_string_equal((char *)listNodeValue(listFirst(myList)), "a");
    assert_string_equal((char *)listNodeValue(listLast(myList)), "b");
    
    listRelease(myList);
}

// 测试插入节点
static void test_list_insert_node(void **state)
{
    UNUSED(state);
    
    list *myList = listCreate();
    assert_non_null(myList);
    
    listAddNodeHead(myList, strdup("middle"));
    listNode *middle_node = listFirst(myList);
    
    // 在middle节点前插入
    listInsertNode(myList, middle_node, strdup("before"), 0);
    // 在middle节点后插入
    listInsertNode(myList, middle_node, strdup("after"), 1);
    
    assert_int_equal(listLength(myList), 3);
    assert_string_equal((char *)listNodeValue(listFirst(myList)), "before");
    assert_string_equal((char *)listNodeValue(listLast(myList)), "after");
    
    listRelease(myList);
}

// 测试删除节点
static void test_list_delete_node(void **state)
{
    UNUSED(state);
    
    list *myList = listCreate();
    assert_non_null(myList);
    
    listAddNodeHead(myList, strdup("a"));
    listAddNodeHead(myList, strdup("b"));
    listAddNodeHead(myList, strdup("c"));
    
    assert_int_equal(listLength(myList), 3);
    
    // 删除中间节点
    listNode *node_to_delete = listIndex(myList, 1);
    listDelNode(myList, node_to_delete);
    
    assert_int_equal(listLength(myList), 2);
    assert_string_equal((char *)listNodeValue(listFirst(myList)), "c");
    assert_string_equal((char *)listNodeValue(listLast(myList)), "a");
    
    listRelease(myList);
}

// 测试搜索功能
static void test_list_search_key(void **state)
{
    UNUSED(state);
    
    list *myList = listCreate();
    assert_non_null(myList);
    
    // 设置自定义方法
    listSetDupMethod(myList, dupStringValue);
    listSetFreeMethod(myList, freeStringValue);
    listSetMatchMethod(myList, matchStringValue);
    
    listAddNodeHead(myList, strdup("Hello"));
    listAddNodeTail(myList, strdup("World"));
    listInsertNode(myList, listFirst(myList), strdup("Middle"), 1);
    
    // 查找元素
    char *key = "World";
    listNode *found = listSearchKey(myList, key);
    assert_non_null(found);
    assert_string_equal((char *)listNodeValue(found), key);
    
    // 查找不存在的元素
    char *not_exist = "NotFound";
    listNode *not_found = listSearchKey(myList, not_exist);
    assert_null(not_found);
    
    listRelease(myList);
}

// 测试索引访问
static void test_list_index(void **state)
{
    UNUSED(state);
    
    list *myList = listCreate();
    assert_non_null(myList);
    
    listAddNodeTail(myList, strdup("a"));
    listAddNodeTail(myList, strdup("b"));
    listAddNodeTail(myList, strdup("c"));
    
    // 正向索引
    assert_string_equal((char *)listNodeValue(listIndex(myList, 0)), "a");
    assert_string_equal((char *)listNodeValue(listIndex(myList, 1)), "b");
    assert_string_equal((char *)listNodeValue(listIndex(myList, 2)), "c");
    
    // 负向索引
    assert_string_equal((char *)listNodeValue(listIndex(myList, -1)), "c");
    assert_string_equal((char *)listNodeValue(listIndex(myList, -2)), "b");
    assert_string_equal((char *)listNodeValue(listIndex(myList, -3)), "a");
    
    // 越界索引
    assert_null(listIndex(myList, 3));
    assert_null(listIndex(myList, -4));
    
    listRelease(myList);
}

// 测试迭代器
static void test_list_iterator(void **state)
{
    UNUSED(state);
    
    list *myList = listCreate();
    assert_non_null(myList);
    
    listAddNodeTail(myList, strdup("1"));
    listAddNodeTail(myList, strdup("2"));
    listAddNodeTail(myList, strdup("3"));
    
    // 正向迭代
    listIter iter;
    listRewind(myList, &iter);
    listNode *node;
    int count = 0;
    const char *expected[] = {"1", "2", "3"};
    
    while ((node = listNext(&iter)) != NULL) {
        assert_string_equal((char *)listNodeValue(node), expected[count]);
        count++;
    }
    assert_int_equal(count, 3);
    
    // 反向迭代
    listRewindTail(myList, &iter);
    count = 0;
    const char *expected_reverse[] = {"3", "2", "1"};
    
    while ((node = listNext(&iter)) != NULL) {
        assert_string_equal((char *)listNodeValue(node), expected_reverse[count]);
        count++;
    }
    assert_int_equal(count, 3);
    
    listRelease(myList);
}

// 测试列表复制
static void test_list_duplicate(void **state)
{
    UNUSED(state);
    
    list *myList = listCreate();
    assert_non_null(myList);
    
    // 设置自定义方法
    listSetDupMethod(myList, dupStringValue);
    listSetFreeMethod(myList, freeStringValue);
    listSetMatchMethod(myList, matchStringValue);
    
    listAddNodeTail(myList, strdup("a"));
    listAddNodeTail(myList, strdup("b"));
    listAddNodeTail(myList, strdup("c"));
    
    // 复制列表
    list *copyList = listDup(myList);
    assert_non_null(copyList);
    assert_int_equal(listLength(copyList), listLength(myList));
    
    // 验证内容相同但地址不同
    listIter iter1, iter2;
    listRewind(myList, &iter1);
    listRewind(copyList, &iter2);
    
    listNode *node1, *node2;
    while ((node1 = listNext(&iter1)) != NULL && (node2 = listNext(&iter2)) != NULL) {
        // 值相同
        assert_string_equal((char *)listNodeValue(node1), (char *)listNodeValue(node2));
        // 地址不同（深拷贝）
        assert_ptr_not_equal(listNodeValue(node1), listNodeValue(node2));
    }
    
    listRelease(myList);
    listRelease(copyList);
}

// 测试列表连接
static void test_list_join(void **state)
{
    UNUSED(state);
    
    list *list1 = listCreate();
    list *list2 = listCreate();
    
    listAddNodeTail(list1, strdup("a"));
    listAddNodeTail(list1, strdup("b"));
    
    listAddNodeTail(list2, strdup("c"));
    listAddNodeTail(list2, strdup("d"));
    
    assert_int_equal(listLength(list1), 2);
    assert_int_equal(listLength(list2), 2);
    
    // 连接列表
    listJoin(list1, list2);
    
    assert_int_equal(listLength(list1), 4);
    assert_int_equal(listLength(list2), 0);
    
    // 验证连接后的顺序
    assert_string_equal((char *)listNodeValue(listIndex(list1, 0)), "a");
    assert_string_equal((char *)listNodeValue(listIndex(list1, 1)), "b");
    assert_string_equal((char *)listNodeValue(listIndex(list1, 2)), "c");
    assert_string_equal((char *)listNodeValue(listIndex(list1, 3)), "d");
    
    listRelease(list1);
    listRelease(list2);
}

// 测试空列表操作
static void test_empty_list_operations(void **state)
{
    UNUSED(state);
    
    list *myList = listCreate();
    assert_non_null(myList);
    assert_int_equal(listLength(myList), 0);
    
    // 在空列表上操作
    assert_null(listFirst(myList));
    assert_null(listLast(myList));
    assert_null(listIndex(myList, 0));
    assert_null(listSearchKey(myList, "any"));
    
    listRelease(myList);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_list_create_and_release),
        cmocka_unit_test(test_list_add_head),
        cmocka_unit_test(test_list_add_tail),
        cmocka_unit_test(test_list_insert_node),
        cmocka_unit_test(test_list_delete_node),
        cmocka_unit_test(test_list_search_key),
        cmocka_unit_test(test_list_index),
        cmocka_unit_test(test_list_iterator),
        cmocka_unit_test(test_list_duplicate),
        cmocka_unit_test(test_list_join),
        cmocka_unit_test(test_empty_list_operations),
    };
    
    return cmocka_run_group_tests(tests, NULL, NULL);
}
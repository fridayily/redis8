#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adlist.h"

// 自定义值复制函数（可选）
void* dupStringValue(void *value) {
    char* str = (char*)value;
    char* copy = strdup(str);
    return copy;
}

// 自定义值释放函数（可选）
void freeStringValue(void *value) {
    free(value);
}

// 自定义匹配函数（可选）
int matchStringValue(void *a, void *b) {
    return strcmp((char*)a, (char*)b) == 0;
}

int main() {
    // 创建一个新的链表
    list *myList = listCreate();
    if (!myList) {
        fprintf(stderr, "Failed to create list\n");
        return 1;
    }

    // 设置自定义方法
    listSetDupMethod(myList, dupStringValue);
    listSetFreeMethod(myList, freeStringValue);
    listSetMatchMethod(myList, matchStringValue);

    // 添加节点到头部和尾部
    listAddNodeHead(myList, strdup("Hello"));
    listAddNodeTail(myList, strdup("World"));
    listInsertNode(myList, listFirst(myList), strdup("Middle"), 1);

    // 遍历链表打印内容
    listIter iter;
    listRewind(myList, &iter);
    listNode *node;

    printf("List contents:\n");
    while ((node = listNext(&iter)) != NULL) {
        printf("%s\n", (char*)listNodeValue(node));
    }

    // 查找元素
    char *key = "World";
    listNode *found = listSearchKey(myList, key);
    if (found) {
        printf("Found: %s\n", (char*)listNodeValue(found));
    } else {
        printf("Not found: %s\n", key);
    }

    // 删除所有节点
    printf("Releasing list...\n");
    listRelease(myList);

    return 0;
}
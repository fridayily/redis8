#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Fenwick Tree结构定义
typedef struct {
    unsigned long long *tree;  // 树状数组，1-based索引
    int size;                  // 数组大小
} FenwickTree;

// 创建Fenwick Tree
FenwickTree* createFenwickTree(int size) {
    FenwickTree *ft = (FenwickTree*)malloc(sizeof(FenwickTree));
    ft->size = size;
    // 分配size+1个元素，因为BIT使用1-based索引
    ft->tree = (unsigned long long*)calloc(size + 1, sizeof(unsigned long long));
    return ft;
}

// 释放Fenwick Tree内存
void freeFenwickTree(FenwickTree *ft) {
    if (ft) {
        free(ft->tree);
        free(ft);
    }
}

// Lowbit操作：获取x的二进制表示中最右边的1所代表的数值
static inline int lowbit(int x) {
    return x & (-x);
}

// 在位置index处增加delta值（1-based索引）
void fenwickUpdate(FenwickTree *ft, int index, long long delta) {
    // 确保索引在有效范围内
    printf("update begin index=%d \n",index);
    assert(index > 0 && index <= ft->size);

    // 从index位置开始，沿着树向上更新所有相关节点
    while (index <= ft->size) {
        ft->tree[index] += delta;
        printf("index: %d, lowbit %d\n",index, lowbit(index));
        index += lowbit(index);  // 跳转到父节点
    }
    printf("update end\n");

}

// 计算前缀和[1, index]的和（1-based索引）
unsigned long long fenwickPrefixSum(FenwickTree *ft, int index) {
    // 确保索引在有效范围内
    printf("sum index: %d\n",index);
    assert(index >= 0 && index <= ft->size);

    unsigned long long sum = 0;
    // 从index位置开始，沿着树向下累加
    while (index > 0) {
        sum += ft->tree[index];
        printf("index: %d, lowbit %d\n",index, lowbit(index));
        index -= lowbit(index);  // 跳转到前驱节点
    }
    return sum;
}

// 计算区间和[left, right]的和（1-based索引）
unsigned long long fenwickRangeSum(FenwickTree *ft, int left, int right) {
    assert(left > 0 && right <= ft->size && left <= right);

    if (left == 1) {
        return fenwickPrefixSum(ft, right);
    } else {
        return fenwickPrefixSum(ft, right) - fenwickPrefixSum(ft, left - 1);
    }
}

// 在指定位置设置值（1-based索引）
void fenwickSetValue(FenwickTree *ft, int index, unsigned long long value) {
    assert(index > 0 && index <= ft->size);

    // 先获取当前值，然后计算差值
    unsigned long long current = fenwickRangeSum(ft, index, index);
    long long delta = (long long)value - (long long)current;
    fenwickUpdate(ft, index, delta);
}

// 获取指定位置的值（1-based索引）
unsigned long long fenwickGetValue(FenwickTree *ft, int index) {
    assert(index > 0 && index <= ft->size);
    return fenwickRangeSum(ft, index, index);
}

// 打印Fenwick Tree的状态（用于调试）
void printFenwickTree(FenwickTree *ft) {
    printf("Fenwick Tree (size=%d):\n", ft->size);
    for (int i = 1; i <= ft->size; i++) {
        printf("  index %d: %llu\n", i, ft->tree[i]);
    }
    printf("\n");
}

// 示例和测试代码
int main() {
    // 创建一个大小为10的Fenwick Tree
    FenwickTree *ft = createFenwickTree(15);

    printf("=== Fenwick Tree 演示 ===\n");

    // 插入一些数据
    printf("1. 插入数据:\n");
    fenwickUpdate(ft, 1, 10);
    fenwickUpdate(ft, 2, 20);
    fenwickUpdate(ft, 3, 30);
    fenwickUpdate(ft, 4, 40);
    fenwickUpdate(ft, 5, 50);
    fenwickUpdate(ft, 6, 60);
    fenwickUpdate(ft, 7, 70);
    fenwickUpdate(ft, 8, 80);
    fenwickUpdate(ft, 9, 90);
    fenwickUpdate(ft, 10, 100);
    fenwickUpdate(ft, 11, 110);
    fenwickUpdate(ft, 12, 120);
    fenwickUpdate(ft, 13, 130);
    fenwickUpdate(ft, 14, 140);
    fenwickUpdate(ft, 15, 150);

    printf("插入数据后，前缀和:\n");
    printf("  前缀和[1,%d] = %llu\n", 3, fenwickPrefixSum(ft, 3));
    printf("  前缀和[1,%d] = %llu\n", 6, fenwickPrefixSum(ft, 6));
    printf("  前缀和[1,%d] = %llu\n", 11, fenwickPrefixSum(ft, 11));
    printf("  前缀和[1,%d] = %llu\n", 15, fenwickPrefixSum(ft, 15));

    printf("\n区间和:\n");
    printf("  区间和[2,4] = %llu\n", fenwickRangeSum(ft, 2, 4));
    printf("  区间和[1,3] = %llu\n", fenwickRangeSum(ft, 1, 3));

    // 修改某个位置的值
    printf("\n2. 修改位置3的值为100:\n");
    fenwickSetValue(ft, 3, 33);

    printf("\n2. 修改位置5的值为55:\n");
    fenwickSetValue(ft, 5, 55);

    printf("修改后，前缀和:\n");
    for (int i = 1; i <= 5; i++) {
        printf("  前缀和[1,%d] = %llu\n", i, fenwickPrefixSum(ft, i));
    }

    printf("\n单点查询:\n");
    for (int i = 1; i <= 5; i++) {
        printf("  位置%d的值 = %llu\n", i, fenwickGetValue(ft, i));
    }

    // 展示lowbit操作的原理
    printf("\n3. Lowbit操作演示:\n");
    int test_values[] = {1, 2, 3, 4, 5, 6, 7, 8, 12, 16};
    int num_tests = sizeof(test_values) / sizeof(test_values[0]);

    for (int i = 0; i < num_tests; i++) {
        int x = test_values[i];
        int lb = lowbit(x);
        printf("  lowbit(%d) = %d (二进制: %d = ", x, lb, x);

        // 打印二进制表示
        for (int j = 7; j >= 0; j--) {
            printf("%d", (x >> j) & 1);
        }
        printf(", ");
        for (int j = 7; j >= 0; j--) {
            printf("%d", ((-x) >> j) & 1);
        }
        printf(")\n");
    }

    // 释放内存
    freeFenwickTree(ft);

    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#define REDIS_TEST 1
// #include "../src/listpack.c"
// #include "../src/dict.c"
// #include "../src/quicklist.c"
// #include "../src/intset.c"
// #include "../src/zipmap.c"
#include "../src/testhelp.h"
#include "../src/mstr.c"

// 定义测试框架所需的全局变量
int __failed_tests = 0;
int __test_num = 0;

int main(int argc, char *argv[]) {
    UNUSED(argc);
    UNUSED(argv);
    // printf("Starting listpack internal tests...\n");
    // if (listpackTest(argc, argv, 0) == 0) {
    //     printf("All listpack internal tests passed!\n");
    //     return 0;
    // } else  {
    //     printf("Some listpack internal tests failed!\n");
    //     return 1;
    // }

    // const char *monotonic_info = monotonicInit();
    // printf("Monotonic clock initialized: %s\n", monotonic_info);
    // if (dictTest(argc, argv, 0)==0) {
    //     printf("All dict internal tests passed!\n");
    //     return 0;
    // }else  {
    //     printf("Some dict internal tests failed!\n");
    //     return 1;
    // }

    // printf("Starting quicklist internal tests...\n");
    // if (quicklistTest(argc, argv, 0) == 0) {
    //     printf("All quicklist internal tests passed!\n");
    //     return 0;
    // } else  {
    //     printf("Some quicklist internal tests failed!\n");
    //     return 1;
    // }

    // printf("Starting intsetTest internal tests...\n");
    // if (intsetTest(argc, argv, 0) == 0) {
    //     printf("All intsetTest internal tests passed!\n");
    //     return 0;
    // } else  {
    //     printf("Some quicklist internal tests failed!\n");
    //     return 1;
    // }

    // printf("Starting intsetTest internal tests...\n");
    // if (zipmapTest(argc, argv, 0) == 0) {
    //     printf("All intsetTest internal tests passed!\n");
    //     return 0;
    // } else  {
    //     printf("Some quicklist internal tests failed!\n");
    //     return 1;
    // }

    printf("Starting mstrTest internal tests...\n");
    if (mstrTest(argc, argv, 0) == 0) {
        printf("All mstrTest internal tests passed!\n");
        return 0;
    } else  {
        printf("Some mstrTest internal tests failed!\n");
        return 1;
    }


}
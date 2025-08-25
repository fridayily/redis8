#include <stdio.h>
#include <stdlib.h>
#define REDIS_TEST 1
#include "../src/listpack.c"
#include "../src/dict.c"

int main(int argc, char *argv[]) {
    UNUSED(argc);
    UNUSED(argv);
    printf("Starting listpack internal tests...\n");
    if (listpackTest(argc, argv, 0) == 0) {
        printf("All listpack internal tests passed!\n");
        return 0;
    } else  {
        printf("Some listpack internal tests failed!\n");
        return 1;
    }

    // const char *monotonic_info = monotonicInit();
    // printf("Monotonic clock initialized: %s\n", monotonic_info);
    // if (dictTest(argc, argv, 0)==0) {
    //     printf("All dict internal tests passed!\n");
    //     return 0;
    // }else  {
    //     printf("Some dict internal tests failed!\n");
    //     return 1;
    // }
}
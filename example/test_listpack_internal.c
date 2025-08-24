#include <stdio.h>
#include <stdlib.h>
#define REDIS_TEST 1
#include "../src/listpack.c"

int main(int argc, char *argv[]) {
    printf("Starting listpack internal tests...\n");
    if (listpackTest(argc, argv, 0) == 0) {
        printf("All listpack internal tests passed!\n");
        return 0;
    } else {
        printf("Some listpack internal tests failed!\n");
        return 1;
    }
}
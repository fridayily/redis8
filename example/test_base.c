#include <stdio.h>

void foo(int x,int y,int z)
{
    int a = 10,b = 20,c=20;
    printf("&x = %p\n", (void*)&x);
    printf("&y = %p\n", (void*)&y);
    printf("&z = %p\n", (void*)&z);
    printf("&a = %p\n", (void*)&a);
    printf("&b = %p\n", (void*)&b);
    printf("&c = %p\n", (void*)&c);
}

void point_test()
{
    int x = 1, y = 2, z = 3;
    printf("&x = %p\n", (void*)&x);
    printf("&y = %p\n", (void*)&y);
    printf("&z = %p\n", (void*)&z);
    printf("--------\n");
    foo(x, y, z);
}


void endian_test()
{
    // 0xbc614e
    int i = 12345678;
    // 在mac中调试时,在内存中顺序为 4e 61 bc 00,所以为小端序
    printf("i=%d",i);
}


int main() {
    endian_test();
    return 0;
}
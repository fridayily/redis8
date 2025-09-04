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



int main() {
    int x = 1, y = 2, z = 3;
    printf("&x = %p\n", (void*)&x);
    printf("&y = %p\n", (void*)&y);
    printf("&z = %p\n", (void*)&z);
    printf("--------\n");
    foo(x, y, z);
    return 0;
}
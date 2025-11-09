#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

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

void daemonize(void) {
    // 返回进程id
    pid_t pid = getpid();
    // 返回 session_id
    pid_t sid = getsid(0);
    // 返回进程组ID
    pid_t pgid = getpgid(0);
    printf("pid %d\n",pid); // 997
    printf("sid %d\n",sid); // 1
    printf("pgid %d\n",pgid);  // 997
    printf("------------------\n");
    if (fork() != 0) exit(0); // 父进程退出
    // 子进程继续执行
    pid_t child_pid = getpid();
    pid_t child_sid = getsid(0);
    pid_t child_pgid = getpgid(0);
    printf("child_pid %d\n",child_pid); // 1000
    printf("child_sid %d\n",child_sid); // 1
    printf("child_pgid %d\n",child_pgid); // 997
    printf("------------------\n");
    setsid();
    pid_t new_pid = getpid();
    pid_t new_sid = getsid(0);
    pid_t new_pgid = getpgid(0);
    printf("new_pid %d\n",new_pid); // 1000
    printf("new_sid %d\n",new_sid); // 1000
    printf("new_pgid %d\n",new_pgid); // 1000
}

int main() {
    daemonize();
    return 0;
}
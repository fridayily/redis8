#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <hiredis.h>
#include <async.h>
#include <adapters/ae.h>

/* Put event loop in the global scope, so it can be explicitly stopped */
static aeEventLoop *loop;

void getCallback(redisAsyncContext *c, void *r, void *privdata) {
    redisReply *reply = r;
    if (reply == NULL) return;
    printf("argv[%s]: %s\n", (char*)privdata, reply->str);

    /* Disconnect after receiving the reply to GET */
    redisAsyncDisconnect(c);
}

void connectCallback(const redisAsyncContext *c, int status) {
    D("connectCallback");
    if (status != REDIS_OK) {
        printf("Error: %s\n", c->errstr);
        aeStop(loop);
        return;
    }

    printf("Connected...\n");
}

void disconnectCallback(const redisAsyncContext *c, int status) {
    if (status != REDIS_OK) {
        printf("Error: %s\n", c->errstr);
        aeStop(loop);
        return;
    }

    printf("Disconnected...\n");
    aeStop(loop);
}

int main (int argc, char **argv) {
    // 忽略 SIGPIPE 信号
    // 如果不忽略
    //      客户端 → 发送数据到已关闭的连接 → 内核发送 SIGPIPE 信号 → 进程被终止
    // 如果忽略
    //     客户端 → 发送数据到已关闭的连接 → write() 返回 -1，errno 设置为 EPIPE
    //     → 程序可以检查返回值并进行错误处理
    signal(SIGPIPE, SIG_IGN);

    // 建立客户端连接
    redisAsyncContext *c = redisAsyncConnect("127.0.0.1", 6379);
    if (c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", c->errstr);
        return 1;
    }

    // 创建事件循环
    loop = aeCreateEventLoop(64);
    redisAeAttach(loop, c);
    redisAsyncSetConnectCallback(c,connectCallback);
    // 注册关闭连接时的回调函数
    redisAsyncSetDisconnectCallback(c,disconnectCallback);
    // redisAsyncCommand(c, NULL, NULL, "SET key %b", argv[argc-1], strlen(argv[argc-1]));
    // 用更简单可控的命令来测试
    redisAsyncCommand(c, NULL, NULL, "SET key %b", "foo", 3);
    // getCallback 会断开连接, 中断 aeMain 的 while 循环
    redisAsyncCommand(c, getCallback, (char*)"end-1", "GET key");
    aeMain(loop);
    return 0;
}


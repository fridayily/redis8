#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis.h>

void test_connection(redisContext* c)
{
    printf("测试 connection...\n");

    redisReply* reply;

    // 清空可能存在的key
    reply = redisCommand(c, "ping");
    if (reply == NULL)
    {
        printf("连接测试失败: 无法获取回复\n");
        return;
    }
    if (reply->type == REDIS_REPLY_ERROR)
    {
        printf("连接测试失败: %s\n", reply->str);
        freeReplyObject(reply);
        return;
    }
    printf("Ping 响应: %s\n", reply->str);
    freeReplyObject(reply);
    printf("基本操作测试 PASSED\n\n");
}


int main(int argc, char** argv)
{
    unsigned int j;
    redisContext* c;
    redisReply* reply;

    const char* hostname = "127.0.0.1";
    int port = 6379;

    struct timeval timeout = {1, 500000}; // 1.5 seconds
    c = redisConnectWithTimeout(hostname, port, timeout);
    if (c == NULL || c->err)
    {
        if (c)
        {
            printf("连接错误: %s\n", c->errstr);
            redisFree(c);
        }
        else
        {
            printf("连接错误: 无法分配redis上下文\n");
        }
        exit(1);
    }


    // 运行所有测试
    test_connection(c);

    // 断开连接
    redisFree(c);
    return 0;
}

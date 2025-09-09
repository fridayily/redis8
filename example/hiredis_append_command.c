#include <stdio.h>
#include <hiredis.h>



int main() {
    // 1. 建立 Redis 连接
    redisContext *c = redisConnect("127.0.0.1", 6379);
    if (c == NULL || c->err) {
        if (c) {
            printf("连接错误: %s\n", c->errstr);
            redisFree(c);
        } else {
            printf("无法分配连接上下文\n");
        }
        return 1;
    }
    printf("连接 Redis 成功\n");

    // 2. 向缓冲区追加多个命令
    redisAppendCommand(c, "SET key1 %s", "value1");  // SET key1 value1
    redisAppendCommand(c, "GET key1");               // GET key1
    redisAppendCommand(c, "INCR counter");           // INCR counter

    // 3. 将缓冲区命令发送到服务器（非阻塞，可循环调用确保发送完成）
    int done = 0;
    while (!done) {
        if (redisBufferWrite(c, &done) != REDIS_OK) {
            printf("发送命令失败: %s\n", c->errstr);
            redisFree(c);
            return 1;
        }
    }

    // 4. 逐个获取命令响应（顺序与追加命令一致）
    redisReply *reply;

    // 获取 SET 命令的响应
    if (redisGetReply(c, (void**)&reply) != REDIS_OK) {
        printf("获取响应失败: %s\n", c->errstr);
        redisFree(c);
        return 1;
    }
    printf("SET 响应: %s\n", reply->str);  // 输出 "OK"
    freeReplyObject(reply);

    // 获取 GET 命令的响应
    if (redisGetReply(c, (void**)&reply) != REDIS_OK) {
        printf("获取响应失败: %s\n", c->errstr);
        redisFree(c);
        return 1;
    }
    printf("GET 响应: %s\n", reply->str);  // 输出 "value1"
    freeReplyObject(reply);

    // 获取 INCR 命令的响应
    if (redisGetReply(c, (void**)&reply) != REDIS_OK) {
        printf("获取响应失败: %s\n", c->errstr);
        redisFree(c);
        return 1;
    }
    printf("INCR 响应: %lld\n", reply->integer);  // 输出 1（首次自增）
    freeReplyObject(reply);

    redisFree(c);
    return 0;
}

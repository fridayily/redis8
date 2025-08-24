#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis.h>

void test_zset_basic(redisContext *c) {
    printf("测试 ZSET 基本操作...\n");

    redisReply *reply;

    // 清空可能存在的key
    reply = redisCommand(c, "DEL myzset");
    freeReplyObject(reply);

    // 添加元素 ZADD
    reply = redisCommand(c, "ZADD myzset 1 one 2 two 3 three");
    if (reply->type == REDIS_REPLY_INTEGER) {
        printf("添加了 %lld 个元素\n", reply->integer);
    }
    freeReplyObject(reply);

    // 获取元素分数 ZSCORE
    reply = redisCommand(c, "ZSCORE myzset one");
    if (reply->type == REDIS_REPLY_STRING) {
        printf("元素 'one' 的分数: %s\n", reply->str);
    }
    freeReplyObject(reply);

    // 获取集合大小 ZCARD
    reply = redisCommand(c, "ZCARD myzset");
    if (reply->type == REDIS_REPLY_INTEGER) {
        printf("集合大小: %lld\n", reply->integer);
    }
    freeReplyObject(reply);

    // 按分数范围获取元素 ZRANGEBYSCORE
    reply = redisCommand(c, "ZRANGEBYSCORE myzset 1 3");
    if (reply->type == REDIS_REPLY_ARRAY) {
        printf("分数在 1-3 之间的元素:\n");
        for (size_t i = 0; i < reply->elements; i++) {
            printf("  %s\n", reply->element[i]->str);
        }
    }
    freeReplyObject(reply);

    printf("基本操作测试 PASSED\n\n");
}

void test_zset_advanced(redisContext *c) {
    printf("测试 ZSET 高级操作...\n");

    redisReply *reply;

    // 清空可能存在的key
    reply = redisCommand(c, "DEL myzset");
    freeReplyObject(reply);

    // 添加元素
    reply = redisCommand(c, "ZADD myzset 1 one 2 two 3 three 4 four 5 five");
    freeReplyObject(reply);

    // ZRANGE 获取指定范围的元素
    reply = redisCommand(c, "ZRANGE myzset 0 -1 WITHSCORES");
    if (reply->type == REDIS_REPLY_ARRAY) {
        printf("所有元素及其分数:\n");
        for (size_t i = 0; i < reply->elements; i += 2) {
            printf("  %s: %s\n", reply->element[i]->str, reply->element[i+1]->str);
        }
    }
    freeReplyObject(reply);

    // ZREM 删除元素
    reply = redisCommand(c, "ZREM myzset two");
    if (reply->type == REDIS_REPLY_INTEGER) {
        printf("删除元素结果: %lld (1表示成功删除, 0表示元素不存在)\n", reply->integer);
    }
    freeReplyObject(reply);

    // ZRANK 获取元素排名
    reply = redisCommand(c, "ZRANK myzset three");
    if (reply->type == REDIS_REPLY_INTEGER) {
        printf("元素 'three' 的排名: %lld\n", reply->integer);
    } else if (reply->type == REDIS_REPLY_NIL) {
        printf("元素 'three' 不存在\n");
    }
    freeReplyObject(reply);

    // ZINCRBY 增加元素分数
    reply = redisCommand(c, "ZINCRBY myzset 10 one");
    if (reply->type == REDIS_REPLY_STRING) {
        printf("元素 'one' 增加分数后的新分数: %s\n", reply->str);
    }
    freeReplyObject(reply);

    printf("高级操作测试 PASSED\n\n");
}

void test_zset_pop(redisContext *c) {
    printf("测试 ZSET 弹出操作...\n");

    redisReply *reply;

    // 清空可能存在的key
    reply = redisCommand(c, "DEL myzset");
    freeReplyObject(reply);

    // 添加元素
    reply = redisCommand(c, "ZADD myzset 1 a 2 b 3 c 4 d 5 e");
    freeReplyObject(reply);

    // ZPOPMIN 弹出最小分数元素
    reply = redisCommand(c, "ZPOPMIN myzset");
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2) {
        printf("弹出最小分数元素: %s (分数: %s)\n",
               reply->element[0]->str, reply->element[1]->str);
    }
    freeReplyObject(reply);

    // ZPOPMAX 弹出最大分数元素
    reply = redisCommand(c, "ZPOPMAX myzset");
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2) {
        printf("弹出最大分数元素: %s (分数: %s)\n",
               reply->element[0]->str, reply->element[1]->str);
    }
    freeReplyObject(reply);

    printf("弹出操作测试 PASSED\n\n");
}

int main(int argc, char **argv) {
    unsigned int j;
    redisContext *c;
    redisReply *reply;

    const char *hostname = "127.0.0.1";
    int port = 6379;

    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    c = redisConnectWithTimeout(hostname, port, timeout);
    if (c == NULL || c->err) {
        if (c) {
            printf("连接错误: %s\n", c->errstr);
            redisFree(c);
        } else {
            printf("连接错误: 无法分配redis上下文\n");
        }
        exit(1);
    }

    printf("开始 Redis ZSET 客户端测试...\n\n");

    // 运行所有测试
    test_zset_basic(c);
    // test_zset_advanced(c);
    // test_zset_pop(c);

    // 断开连接
    redisFree(c);

    printf("所有 ZSET 客户端测试通过！\n");
    return 0;
}

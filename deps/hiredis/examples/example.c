#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis.h>

#ifdef _MSC_VER
#include <winsock2.h> /* For struct timeval */
#endif

static void example_argv_command(redisContext *c, size_t n) {
    char **argv, tmp[42];
    size_t *argvlen;
    redisReply *reply;

    /* We're allocating two additional elements for command and key */
    argv = malloc(sizeof(*argv) * (2 + n));
    argvlen = malloc(sizeof(*argvlen) * (2 + n));

    /* First the command */
    argv[0] = (char*)"RPUSH";
    argvlen[0] = sizeof("RPUSH") - 1;

    /* Now our key */
    argv[1] = (char*)"argvlist";
    argvlen[1] = sizeof("argvlist") - 1;

    /* Now add the entries we wish to add to the list */
    for (size_t i = 2; i < (n + 2); i++) {
        argvlen[i] = snprintf(tmp, sizeof(tmp), "argv-element-%zu", i - 2);
        argv[i] = strdup(tmp);
    }

    /* Execute the command using redisCommandArgv.  We're sending the arguments with
     * two explicit arrays.  One for each argument's string, and the other for its
     * length. */
    reply = redisCommandArgv(c, n + 2, (const char **)argv, (const size_t*)argvlen);

    if (reply == NULL || c->err) {
        fprintf(stderr, "Error:  Couldn't execute redisCommandArgv\n");
        exit(1);
    }

    if (reply->type == REDIS_REPLY_INTEGER) {
        printf("%s reply: %lld\n", argv[0], reply->integer);
    }

    freeReplyObject(reply);

    /* Clean up */
    for (size_t i = 2; i < (n + 2); i++) {
        free(argv[i]);
    }

    free(argv);
    free(argvlen);
}


static void example_stream(redisContext *c) {
    redisReply *reply;
    reply = redisCommand(c, "XADD mystream * sensor-id 1234 temperature 19.8");
    if (reply->type == REDIS_REPLY_ERROR) {
        printf("XADD 错误: %s\n", reply->str);
    } else {
        printf("添加消息 ID: %s\n", reply->str);
    }
    freeReplyObject(reply);
    // 创建消费者组
    reply = redisCommand(c, "XGROUP CREATE mystream mygroup $ MKSTREAM");
    if (reply->type == REDIS_REPLY_ERROR) {
        printf("XGROUP CREATE 错误: %s\n", reply->str);
    } else {
        printf("消费者组创建成功\n");
    }
    freeReplyObject(reply);

    // 添加更多测试数据
    reply = redisCommand(c, "XADD mystream * sensor-id 1235 temperature 20.1");
    freeReplyObject(reply);

    reply = redisCommand(c, "XADD mystream * sensor-id 1236 temperature 21.5");
    freeReplyObject(reply);

    // 从消费者组读取消息
    reply = redisCommand(c, "XREADGROUP GROUP mygroup consumer1 COUNT 2 STREAMS mystream >");
    if (reply->type == REDIS_REPLY_ARRAY) {
        char **message_ids = malloc(sizeof(char*) * 10); // 假设最多10条消息
        int msg_count = 0;
        for (size_t i = 0; i < reply->elements; i++) {
            redisReply *stream = reply->element[i];
            printf("Stream: %s\n", stream->element[0]->str);

            redisReply *messages = stream->element[1];

            for (size_t j = 0; j < messages->elements; j++) {
                redisReply *message = messages->element[j];
                printf("  消息 ID: %s\n", message->element[0]->str);
                // 保存消息ID用于后续确认
                if (msg_count < 10) {
                    message_ids[msg_count] = strdup(message->element[0]->str);
                    msg_count++;
                }
                redisReply *fields = message->element[1];
                for (size_t k = 0; k < fields->elements; k += 2) {
                    printf("    %s: %s\n",
                           fields->element[k]->str,
                           fields->element[k+1]->str);
                }
            }
        }
        // 确认消息处理完成
        if (msg_count > 0) {
            // 构建 XACK 命令
            char *cmd = malloc(256);
            int offset = sprintf(cmd, "XACK mystream mygroup");
            for (int i = 0; i < msg_count; i++) {
                offset += sprintf(cmd + offset, " %s", message_ids[i]);
            }

            redisReply *ack_reply = redisCommand(c, cmd);
            if (ack_reply->type != REDIS_REPLY_ERROR) {
                printf("确认了 %lld 条消息\n", ack_reply->integer);
            }
            freeReplyObject(ack_reply);
            free(cmd);

            // 释放消息ID内存
            for (int i = 0; i < msg_count; i++) {
                free(message_ids[i]);
            }
        }
        free(message_ids);
    }
    freeReplyObject(reply);

    // 确认消息处理完成
    // reply = redisCommand(c, "XACK mystream mygroup 1640995200000-0 1640995200001-0");
    // if (reply->type != REDIS_REPLY_ERROR) {
    //     printf("确认了 %lld 条消息\n", reply->integer);
    // }
    // freeReplyObject(reply);

    reply = redisCommand(c, "XINFO STREAM mystream");
    if (reply->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < reply->elements; i += 2) {
            printf("%s: ", reply->element[i]->str);
            redisReply *value = reply->element[i+1];
            if (value->type == REDIS_REPLY_INTEGER) {
                printf("%lld\n", value->integer);
            } else if (value->type == REDIS_REPLY_STRING) {
                printf("%s\n", value->str);
            }
        }
    }
    freeReplyObject(reply);

    // 完全删除整个 stream
    reply = redisCommand(c, "DEL mystream");
    if (reply->type != REDIS_REPLY_ERROR) {
        printf("已删除整个 stream\n");
    }
    freeReplyObject(reply);

}

int main(int argc, char **argv) {
    unsigned int j, isunix = 0;
    redisContext *c;
    redisReply *reply;
    const char *hostname = (argc > 1) ? argv[1] : "127.0.0.1";

    if (argc > 2) {
        if (*argv[2] == 'u' || *argv[2] == 'U') {
            isunix = 1;
            /* in this case, host is the path to the unix socket */
            printf("Will connect to unix socket @%s\n", hostname);
        }
    }

    int port = (argc > 2) ? atoi(argv[2]) : 6379;

    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    if (isunix) {
        c = redisConnectUnixWithTimeout(hostname, timeout);
    } else {
        c = redisConnectWithTimeout(hostname, port, timeout);
    }
    if (c == NULL || c->err) {
        if (c) {
            printf("Connection error: %s\n", c->errstr);
            redisFree(c);
        } else {
            printf("Connection error: can't allocate redis context\n");
        }
        exit(1);
    }

    /* PING server */
    reply = redisCommand(c,"PING");
    printf("PING: %s\n", reply->str);
    freeReplyObject(reply);

    /* Set a key */
    reply = redisCommand(c,"SET %s %s", "foo", "hello world");
    printf("SET: %s\n", reply->str);
    freeReplyObject(reply);

    /* Set a key using binary safe API */
    reply = redisCommand(c,"SET %b %b", "bar", (size_t) 3, "hello", (size_t) 5);
    printf("SET (binary API): %s\n", reply->str);
    freeReplyObject(reply);

    /* Try a GET and two INCR */
    reply = redisCommand(c,"GET foo");
    printf("GET foo: %s\n", reply->str);
    freeReplyObject(reply);

    reply = redisCommand(c,"INCR counter");
    printf("INCR counter: %lld\n", reply->integer);
    freeReplyObject(reply);
    /* again ... */
    reply = redisCommand(c,"INCR counter");
    printf("INCR counter: %lld\n", reply->integer);
    freeReplyObject(reply);

    /* Create a list of numbers, from 0 to 9 */
    reply = redisCommand(c,"DEL mylist");
    freeReplyObject(reply);
    for (j = 0; j < 10; j++) {
        char buf[64];

        snprintf(buf,64,"%u",j);
        reply = redisCommand(c,"LPUSH mylist element-%s", buf);
        freeReplyObject(reply);
    }

    /* Let's check what we have inside the list */
    reply = redisCommand(c,"LRANGE mylist 0 -1");
    if (reply->type == REDIS_REPLY_ARRAY) {
        for (j = 0; j < reply->elements; j++) {
            printf("%u) %s\n", j, reply->element[j]->str);
        }
    }
    freeReplyObject(reply);

    /* See function for an example of redisCommandArgv */
    example_argv_command(c, 10);
    example_stream(c);
    /* Disconnects and frees the context */
    redisFree(c);

    return 0;
}

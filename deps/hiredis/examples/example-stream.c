#include <hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  reply = redisCommand(
      c, "XREADGROUP GROUP mygroup consumer1 COUNT 2 STREAMS mystream >");
  if (reply->type == REDIS_REPLY_ARRAY) {
    char **message_ids = malloc(sizeof(char *) * 10); // 假设最多10条消息
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
          printf("    %s: %s\n", fields->element[k]->str,
                 fields->element[k + 1]->str);
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
  // reply = redisCommand(c, "XACK mystream mygroup 1640995200000-0
  // 1640995200001-0"); if (reply->type != REDIS_REPLY_ERROR) {
  //     printf("确认了 %lld 条消息\n", reply->integer);
  // }
  // freeReplyObject(reply);

  reply = redisCommand(c, "XINFO STREAM mystream");
  if (reply->type == REDIS_REPLY_ARRAY) {
    for (size_t i = 0; i < reply->elements; i += 2) {
      printf("%s: ", reply->element[i]->str);
      redisReply *value = reply->element[i + 1];
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

  struct timeval timeout = {1, 500000}; // 1.5 seconds

  c = redisConnectWithTimeout(hostname, port, timeout);

  if (c == NULL || c->err) {
    if (c) {
      printf("Connection error: %s\n", c->errstr);
      redisFree(c);
    } else {
      printf("Connection error: can't allocate redis context\n");
    }
    exit(1);
  }
  example_stream(c);
  redisFree(c);

  return 0;
}

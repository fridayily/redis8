//
// Created by L M on 2025/9/7.
//

#ifndef REDIS8_0_3_DEBUG_HIREDIS_DEBUG_H
#define REDIS8_0_3_DEBUG_HIREDIS_DEBUG_H

#define D(...) do { \
fprintf(stderr, "hiredis:%s:%d: ", __func__, __LINE__); \
fprintf(stderr, __VA_ARGS__); \
fprintf(stderr, "\n"); \
} while(0)



#define D_CMD(desc,c) do { \
fprintf(stderr, "hiredis:%s:%d fd=%d %s= ", __func__, __LINE__,(c)->fd,(desc));\
char* _debug_buf_ = get_redis_obuf_string(c); \
if (_debug_buf_) { \
fprintf(stderr, "%s", _debug_buf_); \
free(_debug_buf_); \
} \
} while(0)

struct redisContext;

char* get_redis_obuf_string(struct redisContext* c);

#endif //REDIS8_0_3_DEBUG_HIREDIS_DEBUG_H

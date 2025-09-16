// hiredis_debug.c
#include "hiredis_debug.h"
#include "hiredis.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

char* get_redis_obuf_string(struct redisContext* c)
{
    if (c == NULL || c->obuf == NULL)
    {
        char* result = strdup("Redis context or output buffer is NULL\n");
        return result;
    }

    size_t len = hi_sdslen(c->obuf);

    if (len == 0)
    {
        char* result = strdup("Buffer is empty\n");
        return result;
    }
    // 预估结果字符串的大小（最坏情况下每个字符都需要4个字节：\xHH）
    size_t estimated_size = len * 4 + 100;
    char* result = (char*)malloc(estimated_size);
    if (result == NULL) {
        return NULL;
    }

    size_t pos = 0;
    // pos += snprintf(result + pos, estimated_size - pos, "Redis output buffer (escaped format):\n");

    for (size_t i = 0; i < len && pos < estimated_size - 7; i++) {
        unsigned char ch = (unsigned char)c->obuf[i];

        switch (ch) {
        case '\r':
            pos += snprintf(result + pos, estimated_size - pos, "\\r");
            break;
        case '\n':
            pos += snprintf(result + pos, estimated_size - pos, "\\n");
            break;
        case '\t':
            pos += snprintf(result + pos, estimated_size - pos, "\\t");
            break;
        case '\\':
            pos += snprintf(result + pos, estimated_size - pos, "\\\\");
            break;
        case '"':
            pos += snprintf(result + pos, estimated_size - pos, "\\\"");
            break;
        default:
            if (isprint(ch)) {
                result[pos++] = ch;
            } else {
                pos += snprintf(result + pos, estimated_size - pos, "\\x%02x", ch);
            }
            break;
        }
    }

    pos += snprintf(result + pos, estimated_size - pos, "\n");

    // 调整内存大小以适应实际需要
    char* final_result = (char*)realloc(result, pos + 1);
    return final_result ? final_result : result;

}

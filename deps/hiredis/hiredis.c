/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2015, Matt Stancliff <matt at genges dot com>,
 *                     Jan-Erik Rediger <janerik at fnordig dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>

#include "hiredis.h"
#include "net.h"
#include "sds.h"
#include "async.h"
#include "win32.h"

#include "hiredis_debug.h"

extern int redisContextUpdateConnectTimeout(redisContext *c, const struct timeval *timeout);
extern int redisContextUpdateCommandTimeout(redisContext *c, const struct timeval *timeout);

static redisContextFuncs redisContextDefaultFuncs = {
    .close = redisNetClose,
    .free_privctx = NULL,
    .async_read = redisAsyncRead,
    .async_write = redisAsyncWrite,
    .read = redisNetRead,
    .write = redisNetWrite
};

static redisReply *createReplyObject(int type);
static void *createStringObject(const redisReadTask *task, char *str, size_t len);
static void *createArrayObject(const redisReadTask *task, size_t elements);
static void *createIntegerObject(const redisReadTask *task, long long value);
static void *createDoubleObject(const redisReadTask *task, double value, char *str, size_t len);
static void *createNilObject(const redisReadTask *task);
static void *createBoolObject(const redisReadTask *task, int bval);

/* Default set of functions to build the reply. Keep in mind that such a
 * function returning NULL is interpreted as OOM. */
static redisReplyObjectFunctions defaultFunctions = {
    createStringObject,
    createArrayObject,
    createIntegerObject,
    createDoubleObject,
    createNilObject,
    createBoolObject,
    freeReplyObject
};

/* Create a reply object */
static redisReply *createReplyObject(int type) {
    redisReply *r = hi_calloc(1,sizeof(*r));

    if (r == NULL)
        return NULL;

    r->type = type;
    return r;
}

/* Free a reply object */
void freeReplyObject(void *reply) {
    redisReply *r = reply;
    size_t j;

    if (r == NULL)
        return;

    switch(r->type) {
    case REDIS_REPLY_INTEGER:
    case REDIS_REPLY_NIL:
    case REDIS_REPLY_BOOL:
        break; /* Nothing to free */
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_MAP:
    case REDIS_REPLY_SET:
    case REDIS_REPLY_PUSH:
        if (r->element != NULL) {
            for (j = 0; j < r->elements; j++)
                freeReplyObject(r->element[j]);
            hi_free(r->element);
        }
        break;
    case REDIS_REPLY_ERROR:
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_DOUBLE:
    case REDIS_REPLY_VERB:
    case REDIS_REPLY_BIGNUM:
        hi_free(r->str);
        break;
    }
    hi_free(r);
}

static void *createStringObject(const redisReadTask *task, char *str, size_t len) {
    redisReply *r, *parent;
    char *buf;
    // 根据任务类型,创建 redisReply ,如 REDIS_REPLY_STRING,
    r = createReplyObject(task->type);
    if (r == NULL)
        return NULL;

    assert(task->type == REDIS_REPLY_ERROR  ||
           task->type == REDIS_REPLY_STATUS ||
           task->type == REDIS_REPLY_STRING ||
           task->type == REDIS_REPLY_VERB   ||
           task->type == REDIS_REPLY_BIGNUM);

    /* Copy string value */
    if (task->type == REDIS_REPLY_VERB) {
        buf = hi_malloc(len-4+1); /* Skip 4 bytes of verbatim type header. */
        if (buf == NULL) goto oom;

        memcpy(r->vtype,str,3);
        r->vtype[3] = '\0';
        memcpy(buf,str+4,len-4);
        buf[len-4] = '\0';
        r->len = len - 4;
    } else {
        buf = hi_malloc(len+1);
        if (buf == NULL) goto oom;

        memcpy(buf,str,len);
        // note: 这里已设置 '\0', 因此客户端的返回 redisReply->str 一般可以直接使用
        buf[len] = '\0';
        r->len = len;
    }
    r->str = buf; // 设置 reply 的内容
    // 如果返回的是一个 array 类型, 元素有字符串, parent 类型就是数组
    // 所以这种情况下数据是存储在 parent 中, 这种情况下我们需要的是这个数组
    // 这里返回的 r 不一定会使用,而直接忽略
    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == REDIS_REPLY_ARRAY ||
               parent->type == REDIS_REPLY_MAP ||
               parent->type == REDIS_REPLY_SET ||
               parent->type == REDIS_REPLY_PUSH);
        parent->element[task->idx] = r; // 数组的第 idx 个元素是 StringObject
    }
    return r;

oom:
    freeReplyObject(r);
    return NULL;
}

static void *createArrayObject(const redisReadTask *task, size_t elements) {
    redisReply *r, *parent;

    r = createReplyObject(task->type);
    if (r == NULL)
        return NULL;

    if (elements > 0) {
        r->element = hi_calloc(elements,sizeof(redisReply*));
        if (r->element == NULL) {
            freeReplyObject(r);
            return NULL;
        }
    }

    r->elements = elements;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == REDIS_REPLY_ARRAY ||
               parent->type == REDIS_REPLY_MAP ||
               parent->type == REDIS_REPLY_SET ||
               parent->type == REDIS_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createIntegerObject(const redisReadTask *task, long long value) {
    redisReply *r, *parent;

    r = createReplyObject(REDIS_REPLY_INTEGER);
    if (r == NULL)
        return NULL;

    r->integer = value;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == REDIS_REPLY_ARRAY ||
               parent->type == REDIS_REPLY_MAP ||
               parent->type == REDIS_REPLY_SET ||
               parent->type == REDIS_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createDoubleObject(const redisReadTask *task, double value, char *str, size_t len) {
    redisReply *r, *parent;

    if (len == SIZE_MAX) // Prevents hi_malloc(0) if len equals to SIZE_MAX
        return NULL;

    r = createReplyObject(REDIS_REPLY_DOUBLE);
    if (r == NULL)
        return NULL;

    r->dval = value;
    r->str = hi_malloc(len+1);
    if (r->str == NULL) {
        freeReplyObject(r);
        return NULL;
    }

    /* The double reply also has the original protocol string representing a
     * double as a null terminated string. This way the caller does not need
     * to format back for string conversion, especially since Redis does efforts
     * to make the string more human readable avoiding the calssical double
     * decimal string conversion artifacts. */
    memcpy(r->str, str, len);
    r->str[len] = '\0';
    r->len = len;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == REDIS_REPLY_ARRAY ||
               parent->type == REDIS_REPLY_MAP ||
               parent->type == REDIS_REPLY_SET ||
               parent->type == REDIS_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createNilObject(const redisReadTask *task) {
    redisReply *r, *parent;

    r = createReplyObject(REDIS_REPLY_NIL);
    if (r == NULL)
        return NULL;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == REDIS_REPLY_ARRAY ||
               parent->type == REDIS_REPLY_MAP ||
               parent->type == REDIS_REPLY_SET ||
               parent->type == REDIS_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createBoolObject(const redisReadTask *task, int bval) {
    redisReply *r, *parent;

    r = createReplyObject(REDIS_REPLY_BOOL);
    if (r == NULL)
        return NULL;

    r->integer = bval != 0;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == REDIS_REPLY_ARRAY ||
               parent->type == REDIS_REPLY_MAP ||
               parent->type == REDIS_REPLY_SET ||
               parent->type == REDIS_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;
}

/* Return the number of digits of 'v' when converted to string in radix 10.
 * Implementation borrowed from link in redis/src/util.c:string2ll(). */
static uint32_t countDigits(uint64_t v) {
  uint32_t result = 1;
  for (;;) {
    if (v < 10) return result;
    if (v < 100) return result + 1;
    if (v < 1000) return result + 2;
    if (v < 10000) return result + 3;
    v /= 10000U;
    result += 4;
  }
}

/* Helper that calculates the bulk length given a certain string length.
 * "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
 * 一个参数就是一个 bucket ,如 foo
 */
static size_t bulklen(size_t len) {
    return 1+countDigits(len)+2+len+2;
}

int redisvFormatCommand(char **target, const char *format, va_list ap) {
    const char *c = format;
    char *cmd = NULL; /* final command */
    int pos; /* position in final command */
    hisds curarg, newarg; /* current argument */
    int touched = 0; /* was the current argument touched? */
    char **curargv = NULL, **newargv = NULL;
    int argc = 0;
    int totlen = 0;
    int error_type = 0; /* 0 = no error; -1 = memory error; -2 = format error */
    int j;

    /* Abort if there is not target to set */
    if (target == NULL)
        return -1;

    /* Build the command string accordingly to protocol */
    curarg = hi_sdsempty();
    if (curarg == NULL)
        return -1;

    //  c 形如 "SET %s %s"
    while(*c != '\0') {
        if (*c != '%' || c[1] == '\0') {
            if (*c == ' ') {
                if (touched) {
                    // 当遇到空格且当前参数已被修改时，将参数保存到数组
                    newargv = hi_realloc(curargv,sizeof(char*)*(argc+1));
                    if (newargv == NULL) goto memory_err;
                    curargv = newargv;
                    // 将当前参数保存到参数数组
                    curargv[argc++] = curarg;
                    totlen += bulklen(hi_sdslen(curarg));

                    /* curarg is put in argv so it can be overwritten. */
                    /* 为下一个参数准备新的SDS字符串 */
                    curarg = hi_sdsempty();
                    if (curarg == NULL) goto memory_err;
                    touched = 0;
                }
            } else {
                // 保存输入参数, 如 PING, 每次添加一个字符到 newarg,
                // hi_sdscatlen 可能改变 curarg 地址
                newarg = hi_sdscatlen(curarg,c,1);
                if (newarg == NULL) goto memory_err;
                curarg = newarg;
                touched = 1;
            }
        } else {
            // 当前 c='%'
            char *arg;
            size_t size;

            /* Set newarg so it can be checked even if it is not touched. */
            newarg = curarg;

            switch(c[1]) {
            case 's':
                arg = va_arg(ap,char*);
                size = strlen(arg);
                if (size > 0)
                    newarg = hi_sdscatlen(curarg,arg,size);
                break;
            case 'b':
                arg = va_arg(ap,char*);
                size = va_arg(ap,size_t);
                if (size > 0)
                    newarg = hi_sdscatlen(curarg,arg,size);
                break;
            case '%':
                newarg = hi_sdscat(curarg,"%");
                break;
            default:
                /* Try to detect printf format */
                {
                    static const char intfmts[] = "diouxX";
                    static const char flags[] = "#0-+ ";
                    char _format[16];
                    const char *_p = c+1;
                    size_t _l = 0;
                    va_list _cpy;

                    /* Flags */
                    // 在字符串 flag  中查找字符 *_p 的第一次出现的位置
                    while (*_p != '\0' && strchr(flags,*_p) != NULL) _p++;

                    /* Field width */
                    while (*_p != '\0' && isdigit((int) *_p)) _p++;

                    /* Precision */
                    if (*_p == '.') {
                        _p++;
                        while (*_p != '\0' && isdigit((int) *_p)) _p++;
                    }

                    /* Copy va_list before consuming with va_arg */
                    va_copy(_cpy,ap);

                    /* Make sure we have more characters otherwise strchr() accepts
                     * '\0' as an integer specifier. This is checked after above
                     * va_copy() to avoid UB in fmt_invalid's call to va_end(). */
                    if (*_p == '\0') goto fmt_invalid;

                    /* Integer conversion (without modifiers) */
                    if (strchr(intfmts,*_p) != NULL) {
                        va_arg(ap,int);
                        goto fmt_valid;
                    }

                    /* Double conversion (without modifiers) */
                    if (strchr("eEfFgGaA",*_p) != NULL) {
                        va_arg(ap,double);
                        goto fmt_valid;
                    }

                    /* Size: char */
                    if (_p[0] == 'h' && _p[1] == 'h') {
                        _p += 2;
                        if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                            va_arg(ap,int); /* char gets promoted to int */
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                    /* Size: short */
                    if (_p[0] == 'h') {
                        _p += 1;
                        if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                            va_arg(ap,int); /* short gets promoted to int */
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                    /* Size: long long */
                    if (_p[0] == 'l' && _p[1] == 'l') {
                        _p += 2;
                        if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                            va_arg(ap,long long);
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                    /* Size: long */
                    if (_p[0] == 'l') {
                        _p += 1;
                        if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                            va_arg(ap,long);
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                fmt_invalid:
                    va_end(_cpy);
                    goto format_err;

                fmt_valid:
                    _l = (_p+1)-c;
                    if (_l < sizeof(_format)-2) {
                        memcpy(_format,c,_l);
                        _format[_l] = '\0';
                        newarg = hi_sdscatvprintf(curarg,_format,_cpy);

                        /* Update current position (note: outer blocks
                         * increment c twice so compensate here) */
                        c = _p-1;
                    }

                    va_end(_cpy);
                    break;
                }
            }

            if (newarg == NULL) goto memory_err;
            curarg = newarg;

            touched = 1;
            c++;
            if (*c == '\0')
                break;
        }
        c++;
    }

    /* Add the last argument if needed */
    if (touched) {
        newargv = hi_realloc(curargv,sizeof(char*)*(argc+1));
        if (newargv == NULL) goto memory_err;
        curargv = newargv;
        curargv[argc++] = curarg;
        totlen += bulklen(hi_sdslen(curarg));
    } else {
        hi_sdsfree(curarg);
    }

    /* Clear curarg because it was put in curargv or was free'd. */
    curarg = NULL;

    /* Add bytes needed to hold multi bulk count */
    totlen += 1+countDigits(argc)+2;

    /* Build the command at protocol level */
    cmd = hi_malloc(totlen+1);
    if (cmd == NULL) goto memory_err;
    // 构造 RESP 格式的字符串到 cmd 如 1\r\n$4\r\nPING\r\n
    pos = sprintf(cmd,"*%d\r\n",argc);
    for (j = 0; j < argc; j++) {
        // %z 是 C99 标准引入的长度修饰符，用于 size_t 类型
        // u 表示无符号十进制整数 (unsigned)
        // %zu 组合起来就是专门用于打印 size_t 类型的格式符
        pos += sprintf(cmd+pos,"$%zu\r\n",hi_sdslen(curargv[j]));
        memcpy(cmd+pos,curargv[j],hi_sdslen(curargv[j]));
        pos += hi_sdslen(curargv[j]);
        hi_sdsfree(curargv[j]);
        cmd[pos++] = '\r';
        cmd[pos++] = '\n';
    }
    assert(pos == totlen);
    cmd[pos] = '\0';

    hi_free(curargv);
    *target = cmd;
    return totlen;

format_err:
    error_type = -2;
    goto cleanup;

memory_err:
    error_type = -1;
    goto cleanup;

cleanup:
    if (curargv) {
        while(argc--)
            hi_sdsfree(curargv[argc]);
        hi_free(curargv);
    }

    hi_sdsfree(curarg);
    hi_free(cmd);

    return error_type;
}

/* Format a command according to the Redis protocol. This function
 * takes a format similar to printf:
 *
 * %s represents a C null terminated string you want to interpolate
 * %b represents a binary safe string
 *
 * When using %b you need to provide both the pointer to the string
 * and the length in bytes as a size_t. Examples:
 *
 * len = redisFormatCommand(target, "GET %s", mykey);
 * len = redisFormatCommand(target, "SET %s %b", mykey, myval, myvallen);
 */
int redisFormatCommand(char **target, const char *format, ...) {
    va_list ap;
    int len;
    va_start(ap,format);
    len = redisvFormatCommand(target,format,ap);
    va_end(ap);

    /* The API says "-1" means bad result, but we now also return "-2" in some
     * cases.  Force the return value to always be -1. */
    if (len < 0)
        len = -1;

    return len;
}

/* Format a command according to the Redis protocol using an hisds string and
 * hi_sdscatfmt for the processing of arguments. This function takes the
 * number of arguments, an array with arguments and an array with their
 * lengths. If the latter is set to NULL, strlen will be used to compute the
 * argument lengths.
 */
long long redisFormatSdsCommandArgv(hisds *target, int argc, const char **argv,
                                    const size_t *argvlen)
{
    hisds cmd, aux;
    unsigned long long totlen, len;
    int j;

    /* Abort on a NULL target */
    if (target == NULL)
        return -1;

    /* Calculate our total size */
    totlen = 1+countDigits(argc)+2;
    for (j = 0; j < argc; j++) {
        len = argvlen ? argvlen[j] : strlen(argv[j]);

        totlen += bulklen(len);
    }

    /* Use an SDS string for command construction */
    cmd = hi_sdsempty();
    if (cmd == NULL)
        return -1;

    /* We already know how much storage we need */
    aux = hi_sdsMakeRoomFor(cmd, totlen);
    if (aux == NULL) {
        hi_sdsfree(cmd);
        return -1;
    }

    cmd = aux;

    /* Construct command */
    cmd = hi_sdscatfmt(cmd, "*%i\r\n", argc);
    for (j=0; j < argc; j++) {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        cmd = hi_sdscatfmt(cmd, "$%U\r\n", len);
        cmd = hi_sdscatlen(cmd, argv[j], len);
        cmd = hi_sdscatlen(cmd, "\r\n", sizeof("\r\n")-1);
    }

    assert(hi_sdslen(cmd)==totlen);

    *target = cmd;
    return totlen;
}

void redisFreeSdsCommand(hisds cmd) {
    hi_sdsfree(cmd);
}

/* Format a command according to the Redis protocol. This function takes the
 * number of arguments, an array with arguments and an array with their
 * lengths. If the latter is set to NULL, strlen will be used to compute the
 * argument lengths.
 */
long long redisFormatCommandArgv(char **target, int argc, const char **argv, const size_t *argvlen) {
    char *cmd = NULL; /* final command */
    size_t pos; /* position in final command */
    size_t len, totlen;
    int j;

    /* Abort on a NULL target */
    if (target == NULL)
        return -1;

    /* Calculate number of bytes needed for the command */
    totlen = 1+countDigits(argc)+2;
    for (j = 0; j < argc; j++) {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        totlen += bulklen(len);
    }

    /* Build the command at protocol level */
    cmd = hi_malloc(totlen+1);
    if (cmd == NULL)
        return -1;

    pos = sprintf(cmd,"*%d\r\n",argc);
    for (j = 0; j < argc; j++) {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        pos += sprintf(cmd+pos,"$%zu\r\n",len);
        memcpy(cmd+pos,argv[j],len);
        pos += len;
        cmd[pos++] = '\r';
        cmd[pos++] = '\n';
    }
    assert(pos == totlen);
    cmd[pos] = '\0';

    *target = cmd;
    return totlen;
}

void redisFreeCommand(char *cmd) {
    hi_free(cmd);
}

void __redisSetError(redisContext *c, int type, const char *str) {
    size_t len;

    c->err = type;
    if (str != NULL) {
        len = strlen(str);
        // 错误消息的截取
        len = len < (sizeof(c->errstr)-1) ? len : (sizeof(c->errstr)-1);
        memcpy(c->errstr,str,len);
        c->errstr[len] = '\0';
    } else {
        /* Only REDIS_ERR_IO may lack a description! */
        assert(type == REDIS_ERR_IO);
        strerror_r(errno, c->errstr, sizeof(c->errstr));
    }
}

redisReader *redisReaderCreate(void) {
    return redisReaderCreateWithFunctions(&defaultFunctions);
}

static void redisPushAutoFree(void *privdata, void *reply) {
    (void)privdata;
    freeReplyObject(reply);
}

/*
 * 初始化 redisContext
 * 1.设置同步读写函数、异步读写函数、关闭 fd 函数 (redisContextDefaultFuncs)
 * 2.设置处理服务端返回的 StringObject, ArrayObject 的函数
 */
static redisContext *redisContextInit(void) {
    redisContext *c;

    c = hi_calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    // 初始化 redisContext 的默认函数,如读、写操作
    c->funcs = &redisContextDefaultFuncs;

    c->obuf = hi_sdsempty();
    // 初始化返回 redis 对象函数,如 StringObject, ArrayObject
    c->reader = redisReaderCreate();
    c->fd = REDIS_INVALID_FD;

    if (c->obuf == NULL || c->reader == NULL) {
        redisFree(c);
        return NULL;
    }

    return c;
}

void redisFree(redisContext *c) {
    if (c == NULL)
        return;

    if (c->funcs && c->funcs->close) {
        c->funcs->close(c);
    }

    hi_sdsfree(c->obuf);
    redisReaderFree(c->reader);
    hi_free(c->tcp.host);
    hi_free(c->tcp.source_addr);
    hi_free(c->unix_sock.path);
    hi_free(c->connect_timeout);
    hi_free(c->command_timeout);
    hi_free(c->saddr);

    if (c->privdata && c->free_privdata)
        c->free_privdata(c->privdata);

    if (c->funcs && c->funcs->free_privctx)
        c->funcs->free_privctx(c->privctx);

    memset(c, 0xff, sizeof(*c));
    hi_free(c);
}

redisFD redisFreeKeepFd(redisContext *c) {
    redisFD fd = c->fd;
    c->fd = REDIS_INVALID_FD;
    redisFree(c);
    return fd;
}

int redisReconnect(redisContext *c) {
    c->err = 0;
    memset(c->errstr, '\0', strlen(c->errstr));

    if (c->privctx && c->funcs->free_privctx) {
        c->funcs->free_privctx(c->privctx);
        c->privctx = NULL;
    }

    if (c->funcs && c->funcs->close) {
        c->funcs->close(c);
    }

    hi_sdsfree(c->obuf);
    redisReaderFree(c->reader);

    c->obuf = hi_sdsempty();
    c->reader = redisReaderCreate();

    if (c->obuf == NULL || c->reader == NULL) {
        __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
        return REDIS_ERR;
    }

    int ret = REDIS_ERR;
    if (c->connection_type == REDIS_CONN_TCP) {
        ret = redisContextConnectBindTcp(c, c->tcp.host, c->tcp.port,
               c->connect_timeout, c->tcp.source_addr);
    } else if (c->connection_type == REDIS_CONN_UNIX) {
        ret = redisContextConnectUnix(c, c->unix_sock.path, c->connect_timeout);
    } else {
        /* Something bad happened here and shouldn't have. There isn't
           enough information in the context to reconnect. */
        __redisSetError(c,REDIS_ERR_OTHER,"Not enough information to reconnect");
        ret = REDIS_ERR;
    }

    if (c->command_timeout != NULL && (c->flags & REDIS_BLOCK) && c->fd != REDIS_INVALID_FD) {
        redisContextSetTimeout(c, *c->command_timeout);
    }

    return ret;
}

/*
 * options 是用户的配置信息
 * 这里用配置信息初始化 redisContext
 *
 */
redisContext *redisConnectWithOptions(const redisOptions *options) {
    redisContext *c = redisContextInit();
    if (c == NULL) {
        return NULL;
    }
    if (!(options->options & REDIS_OPT_NONBLOCK)) {
        c->flags |= REDIS_BLOCK;
    }
    if (options->options & REDIS_OPT_REUSEADDR) {
        c->flags |= REDIS_REUSEADDR;
    }
    if (options->options & REDIS_OPT_NOAUTOFREE) {
        c->flags |= REDIS_NO_AUTO_FREE;
    }
    if (options->options & REDIS_OPT_NOAUTOFREEREPLIES) {
        c->flags |= REDIS_NO_AUTO_FREE_REPLIES;
    }
    if (options->options & REDIS_OPT_PREFER_IPV4) {
        c->flags |= REDIS_PREFER_IPV4;
    }
    if (options->options & REDIS_OPT_PREFER_IPV6) {
        c->flags |= REDIS_PREFER_IPV6;
    }

    /* Set any user supplied RESP3 PUSH handler or use freeReplyObject
     * as a default unless specifically flagged that we don't want one. */
    if (options->push_cb != NULL)
        redisSetPushCallback(c, options->push_cb);
    else if (!(options->options & REDIS_OPT_NO_PUSH_AUTOFREE))
        redisSetPushCallback(c, redisPushAutoFree);

    c->privdata = options->privdata;
    c->free_privdata = options->free_privdata;

    if (redisContextUpdateConnectTimeout(c, options->connect_timeout) != REDIS_OK ||
        redisContextUpdateCommandTimeout(c, options->command_timeout) != REDIS_OK) {
        __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
        return c;
    }

    if (options->type == REDIS_CONN_TCP) {
        redisContextConnectBindTcp(c, options->endpoint.tcp.ip,
                                   options->endpoint.tcp.port, options->connect_timeout,
                                   options->endpoint.tcp.source_addr);
    } else if (options->type == REDIS_CONN_UNIX) {
        redisContextConnectUnix(c, options->endpoint.unix_socket,
                                options->connect_timeout);
    } else if (options->type == REDIS_CONN_USERFD) {
        c->fd = options->endpoint.fd;
        c->flags |= REDIS_CONNECTED;
    } else {
        redisFree(c);
        return NULL;
    }

    if (c->err == 0 && c->fd != REDIS_INVALID_FD &&
        options->command_timeout != NULL && (c->flags & REDIS_BLOCK))
    {
        redisContextSetTimeout(c, *options->command_timeout);
    }

    return c;
}

/* Connect to a Redis instance. On error the field error in the returned
 * context will be set to the return value of the error function.
 * When no set of reply functions is given, the default set will be used. */
redisContext *redisConnect(const char *ip, int port) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options, ip, port);
    return redisConnectWithOptions(&options);
}

redisContext *redisConnectWithTimeout(const char *ip, int port, const struct timeval tv) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options, ip, port);
    options.connect_timeout = &tv;
    return redisConnectWithOptions(&options);
}

redisContext *redisConnectNonBlock(const char *ip, int port) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options, ip, port);
    options.options |= REDIS_OPT_NONBLOCK;
    return redisConnectWithOptions(&options);
}

redisContext *redisConnectBindNonBlock(const char *ip, int port,
                                       const char *source_addr) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options, ip, port);
    options.endpoint.tcp.source_addr = source_addr;
    options.options |= REDIS_OPT_NONBLOCK;
    return redisConnectWithOptions(&options);
}

redisContext *redisConnectBindNonBlockWithReuse(const char *ip, int port,
                                                const char *source_addr) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options, ip, port);
    options.endpoint.tcp.source_addr = source_addr;
    options.options |= REDIS_OPT_NONBLOCK|REDIS_OPT_REUSEADDR;
    return redisConnectWithOptions(&options);
}

redisContext *redisConnectUnix(const char *path) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_UNIX(&options, path);
    return redisConnectWithOptions(&options);
}

redisContext *redisConnectUnixWithTimeout(const char *path, const struct timeval tv) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_UNIX(&options, path);
    options.connect_timeout = &tv;
    return redisConnectWithOptions(&options);
}

redisContext *redisConnectUnixNonBlock(const char *path) {
    redisOptions options = {0};
    REDIS_OPTIONS_SET_UNIX(&options, path);
    options.options |= REDIS_OPT_NONBLOCK;
    return redisConnectWithOptions(&options);
}

redisContext *redisConnectFd(redisFD fd) {
    redisOptions options = {0};
    options.type = REDIS_CONN_USERFD;
    options.endpoint.fd = fd;
    return redisConnectWithOptions(&options);
}

/* Set read/write timeout on a blocking socket. */
int redisSetTimeout(redisContext *c, const struct timeval tv) {
    if (c->flags & REDIS_BLOCK)
        return redisContextSetTimeout(c,tv);
    return REDIS_ERR;
}

int redisEnableKeepAliveWithInterval(redisContext *c, int interval) {
    return redisKeepAlive(c, interval);
}

/* Enable connection KeepAlive. */
int redisEnableKeepAlive(redisContext *c) {
    return redisKeepAlive(c, REDIS_KEEPALIVE_INTERVAL);
}

/* Set the socket option TCP_USER_TIMEOUT. */
int redisSetTcpUserTimeout(redisContext *c, unsigned int timeout) {
    return redisContextSetTcpUserTimeout(c, timeout);
}

/* Set a user provided RESP3 PUSH handler and return any old one set. */
redisPushFn *redisSetPushCallback(redisContext *c, redisPushFn *fn) {
    redisPushFn *old = c->push_cb;
    c->push_cb = fn;
    return old;
}

/* Use this function to handle a read event on the descriptor. It will try
 * and read some bytes from the socket and feed them to the reply parser.
 *
 * After this function is called, you may use redisGetReplyFromReader to
 * see if there is a reply available. */
int redisBufferRead(redisContext *c) {
    char buf[1024*16];
    int nread;

    /* Return early when the context has seen an error. */
    if (c->err)
        return REDIS_ERR;

    // 将结果读取到 buf 中
    // 可能一次性返回多个命令的返回
    nread = c->funcs->read(c, buf, sizeof(buf)); // redisNetRead
    if (nread < 0) {
        return REDIS_ERR;
    }
    // 读取成功,将消息存到 c->reader->buf 中
    if (nread > 0 && redisReaderFeed(c->reader, buf, nread) != REDIS_OK) {
        __redisSetError(c, c->reader->err, c->reader->errstr);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/* Write the output buffer to the socket.
 *
 * Returns REDIS_OK when the buffer is empty, or (a part of) the buffer was
 * successfully written to the socket. When the buffer is empty after the
 * write operation, "done" is set to 1 (if given).
 *
 * Returns REDIS_ERR if an unrecoverable error occurred in the underlying
 * c->funcs->write function.
 */
int redisBufferWrite(redisContext *c, int *done) {

    /* Return early when the context has seen an error. */
    if (c->err)
        return REDIS_ERR;
    // c->buf 存储的是格式化的命令(RESP) 如 1\r\n$4\r\nPING\r\n
    if (hi_sdslen(c->obuf) > 0) {
        // c->funcs->write = redisNetWrite
        ssize_t nwritten = c->funcs->write(c);
        if (nwritten < 0) {
            return REDIS_ERR;
        } else if (nwritten > 0) {
            if (nwritten == (ssize_t)hi_sdslen(c->obuf)) {
                hi_sdsfree(c->obuf);
                c->obuf = hi_sdsempty();
                if (c->obuf == NULL)
                    goto oom;
            } else {
                if (hi_sdsrange(c->obuf,nwritten,-1) < 0) goto oom;
            }
        }
    }
    // c->obuf 被清空了才设置 done 为 true
    // 如果 nwritten = 0, 说明读取未完成, 会等到下次再读取
    if (done != NULL) *done = (hi_sdslen(c->obuf) == 0);
    return REDIS_OK;

oom:
    __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
    return REDIS_ERR;
}

/* Internal helper that returns 1 if the reply was a RESP3 PUSH
 * message and we handled it with a user-provided callback. */
// PUSH 消息是 Redis 6.0 引入 RESP3 协议后新增的一种消息类型，
// 用于服务器主动向客户端推送信息，而不需要客户端先发送请求。
//      Pub/Sub 消息：订阅频道的消息推送
//      键空间通知：键发生变化时的通知
//      服务器事件：如客户端连接、断开等事件
static int redisHandledPushReply(redisContext *c, void *reply) {
    if (reply && c->push_cb && redisIsPushReply(reply)) {
        c->push_cb(c->privdata, reply);
        return 1;
    }

    return 0;
}

/* Get a reply from our reader or set an error in the context. */
int redisGetReplyFromReader(redisContext *c, void **reply) {
    if (redisReaderGetReply(c->reader, reply) == REDIS_ERR) {
        __redisSetError(c,c->reader->err,c->reader->errstr);
        return REDIS_ERR;
    }

    return REDIS_OK;
}

/* Internal helper to get the next reply from our reader while handling
 * any PUSH messages we encounter along the way.  This is separate from
 * redisGetReplyFromReader so as to not change its behavior. */
static int redisNextInBandReplyFromReader(redisContext *c, void **reply) {
    do {
        // 现在返回的消息还在 Reader 中, 现在写到 reply 中
        if (redisGetReplyFromReader(c, reply) == REDIS_ERR)
            return REDIS_ERR;
    } while (redisHandledPushReply(c, *reply));
    // redisHandledPushReply 若 reply 是 Push 类型的消息, 则可以继续循环, 默认是调用 redisPushAutoFree 将 reply 释放掉
    return REDIS_OK;
}

int redisGetReply(redisContext *c, void **reply) {
    int wdone = 0;
    void *aux = NULL;
    // "aux" 通常是 "auxiliary"（辅助的、辅助用的）的缩写

    /* Try to read pending replies */
    /*
     * 如使用 redisAppendCommand 一次性提交多个命令, 会返回多个结果
     * 每次调用 redisGetReply 获取一个结果
     * 第二次调用 redisGetReply 从缓存中获取结果,不需要访问服务端
     * 数据会写到 aux, aux!=NULL ,会直接返回
     *
     * 如果执行
     *      HELLO 3
     *      CLIENT TRACKING ON
     *      GET mykey   返回 nil
     *      SET key:0 val:0  返回  "+OK\r\n>2\r\n$10\r\invalidate\r\n*1\r\n$5\r\nkey:0\r\n"
     *          reply 中只会解析消息 ok, 剩余的 PUSH 消息等待下次执行命令是处理
     *      如果在再执行 GET key:0 且没有设置对应的 push_cb (push 回调函数)
     *      则 redisNextInBandReplyFromReader 会解析剩余的 PUSH 消息,并调用 redisPushAutoFree 将解析出来的消息销毁
     *      然后开始执行命令
     */
    if (redisNextInBandReplyFromReader(c,&aux) == REDIS_ERR)
        return REDIS_ERR;

    /* For the blocking context, flush output buffer and read reply */
    if (aux == NULL && c->flags & REDIS_BLOCK) {
        /* Write until done */
        do {
            // 发送命令到服务端
            if (redisBufferWrite(c,&wdone) == REDIS_ERR)
                return REDIS_ERR;
        } while (!wdone);

        /* Read until there is a reply */
        do {
            // 从服务端读取返回的消息,会放到 redisContext->redisReader->buf 中
            if (redisBufferRead(c) == REDIS_ERR)
                return REDIS_ERR;
            // 将读取的消息存到 aux
            if (redisNextInBandReplyFromReader(c,&aux) == REDIS_ERR)
                return REDIS_ERR;
        } while (aux == NULL);
    }

    /* Set reply or free it if we were passed NULL */
    if (reply != NULL) {
        *reply = aux;
    } else {
        freeReplyObject(aux);
    }

    return REDIS_OK;
}


/* Helper function for the redisAppendCommand* family of functions.
 *
 * Write a formatted command to the output buffer. When this family
 * is used, you need to call redisGetReply yourself to retrieve
 * the reply (or replies in pub/sub).
 *
 * 将格式化的命令存到 c->obuf
 * 这时候c->obuf 一般是空的,只有一个 cmd 命令
 * 但使用 redisAppendCommand 是,可以一次性发送多个命令
 *      redisAppendCommand(c, "SET key1 value1");
 *      redisAppendCommand(c, "SET key2 value2");
 * c->reader->buf 存储的是服务端的返回,是 append 的
 */
int __redisAppendCommand(redisContext *c, const char *cmd, size_t len) {
    hisds newbuf;

    newbuf = hi_sdscatlen(c->obuf,cmd,len);
    if (newbuf == NULL) {
        __redisSetError(c,REDIS_ERR_OOM,"Out of memory");
        return REDIS_ERR;
    }

    c->obuf = newbuf;
    return REDIS_OK;
}

int redisAppendFormattedCommand(redisContext *c, const char *cmd, size_t len) {

    if (__redisAppendCommand(c, cmd, len) != REDIS_OK) {
        return REDIS_ERR;
    }

    return REDIS_OK;
}

// 将用户的输入格式化为 RESP 字符串,存到 c->obuf 中
int redisvAppendCommand(redisContext *c, const char *format, va_list ap) {
    char *cmd;
    int len;
    // 将用户输入转为符合 redis 格式的命令 (RESP)
    len = redisvFormatCommand(&cmd,format,ap);
    if (len == -1) {
        __redisSetError(c,REDIS_ERR_OOM,"Out of memory");
        return REDIS_ERR;
    } else if (len == -2) {
        __redisSetError(c,REDIS_ERR_OTHER,"Invalid format string");
        return REDIS_ERR;
    }
    // 将命令字符串添加到 c 中
    if (__redisAppendCommand(c,cmd,len) != REDIS_OK) {
        hi_free(cmd);
        return REDIS_ERR;
    }

    hi_free(cmd);
    return REDIS_OK;
}

int redisAppendCommand(redisContext *c, const char *format, ...) {
    va_list ap;
    int ret;

    va_start(ap,format);
    ret = redisvAppendCommand(c,format,ap);
    va_end(ap);
    return ret;
}

int redisAppendCommandArgv(redisContext *c, int argc, const char **argv, const size_t *argvlen) {
    hisds cmd;
    long long len;

    len = redisFormatSdsCommandArgv(&cmd,argc,argv,argvlen);
    if (len == -1) {
        __redisSetError(c,REDIS_ERR_OOM,"Out of memory");
        return REDIS_ERR;
    }

    if (__redisAppendCommand(c,cmd,len) != REDIS_OK) {
        hi_sdsfree(cmd);
        return REDIS_ERR;
    }

    hi_sdsfree(cmd);
    return REDIS_OK;
}

/* Helper function for the redisCommand* family of functions.
 *
 * Write a formatted command to the output buffer. If the given context is
 * blocking, immediately read the reply into the "reply" pointer. When the
 * context is non-blocking, the "reply" pointer will not be used and the
 * command is simply appended to the write buffer.
 *
 * Returns the reply when a reply was successfully retrieved. Returns NULL
 * otherwise. When NULL is returned in a blocking context, the error field
 * in the context will be set.
 */
static void *__redisBlockForReply(redisContext *c) {
    void *reply;

    // 检查上下文是否设置了阻塞标志。
    if (c->flags & REDIS_BLOCK) {
        // 调用 redisGetReply 函数等待服务器响应
        if (redisGetReply(c,&reply) != REDIS_OK)
            return NULL;
        return reply;
    }
    return NULL;
}

// 这里的v表示 variadic, 即"可变参数"的意思
// 带有 v 后缀的函数通常表示使用 va_list 参数的版本
void *redisvCommand(redisContext *c, const char *format, va_list ap) {
    if (redisvAppendCommand(c,format,ap) != REDIS_OK)
        return NULL;
    return __redisBlockForReply(c);
}

// mac m4 下 redisCommand 汇编
// x29 指向当前函数的栈帧基址
// x30 (链接寄存器/Link Register),函数调用时自动保存返回地址
// redisCommand:
//     sub    sp, sp, #0x30   ; 分配48字节栈空间
//     stp    x29, x30, [sp, #0x20] ;将 x29 的值存储到地址 sp + 0x20, 将 x30 的值存储到地址 sp + 0x28
//     add    x29, sp, #0x20 ; 设置新的帧指针
//     stur   x0, [x29, #-0x8] ; 保存 context 参数 (redisContext*)
//     str    x1, [sp, #0x10] ; 保存 format 参数 (const char*)
//     add    x9, sp, #0x8 ; x9 = argv数组存储位置
//     add    x8, x29, #0x10 ;x8 = 参数存储区起始地址
//     str    x8, [x9]  ; 构建argv[0]指向参数区
//     ldur   x0, [x29, #-0x8] ; 重新加载 context 参数
//     ldr    x1, [sp, #0x10] ; 重新加载 format 参数
//     ldr    x2, [sp, #0x8] ; 加载 va_list (argv数组指针)
//     bl     0x100ddab48                ; redisvCommand at hiredis.c:1216
//     str    x0, [sp] ; 保存返回值
//     ldr    x0, [sp] ; 重新加载返回值到x0 (准备返回)
//     ldp    x29, x30, [sp, #0x20] ; 恢复帧指针和返回地址
//     add    sp, sp, #0x30 ; 恢复栈指针
//     ret
//
// 高地址
// +------------------+ ← 调用者栈帧
// | 返回地址         |
// +------------------+ ← 原始sp (函数调用前)
// | 保留空间         | 0x30-0x28
// +------------------+
// | 保存的x30        | 0x28 [sp+0x20+8]
// +------------------+
// | 保存的x29        | 0x20 [sp+0x20]
// +------------------+
// | context备份      | 0x18 [x29-0x8]
// +------------------+
// | format备份       | 0x10 [sp+0x10]
// +------------------+
// | argv数组指针     | 0x08 [sp+0x8]
// +------------------+
// | 保留空间         | 0x00 [sp]
// +------------------+ ← 当前sp (函数调用后)
// 低地址

// 调用函数时, 会将参数放入栈中
void *redisCommand(redisContext *c, const char *format, ...) {
    va_list ap;
    // 初始化 va_list 变量 ap
    // 让 ap 指向第一个可变参数在栈中的位置
    va_start(ap,format);
    void *reply = redisvCommand(c,format,ap);
    va_end(ap);
    return reply;
}

void *redisCommandArgv(redisContext *c, int argc, const char **argv, const size_t *argvlen) {
    if (redisAppendCommandArgv(c,argc,argv,argvlen) != REDIS_OK)
        return NULL;
    return __redisBlockForReply(c);
}

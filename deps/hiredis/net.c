/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
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
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>

#include "net.h"
#include "sds.h"
#include "sockcompat.h"
#include "win32.h"

/* Defined in hiredis.c */
void __redisSetError(redisContext *c, int type, const char *str);

int redisContextUpdateCommandTimeout(redisContext *c, const struct timeval *timeout);

void redisNetClose(redisContext *c) {
    if (c && c->fd != REDIS_INVALID_FD) {
        close(c->fd);
        c->fd = REDIS_INVALID_FD;
    }
}

ssize_t redisNetRead(redisContext *c, char *buf, size_t bufcap) {
    // redisContextSetTimeout 中有设置 c->fd 的超时行为
    ssize_t nread = recv(c->fd, buf, bufcap, 0);
    if (nread == -1) {
        // EWOULDBLOCK 表示操作会阻塞，但套接字被设置为非阻塞模式，因此操作无法立即完成。
        if ((errno == EWOULDBLOCK && !(c->flags & REDIS_BLOCK)) || (errno == EINTR)) {
            /* Try again later */
            return 0;
        } else if(errno == ETIMEDOUT && (c->flags & REDIS_BLOCK)) {
            /* especially in windows */
            __redisSetError(c, REDIS_ERR_TIMEOUT, "recv timeout");
            return -1;
        } else {
            __redisSetError(c, REDIS_ERR_IO, strerror(errno));
            return -1;
        }
    } else if (nread == 0) {
        __redisSetError(c, REDIS_ERR_EOF, "Server closed the connection");
        return -1;
    } else {
        return nread;
    }
}

ssize_t redisNetWrite(redisContext *c) {
    ssize_t nwritten;

    nwritten = send(c->fd, c->obuf, hi_sdslen(c->obuf), 0);
    if (nwritten < 0) {
        if ((errno == EWOULDBLOCK && !(c->flags & REDIS_BLOCK)) || (errno == EINTR)) {
            /* Try again */
            return 0;
        } else {
            __redisSetError(c, REDIS_ERR_IO, strerror(errno));
            return -1;
        }
    }

    return nwritten;
}

static void __redisSetErrorFromErrno(redisContext *c, int type, const char *prefix) {
    int errorno = errno;  /* snprintf() may change errno */
    char buf[128] = { 0 };
    size_t len = 0;

    if (prefix != NULL)
        len = snprintf(buf,sizeof(buf),"%s: ",prefix);
    // 线程安全的错误消息转换函数
    // 将 errno 转换为对应的错误描述字符串
    // 将结果写入指定的缓冲区位置
    strerror_r(errorno, (char *)(buf + len), sizeof(buf) - len);
    __redisSetError(c,type,buf);
}

static int redisSetReuseAddr(redisContext *c) {
    int on = 1;
    if (setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        redisNetClose(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

static int redisCreateSocket(redisContext *c, int type) {
    redisFD s;
    if ((s = socket(type, SOCK_STREAM, 0)) == REDIS_INVALID_FD) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }
    c->fd = s;
    if (type == AF_INET) {
        if (redisSetReuseAddr(c) == REDIS_ERR) {
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

// 用于设置 Redis 连接套接字的阻塞/非阻塞模式
static int redisSetBlocking(redisContext *c, int blocking) {
#ifndef _WIN32
    int flags;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    // 获取当前套接字标志
    if ((flags = fcntl(c->fd, F_GETFL)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_GETFL)");
        redisNetClose(c);
        return REDIS_ERR;
    }

    // 修改标志位
    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    // 应用新标志
    if (fcntl(c->fd, F_SETFL, flags) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"fcntl(F_SETFL)");
        redisNetClose(c);
        return REDIS_ERR;
    }
#else
    u_long mode = blocking ? 0 : 1;
    if (ioctl(c->fd, FIONBIO, &mode) == -1) {
        __redisSetErrorFromErrno(c, REDIS_ERR_IO, "ioctl(FIONBIO)");
        redisNetClose(c);
        return REDIS_ERR;
    }
#endif /* _WIN32 */
    return REDIS_OK;
}

int redisKeepAlive(redisContext *c, int interval) {
    int val = 1;
    redisFD fd = c->fd;

#ifndef _WIN32
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1){
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = interval;

#if defined(__APPLE__) && defined(__MACH__)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }
#else
#if defined(__GLIBC__) && !defined(__FreeBSD_kernel__)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = interval/3;
    if (val == 0) val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }

    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        __redisSetError(c,REDIS_ERR_OTHER,strerror(errno));
        return REDIS_ERR;
    }
#endif
#endif
#else
    int res;

    res = win32_redisKeepAlive(fd, interval * 1000);
    if (res != 0) {
        __redisSetError(c, REDIS_ERR_OTHER, strerror(res));
        return REDIS_ERR;
    }
#endif
    return REDIS_OK;
}

int redisSetTcpNoDelay(redisContext *c) {
    int yes = 1;
    if (setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(TCP_NODELAY)");
        redisNetClose(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

int redisContextSetTcpUserTimeout(redisContext *c, unsigned int timeout) {
    int res;
#ifdef TCP_USER_TIMEOUT
    res = setsockopt(c->fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, sizeof(timeout));
#else
    res = -1;
    errno = ENOTSUP;
    (void)timeout;
#endif
    if (res == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(TCP_USER_TIMEOUT)");
        redisNetClose(c);
        return REDIS_ERR;
    }
    return REDIS_OK;
}

#define __MAX_MSEC (((LONG_MAX) - 999) / 1000)

static int redisContextTimeoutMsec(redisContext *c, long *result)
{
    const struct timeval *timeout = c->connect_timeout;
    long msec = -1;

    /* Only use timeout when not NULL. */
    if (timeout != NULL) {
        if (timeout->tv_usec > 1000000 || timeout->tv_sec > __MAX_MSEC) {
            __redisSetError(c, REDIS_ERR_IO, "Invalid timeout specified");
            *result = msec;
            return REDIS_ERR;
        }

        msec = (timeout->tv_sec * 1000) + ((timeout->tv_usec + 999) / 1000);

        if (msec < 0 || msec > INT_MAX) {
            msec = INT_MAX;
        }
    }

    *result = msec;
    return REDIS_OK;
}

static int redisContextWaitReady(redisContext *c, long msec) {
    struct pollfd   wfd[1];

    wfd[0].fd     = c->fd;
    wfd[0].events = POLLOUT;

    if (errno == EINPROGRESS) {
        int res;

        if ((res = poll(wfd, 1, msec)) == -1) {
            __redisSetErrorFromErrno(c, REDIS_ERR_IO, "poll(2)");
            redisNetClose(c);
            return REDIS_ERR;
        } else if (res == 0) {
            errno = ETIMEDOUT;
            __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
            redisNetClose(c);
            return REDIS_ERR;
        }

        if (redisCheckConnectDone(c, &res) != REDIS_OK || res == 0) {
            redisCheckSocketError(c);
            return REDIS_ERR;
        }

        return REDIS_OK;
    }

    __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
    redisNetClose(c);
    return REDIS_ERR;
}

int redisCheckConnectDone(redisContext *c, int *completed) {
    int rc = connect(c->fd, (const struct sockaddr *)c->saddr, c->addrlen);
    if (rc == 0) {
        *completed = 1;
        return REDIS_OK;
    }
    int error = errno;
    if (error == EINPROGRESS) {
        /* must check error to see if connect failed.  Get the socket error */
        int fail, so_error;
        socklen_t optlen = sizeof(so_error);
        fail = getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen);
        if (fail == 0) {
            if (so_error == 0) {
                /* Socket is connected! */
                *completed = 1;
                return REDIS_OK;
            }
            /* connection error; */
            errno = so_error;
            error = so_error;
        }
    }
    switch (error) {
    case EISCONN:
        *completed = 1;
        return REDIS_OK;
    case EALREADY:
    case EWOULDBLOCK:
        *completed = 0;
        return REDIS_OK;
    default:
        return REDIS_ERR;
    }
}

int redisCheckSocketError(redisContext *c) {
    int err = 0, errno_saved = errno;
    socklen_t errlen = sizeof(err);

    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"getsockopt(SO_ERROR)");
        return REDIS_ERR;
    }

    if (err == 0) {
        err = errno_saved;
    }

    if (err) {
        errno = err;
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }

    return REDIS_OK;
}

int redisContextSetTimeout(redisContext *c, const struct timeval tv) {
    const void *to_ptr = &tv;
    size_t to_sz = sizeof(tv);

    if (redisContextUpdateCommandTimeout(c, &tv) != REDIS_OK) {
        __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
        return REDIS_ERR;
    }

    // SO_RCVTIMEO (Receive Timeout) 设置接收数据操作的超时时间
    if (setsockopt(c->fd,SOL_SOCKET,SO_RCVTIMEO,to_ptr,to_sz) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_RCVTIMEO)");
        return REDIS_ERR;
    }

    // SO_SNDTIMEO (Send Timeout) 作用：设置发送数据操作的超时时间
    if (setsockopt(c->fd,SOL_SOCKET,SO_SNDTIMEO,to_ptr,to_sz) == -1) {
        __redisSetErrorFromErrno(c,REDIS_ERR_IO,"setsockopt(SO_SNDTIMEO)");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

int redisContextUpdateConnectTimeout(redisContext *c, const struct timeval *timeout) {
    /* Same timeval struct, short circuit
     * 如果指针相同,直接返回
     * 和 _redisContextConnectTcp 中 (c->tcp.host != addr) 的原理相同
     */
    if (c->connect_timeout == timeout)
        return REDIS_OK;

    /* Allocate context timeval if we need to */
    if (c->connect_timeout == NULL) {
        c->connect_timeout = hi_malloc(sizeof(*c->connect_timeout));
        if (c->connect_timeout == NULL)
            return REDIS_ERR;
    }

    memcpy(c->connect_timeout, timeout, sizeof(*c->connect_timeout));
    return REDIS_OK;
}

int redisContextUpdateCommandTimeout(redisContext *c, const struct timeval *timeout) {
    /* Same timeval struct, short circuit */
    if (c->command_timeout == timeout)
        return REDIS_OK;

    /* Allocate context timeval if we need to */
    if (c->command_timeout == NULL) {
        c->command_timeout = hi_malloc(sizeof(*c->command_timeout));
        if (c->command_timeout == NULL)
            return REDIS_ERR;
    }

    memcpy(c->command_timeout, timeout, sizeof(*c->command_timeout));
    return REDIS_OK;
}

static int _redisContextConnectTcp(redisContext *c, const char *addr, int port,
                                   const struct timeval *timeout,
                                   const char *source_addr) {
    // 文件描述符
    redisFD s;
    int rv, n;
    char _port[6];  /* strlen("65535"); */
    // ervinfo: 用于 DNS 查找的结果
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;
    // blocking: 是否为阻塞模式
    int blocking = (c->flags & REDIS_BLOCK);
    // reuseaddr: 是否启用地址重用
    int reuseaddr = (c->flags & REDIS_REUSEADDR);
    int reuses = 0;
    long timeout_msec = -1;

    servinfo = NULL;
    c->connection_type = REDIS_CONN_TCP;
    c->tcp.port = port;

    /* We need to take possession of the passed parameters
     * to make them reusable for a reconnect.
     * We also carefully check we don't free data we already own,
     * as in the case of the reconnect method.
     *
     * This is a bit ugly, but at least it works and doesn't leak memory.
     *
     * 如果用 redisReconnect(c) 进行重连, 传入的 *addr 参数就是 c->tcp.host
     * 这时 c->tcp.host 和 *addr 地址是相同的
     * c->tcp.host != addr 避免释放 c->tcp.host
     */
    if (c->tcp.host != addr) {
        hi_free(c->tcp.host);

        c->tcp.host = hi_strdup(addr);
        if (c->tcp.host == NULL)
            goto oom;
    }

    if (timeout) {
        if (redisContextUpdateConnectTimeout(c, timeout) == REDIS_ERR)
            goto oom;
    } else {
        hi_free(c->connect_timeout);
        c->connect_timeout = NULL;
    }

    if (redisContextTimeoutMsec(c, &timeout_msec) != REDIS_OK) {
        goto error;
    }

    // 如果指定了源地址，则保存它用于绑定本地套接字。
    if (source_addr == NULL) {
        hi_free(c->tcp.source_addr);
        c->tcp.source_addr = NULL;
    } else if (c->tcp.source_addr != source_addr) {
        hi_free(c->tcp.source_addr);
        c->tcp.source_addr = hi_strdup(source_addr);
    }

    snprintf(_port, 6, "%d", port);
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_INET;
    // 流套接字（TCP）
    hints.ai_socktype = SOCK_STREAM;

    /* DNS lookup. To use dual stack, set both flags to prefer both IPv4 and
     * IPv6. By default, for historical reasons, we try IPv4 first and then we
     * try IPv6 only if no IPv4 address was found. */
    if (c->flags & REDIS_PREFER_IPV6 && c->flags & REDIS_PREFER_IPV4)
        hints.ai_family = AF_UNSPEC;
    else if (c->flags & REDIS_PREFER_IPV6)
        hints.ai_family = AF_INET6;
    else
        hints.ai_family = AF_INET;

    // getaddrinfo 根据指定的主机名和服务名,返回一个 struct addrinfo 结构体链表
    //    int getaddrinfo(const char *node,     // 主机名或IP地址
    //    const char *service,  // 服务名或端口号
    //    const struct addrinfo *hints,   // 输入参数，指定查询条件
    //    struct addrinfo **res);         // 输出参数，返回结果链表
    rv = getaddrinfo(c->tcp.host, _port, &hints, &servinfo);
    if (rv != 0 && hints.ai_family != AF_UNSPEC) {
        /* Try again with the other IP version. */
        hints.ai_family = (hints.ai_family == AF_INET) ? AF_INET6 : AF_INET;
        rv = getaddrinfo(c->tcp.host, _port, &hints, &servinfo);
    }
    if (rv != 0) {
        __redisSetError(c, REDIS_ERR_OTHER, gai_strerror(rv));
        return REDIS_ERR;
    }
    // 遍历所有解析到的地址，尝试建立连接
    for (p = servinfo; p != NULL; p = p->ai_next) {
addrretry:
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == REDIS_INVALID_FD)
            continue;
        // 客户端 fd
        c->fd = s;
        if (redisSetBlocking(c,0) != REDIS_OK)
            goto error;

        // 源地址,客户端的地址
        // 正常连接不指定源地址
        // 如果是多网卡环境或者防火墙限制,可以指定源地址
        // redisContext *c = redisConnectBindNonBlock("192.168.1.100", 6379, "10.0.0.100");
        if (c->tcp.source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            // 解析源地址
            if ((rv = getaddrinfo(c->tcp.source_addr, NULL, &hints, &bservinfo)) != 0) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't get addr: %s",gai_strerror(rv));
                __redisSetError(c,REDIS_ERR_OTHER,buf);
                goto error;
            }
            // 设置地址重用
            if (reuseaddr) {
                n = 1;
                if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*) &n,
                               sizeof(n)) < 0) {
                    freeaddrinfo(bservinfo);
                    goto error;
                }
            }
            // 尝试绑定到指定源地址
            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s,b->ai_addr,b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
            if (!bound) {
                char buf[128];
                snprintf(buf,sizeof(buf),"Can't bind socket: %s",strerror(errno));
                __redisSetError(c,REDIS_ERR_OTHER,buf);
                goto error;
            }
        }

        /* For repeat connection
         * 如果是重新连接 c->saddr 指向已分配的内存空间, 所以需要先释放
         * p->ai_addrlen 是 getaddrinfo 新解析出来的,地址可能与重新连接之前的不同
         * 所以要用新地址
         */
        hi_free(c->saddr);
        c->saddr = hi_malloc(p->ai_addrlen);
        if (c->saddr == NULL)
            goto oom;
        // getaddrinfo 会解析用户指定IP,将解析的结果放在 servinfo 的链表中
        // p 是其中链表中的一个节点
        // 这段处理还在 for 循环中
        memcpy(c->saddr, p->ai_addr, p->ai_addrlen);
        c->addrlen = p->ai_addrlen;

        // 连接建立
        if (connect(s,p->ai_addr,p->ai_addrlen) == -1) {
            if (errno == EHOSTUNREACH) {
                // 主机不可达，尝试下一个地址
                redisNetClose(c);
                continue;
            } else if (errno == EINPROGRESS) {
                // 非阻塞连接正在进行
                if (blocking) {
                    goto wait_for_ready;
                }
                /* This is ok.
                 * Note that even when it's in blocking mode, we unset blocking
                 * for `connect()`
                 */
            } else if (errno == EADDRNOTAVAIL && reuseaddr) {
                // 地址不可用且启用了地址重用
                if (++reuses >= REDIS_CONNECT_RETRIES) {
                    goto error;
                } else {
                    redisNetClose(c);
                    goto addrretry;
                }
            } else {
                wait_for_ready:
                // 等待连接完成
                if (redisContextWaitReady(c,timeout_msec) != REDIS_OK)
                    goto error;
                if (redisSetTcpNoDelay(c) != REDIS_OK)
                    goto error;
            }
        }
        // 连接完成处理
        if (blocking && redisSetBlocking(c,1) != REDIS_OK)
            goto error;

        c->flags |= REDIS_CONNECTED;
        rv = REDIS_OK;
        goto end;
    }
    // 只要有一个连接成功就会跳转到 end ,不会执行到这里
    if (p == NULL) {
        char buf[128];
        snprintf(buf,sizeof(buf),"Can't create socket: %s",strerror(errno));
        __redisSetError(c,REDIS_ERR_OTHER,buf);
        goto error;
    }

oom:
    __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
error:
    rv = REDIS_ERR;
end:
    if(servinfo) {
        freeaddrinfo(servinfo);
    }

    return rv;  // Need to return REDIS_OK if alright
}

int redisContextConnectTcp(redisContext *c, const char *addr, int port,
                           const struct timeval *timeout) {
    return _redisContextConnectTcp(c, addr, port, timeout, NULL);
}

int redisContextConnectBindTcp(redisContext *c, const char *addr, int port,
                               const struct timeval *timeout,
                               const char *source_addr) {
    return _redisContextConnectTcp(c, addr, port, timeout, source_addr);
}

int redisContextConnectUnix(redisContext *c, const char *path, const struct timeval *timeout) {
#ifndef _WIN32
    int blocking = (c->flags & REDIS_BLOCK);
    struct sockaddr_un *sa;
    long timeout_msec = -1;

    if (redisCreateSocket(c,AF_UNIX) < 0)
        return REDIS_ERR;
    if (redisSetBlocking(c,0) != REDIS_OK)
        return REDIS_ERR;

    c->connection_type = REDIS_CONN_UNIX;
    if (c->unix_sock.path != path) {
        hi_free(c->unix_sock.path);

        c->unix_sock.path = hi_strdup(path);
        if (c->unix_sock.path == NULL)
            goto oom;
    }

    if (timeout) {
        if (redisContextUpdateConnectTimeout(c, timeout) == REDIS_ERR)
            goto oom;
    } else {
        hi_free(c->connect_timeout);
        c->connect_timeout = NULL;
    }

    if (redisContextTimeoutMsec(c,&timeout_msec) != REDIS_OK)
        return REDIS_ERR;

    /* Don't leak sockaddr if we're reconnecting */
    if (c->saddr) hi_free(c->saddr);

    sa = (struct sockaddr_un*)(c->saddr = hi_malloc(sizeof(struct sockaddr_un)));
    if (sa == NULL)
        goto oom;

    c->addrlen = sizeof(struct sockaddr_un);
    sa->sun_family = AF_UNIX;
    strncpy(sa->sun_path, path, sizeof(sa->sun_path) - 1);
    if (connect(c->fd, (struct sockaddr*)sa, sizeof(*sa)) == -1) {
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
            if (redisContextWaitReady(c,timeout_msec) != REDIS_OK)
                return REDIS_ERR;
        }
    }

    /* Reset socket to be blocking after connect(2). */
    if (blocking && redisSetBlocking(c,1) != REDIS_OK)
        return REDIS_ERR;

    c->flags |= REDIS_CONNECTED;
    return REDIS_OK;
#else
    /* We currently do not support Unix sockets for Windows. */
    /* TODO(m): https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/ */
    errno = EPROTONOSUPPORT;
    return REDIS_ERR;
#endif /* _WIN32 */
oom:
    __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
    return REDIS_ERR;
}

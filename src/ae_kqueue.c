/* Kqueue(2)-based ae.c module
 *
 * Copyright (C) 2009 Harish Mallipeddi - harish.mallipeddi@gmail.com
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


#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

typedef struct aeApiState {
    // 创建 kqueue 实例时返回的 fd
    int kqfd;
    // 存储就绪事件的数组
    struct kevent *events;

    /* Events mask for merge read and write event.
     * To reduce memory consumption, we use 2 bits to store the mask
     * of an event, so that 1 byte will store the mask of 4 events.
     * 为了节省内存，使用2个bit存储一个事件的掩码，这样1个字节可以存储4个事件的掩码
     */
    char *eventsMask; 
} aeApiState;

#define EVENT_MASK_MALLOC_SIZE(sz) (((sz) + 3) / 4)
#define EVENT_MASK_OFFSET(fd) ((fd) % 4 * 2)
#define EVENT_MASK_ENCODE(fd, mask) (((mask) & 0x3) << EVENT_MASK_OFFSET(fd))

static inline int getEventMask(const char *eventsMask, int fd) {
    return (eventsMask[fd/4] >> EVENT_MASK_OFFSET(fd)) & 0x3;
}

static inline void addEventMask(char *eventsMask, int fd, int mask) {
    eventsMask[fd/4] |= EVENT_MASK_ENCODE(fd, mask);
}

static inline void resetEventMask(char *eventsMask, int fd) {
    eventsMask[fd/4] &= ~EVENT_MASK_ENCODE(fd, 0x3);
}

// 初始化 kqueue 实例
// 初始化 aeApiState, 其中中保存了 kevent 实例, events 数组等
// 然后赋值给 eventLoop
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    // 分配存储 kevent 事件的数组
    //  (sizeof(struct kevent))  * (eventLoop->setsize)
    // 用于存储 kevent() 系统调用返回的就绪事件
    state->events = zmalloc(sizeof(struct kevent)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    // 初始化 kqueue 实例
    state->kqfd = kqueue();
    if (state->kqfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    // 设置 kqueue 文件描述符的 FD_CLOEXEC 标志
    // 防止在 exec 系列函数调用时文件描述符泄露给子进程
    anetCloexec(state->kqfd);
    state->eventsMask = zmalloc(EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct kevent)*setsize);
    state->eventsMask = zrealloc(state->eventsMask, EVENT_MASK_MALLOC_SIZE(setsize));
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(setsize));
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->kqfd);
    zfree(state->events);
    zfree(state->eventsMask);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    if (tvp != NULL) {
        struct timespec timeout;
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        // timeout = {0, 0}（立即返回),轮询模式，检查是否有已就绪事件
        // timeout = {seconds, nanoseconds}（限时等待）
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,
                        &timeout);
    } else {
        // 等待事件发生 timeout = NULL（无限等待）,直到有事件发生
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,
                        NULL);
    }

    if (retval > 0) {
        int j;

        /* Normally we execute the read event first and then the write event.
         * When the barrier is set, we will do it reverse.
         * 
         * However, under kqueue, read and write events would be separate
         * events, which would make it impossible to control the order of
         * reads and writes. So we store the event's mask we've got and merge
         * the same fd events later.
         *
         * 假设 fd=5 同时可读可写
         *   kqueue 可能返回两个独立的 kevent 结构：
         *   kevent1: {ident=5, filter=EVFILT_READ, ...}   // 可读事件
         *   kevent2: {ident=5, filter=EVFILT_WRITE, ...}  // 可写事件
         */
        for (j = 0; j < retval; j++) {
            struct kevent *e = state->events+j;
            int fd = e->ident;
            int mask = 0; 

            if (e->filter == EVFILT_READ) mask = AE_READABLE;
            else if (e->filter == EVFILT_WRITE) mask = AE_WRITABLE;
            // 合并掩码
            // 如果不合并，Redis 会分别处理这两个事件，可能导致
            //     同一个 fd 被处理两次
            //     无法控制读写事件的处理顺序
            //     无法正确处理 AE_BARRIER 标志
            // eventsMask 是一个数组，修改相同 fd 的值
            addEventMask(state->eventsMask, fd, mask);
        }

        /* Re-traversal to merge read and write events, and set the fd's mask to
         * 0 so that events are not added again when the fd is encountered again. */
        numevents = 0;
        for (j = 0; j < retval; j++) {
            struct kevent *e = state->events+j;
            int fd = e->ident;
            // 获取合并后的掩码
            int mask = getEventMask(state->eventsMask, fd);

            if (mask) {
                eventLoop->fired[numevents].fd = fd;
                // 包含读写事件的合并掩码
                eventLoop->fired[numevents].mask = mask;
                // 清除已处理的掩码
                // 当下次相同的 fd 执行  getEventMask(state->eventsMask, fd) 时
                // 获取的是空的，故不会再次进入
                resetEventMask(state->eventsMask, fd);
                numevents++;
            }
        }
    } else if (retval == -1 && errno != EINTR) {
        panic("aeApiPoll: kevent, %s", strerror(errno));
    }
    // 合并读写事件后实际的事件数
    return numevents;
}

static char *aeApiName(void) {
    return "kqueue";
}

/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "ae.h"
#include "anet.h"
#include "redisassert.h"

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "zmalloc.h"
#include "config.h"
#include "debugmacro.h"

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
    #ifdef HAVE_EPOLL
    #include "ae_epoll.c"
    #else
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.c"
        #else
        #include "ae_select.c"
        #endif
    #endif
#endif

#define INITIAL_EVENT 1024
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    monotonicInit();    /* just in case the calling app didn't initialize */
    // 初始化事件循环结构体
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
    eventLoop->nevents = setsize < INITIAL_EVENT ? setsize : INITIAL_EVENT;
    // 初始化文件事件结构和已就绪文件事件结构数组
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*eventLoop->nevents);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*eventLoop->nevents);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    // 设置数组大小
    eventLoop->setsize = setsize;
    // 初始化时间事件结构
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
    eventLoop->aftersleep = NULL;
    eventLoop->flags = 0;
    memset(eventLoop->privdata, 0, sizeof(eventLoop->privdata));
    // 创建 epoll, kqueue 实例
    if (aeApiCreate(eventLoop) == -1) goto err;
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it.
     * 初始化监听事件  文件事件状态为 AE_NONE
     */
    for (i = 0; i < eventLoop->nevents; i++)
        eventLoop->events[i].mask = AE_NONE;
    // 返回事件循环
    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* Return the current set size. */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/*
 * Tell the event processing to change the wait timeout as soon as possible.
 *
 * Note: it just means you turn on/off the global AE_DONT_WAIT.
 */
void aeSetDontWait(aeEventLoop *eventLoop, int noWait) {
    if (noWait)
        eventLoop->flags |= AE_DONT_WAIT;
    else
        eventLoop->flags &= ~AE_DONT_WAIT;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    if (setsize == eventLoop->setsize) return AE_OK;
    if (eventLoop->maxfd >= setsize) return AE_ERR;
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;

    eventLoop->setsize = setsize;

    /* If the current allocated space is larger than the requested size,
     * we need to shrink it to the requested size. */
    if (setsize < eventLoop->nevents) {
        eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
        eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
        eventLoop->nevents = setsize;
    }
    return AE_OK;
}

void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);

    /* Free the time events list. */
    aeTimeEvent *next_te, *te = eventLoop->timeEventHead;
    while (te) {
        next_te = te->next;
        if (te->finalizerProc)
            te->finalizerProc(eventLoop, te->clientData);
        zfree(te);
        te = next_te;
    }
    zfree(eventLoop);
}

void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

// 在事件循环中注册一个文件事件监听器，
// 当指定的文件描述符(fd)上发生特定类型的I/O事件(可读/可写)时，会调用相应的处理函数
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    // 检查文件描述符是否超出了事件循环的最大容
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }

    /* Resize the events and fired arrays if the file
     * descriptor exceeds the current number of events. */
    if (unlikely(fd >= eventLoop->nevents)) {
        int newnevents = eventLoop->nevents;
        // 如果当前容量的2倍大于fd+1，则新容量为当前容量的2倍, 否则新容量为fd+1
        // 当前 nevents = 16
        //      fd = 18 , newnevents = 32
        //      fd = 33 , newnevents = 34
        newnevents = (newnevents * 2 > fd + 1) ? newnevents * 2 : fd + 1;
        // 不能超过eventLoop->setsize
        newnevents = (newnevents > eventLoop->setsize) ? eventLoop->setsize : newnevents;
        eventLoop->events = zrealloc(eventLoop->events, sizeof(aeFileEvent) * newnevents);
        eventLoop->fired = zrealloc(eventLoop->fired, sizeof(aeFiredEvent) * newnevents);

        /* Initialize new slots with an AE_NONE mask */
        /* 将新分配的事件槽位初始化为 AE_NONE（无事件监听） */
        for (int i = eventLoop->nevents; i < newnevents; i++)
            eventLoop->events[i].mask = AE_NONE;
        eventLoop->nevents = newnevents;
    }

    // 获取对应文件描述符的事件结构
    D_CONSOLE("eventLoop->events[%d]",fd);
    // redisAeEvents 是在客户端定义的结构体, ae.c 是服务端代码
    // 所以 clientData 代码无法转换为 redisAeEvents, clientData 是客户端产生的数据
    // ((redisAeEvents*)clientData)->context->c.obuf
    aeFileEvent *fe = &eventLoop->events[fd];
    // 调用底层API（如epoll、kqueue等）注册事件
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
    fe->mask |= mask;
    // 根据事件类型设置相应的处理函数,这里不会执行
    // aeProcessEvents 中根据触发的事件类型执行
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    // 保存用户数据
    // clientData 即 redisAeEvents
    fe->clientData = clientData;
    // 更新事件循环中最大的文件描述符，用于优化事件轮询
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

/*
 * aeFileEvent 是连续的数组,通过索引来访问
 *          0             1             2           3
    ┌─────────────┐─────────────┐─────────────┐─────────────┐
    │ mask        │ mask        │ mask        │ mask        │
    │ *rfileProc  │ *rfileProc  │ *rfileProc  │ *rfileProc  │
    │ *wfileProc  │ *wfileProc  │ *wfileProc  │ *wfileProc  │
    │ *clientData │ *clientData │ *clientData │ *clientData │
    └─────────────┘─────────────┘─────────────┘─────────────┘
 *
 *
 */

void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    if (fd >= eventLoop->setsize) return;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return;

    /* We want to always remove AE_BARRIER if set when AE_WRITABLE
     * is removed. */
    if (mask & AE_WRITABLE) mask |= AE_BARRIER;

    aeApiDelEvent(eventLoop, fd, mask);
    fe->mask = fe->mask & (~mask);
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}

void *aeGetFileClientData(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return NULL;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return NULL;

    return fe->clientData;
}

int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}

long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
    // 更新时间计数器
    long long id = eventLoop->timeEventNextId++;
    // 创建时间事件结构
    aeTimeEvent *te;
    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    // 设定处理事件的时间
    te->when = getMonotonicUs() + milliseconds * 1000;
    // 设置时间事件处理器
    te->timeProc = proc;
    // 事件释放函数
    te->finalizerProc = finalizerProc;
    // 设置私有数据
    te->clientData = clientData;
    // 头节点的 prev 总是 NULL
    te->prev = NULL;
    te->next = eventLoop->timeEventHead;
    te->refcount = 0;
    if (te->next)
        te->next->prev = te;
    // 修改头指针指向
    eventLoop->timeEventHead = te;
    return id;
}

/*
head (eventLoop->timeEventHead)
  ↓
┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│   Event 1   │    │   Event 2   │    │   Event 3   │    │   Event N   │
│             │    │             │    │             │    │             │
│ id: 1       │    │ id: 2       │    │ id: 3       │    │ id: N       │
│ when: 1000  │    │ when: 1500  │    │ when: 2000  │    │ when: 5000  │
│ timeProc    │    │ timeProc    │    │ timeProc    │    │ timeProc    │
│ finalizer   │    │ finalizer   │    │ finalizer   │    │ finalizer   │
│ clientData  │    │ clientData  │    │ clientData  │    │ clientData  │
│ refcount: 0 │    │ refcount: 0 │    │ refcount: 0 │    │ refcount: 0 │
│             │    │             │    │             │    │             │
│ prev: NULL  │←───│ prev: ──────│←───│ prev: ──────│←───│ prev: ──────│
│ next: ──────│───→│ next: ──────│───→│ next: ──────│───→│ next: NULL  │
└─────────────┘    └─────────────┘    └─────────────┘    └─────────────┘

链表特点：
1. 单向链表（通过 next 指针）
2. 可以反向遍历（通过 prev 指针）
3. 头节点的 prev 为 NULL
4. 尾节点的 next 为 NULL
5. 新事件总是插入到链表头部
*/


int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    while(te) {
        if (te->id == id) {
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* How many microseconds until the first timer should fire.
 * If there are no timers, -1 is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 *
 * 计算距离下一个最早触发的定时器事件还需要等待的时间（微秒）。
 */
static int64_t usUntilEarliestTimer(aeEventLoop *eventLoop) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    // 没有定时器事件，返回 -1
    if (te == NULL) return -1;

    // 遍历所有定时器事件找到最早触发的
    aeTimeEvent *earliest = NULL;
    while (te) {
        if ((!earliest || te->when < earliest->when) && te->id != AE_DELETED_EVENT_ID)
            earliest = te;
        te = te->next;
    }

    monotime now = getMonotonicUs();
    // 如果已经到了触发时间，返回 0（不需要等待）
    // 否则返回还需要等待的微秒数
    return (now >= earliest->when) ? 0 : earliest->when - now;
}

/* Process time events */
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
    monotime now = getMonotonicUs();
    while(te) {
        long long id;

        /* Remove events scheduled for deletion. */
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            /* If a reference exists for this timer event,
             * don't free it. This is currently incremented
             * for recursive timerProc calls */
            if (te->refcount) {
                te = next;
                continue;
            }
            if (te->prev)
                te->prev->next = te->next;
            else
                eventLoop->timeEventHead = te->next;
            if (te->next)
                te->next->prev = te->prev;
            if (te->finalizerProc) {
                te->finalizerProc(eventLoop, te->clientData);
                now = getMonotonicUs();
            }
            zfree(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
        if (te->id > maxId) {
            te = te->next;
            continue;
        }

        if (te->when <= now) {
            int retval;

            id = te->id;
            te->refcount++;
            retval = te->timeProc(eventLoop, id, te->clientData);
            te->refcount--;
            processed++;
            now = getMonotonicUs();
            if (retval != AE_NOMORE) {
                te->when = now + (monotime)retval * 1000;
            } else {
                te->id = AE_DELETED_EVENT_ID;
            }
        }
        te = te->next;
    }
    return processed;
}

/* Process every pending file event, then every pending time event
 * (that may be registered by file event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set, the function returns ASAP once all
 * the events that can be handled without a wait are processed.
 * if flags has AE_CALL_AFTER_SLEEP set, the aftersleep callback is called.
 * if flags has AE_CALL_BEFORE_SLEEP set, the beforesleep callback is called.
 *
 * The function returns the number of events processed.
 *
 * 创建和初始化
 * aeCreateEventLoop->aeApiCreate
 *        执行  state->kqfd = kqueue();
 * 添加事件
 * aeCreateFileEvent->aeApiAddEvent
 *        执行  kevent(state->kqfd, &ke, 1, NULL, 0, NULL)
 * 事件轮询
 * aeProcessEvents->aeApiPoll
 *         执行  retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,NULL)
 */
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    // D_CONSOLE("aeProcessEvents");

    int processed = 0, numevents;

    /* Nothing to do? return ASAP  (As Soon As Possible) */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want to call aeApiPoll() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */

    /* 注意，即使没有文件事件需要处理，只要我们想要处理时间事件，
     * 我们仍然需要调用 aeApiPoll()，以便休眠直到下一个时间事件准备触发。
     *
     * eventLoop->maxfd != -1 说明已经注册了文件事件
     * 否则没有注册,只有时间事件,这种情况下也会调用 aeApiPoll() 来休眠指定的时间
     */
    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        struct timeval tv, *tvp = NULL; /* NULL means infinite wait. */
        int64_t usUntilTimer;

        // 调用事件处理前回调
        if (eventLoop->beforesleep != NULL && (flags & AE_CALL_BEFORE_SLEEP))
            eventLoop->beforesleep(eventLoop);

        /* The eventLoop->flags may be changed inside beforesleep.
         * So we should check it after beforesleep be called. At the same time,
         * the parameter flags always should have the highest priority.
         * That is to say, once the parameter flag is set to AE_DONT_WAIT,
         * no matter what value eventLoop->flags is set to, we should ignore it. */
        // 计算等待时间
        if ((flags & AE_DONT_WAIT) || (eventLoop->flags & AE_DONT_WAIT)) {
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        } else if (flags & AE_TIME_EVENTS) {
            // 计算到下一个时间事件的等待时间
            usUntilTimer = usUntilEarliestTimer(eventLoop);
            if (usUntilTimer >= 0) {
                tv.tv_sec = usUntilTimer / 1000000;
                tv.tv_usec = usUntilTimer % 1000000;
                tvp = &tv;
            }
        }
        /* Call the multiplexing API, will return only on timeout or when
         * some event fires. */
        // 调用底层多路复用API等待事件
        // 在这里会阻塞, tvp 决定
        // 如果 flag 只有 AE_TIME_EVENTS, 则会阻塞 tvp 时长,然后去处理时间事件
        // 如果 flag 只有 AE_FILE_EVENTS, 则无限等待文件事件的发生
        // 如果 flag 两种事件都有设置
        //      如果设置了 AE_FILE_EVENTS, 立即返回
        //      否则会至多等待 tvp 时长
        numevents = aeApiPoll(eventLoop, tvp);

        /* Don't process file events if not requested. */
        if (!(flags & AE_FILE_EVENTS)) {
            numevents = 0;
        }

        /* After sleep callback. */
        if (eventLoop->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP)
            eventLoop->aftersleep(eventLoop);

        // 处理已触发的文件事件
        // D_CONSOLE("%d events need process",numevents);
        for (j = 0; j < numevents; j++) {
            int fd = eventLoop->fired[j].fd;
            // 从已就绪数组中获取事件
            aeFileEvent *fe = &eventLoop->events[fd];
            int mask = eventLoop->fired[j].mask;
            int fired = 0; /* Number of events fired for current fd. */

            /* Normally we execute the readable event first, and the writable
             * event later. This is useful as sometimes we may be able
             * to serve the reply of a query immediately after processing the
             * query.
             *
             * However if AE_BARRIER is set in the mask, our application is
             * asking us to do the reverse: never fire the writable event
             * after the readable. In such a case, we invert the calls.
             * This is useful when, for instance, we want to do things
             * in the beforeSleep() hook, like fsyncing a file to disk,
             * before replying to a client.
             */
            int invert = fe->mask & AE_BARRIER;

            /* Note the "fe->mask & mask & ..." code: maybe an already
             * processed event removed an element that fired and we still
             * didn't processed, so we check if the event is still valid.
             *
             * Fire the readable event if the call sequence is not
             * inverted.
             *
             * AE_BARRIER 事件屏障
             * 设置了 invert , 先处理事件, 再处理写事件
             */
            if (!invert && fe->mask & mask & AE_READABLE) {
                // redisAeReadEvent
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                fired++;
                // aeCreateFileEvent 可能重新分配 eventLoop->events
                // 所以这里重新获取 fe
                fe = &eventLoop->events[fd]; /* Refresh in case of resize. */
            }

            /* Fire the writable event. */
            // 处理可读事件
            if (fe->mask & mask & AE_WRITABLE) {
                // 如果写函数和读函数一样,则不必重新处理
                // 故添加 !fired 来避免这种情况
                if (!fired || fe->wfileProc != fe->rfileProc) {
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            /* If we have to invert the call, fire the readable event now
             * after the writable one. */
            if (invert) {
                fe = &eventLoop->events[fd]; /* Refresh in case of resize. */
                if ((fe->mask & mask & AE_READABLE) &&
                    (!fired || fe->wfileProc != fe->rfileProc))
                {
                    fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                    // TODO 这里的 fired++ 是无效代码
                    fired++;
                }
            }

            processed++;
        }
    }
    /* Check time events */
    // 处理时间事件
    // 如果只有时间事件,这里肯定有事件可以处理,因为上面有延迟等待
    // 如果两个都有,可能没有时间事件需要处理
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

void aeMain(aeEventLoop *eventLoop) {
    D_CONSOLE("aeMain");
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        aeProcessEvents(eventLoop, AE_ALL_EVENTS|
                                   AE_CALL_BEFORE_SLEEP|
                                   AE_CALL_AFTER_SLEEP);
    }
}

char *aeGetApiName(void) {
    return aeApiName();
}

void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}

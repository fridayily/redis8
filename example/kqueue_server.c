#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define MAX_EVENTS 64
#define BUFFER_SIZE 1024
#define SERVER_PORT 8080

// https://people.freebsd.org/~jlemon/papers/kqueue.pdf
// 设置文件描述符为非阻塞模式
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return -1;
    }

    return 0;
}

// 创建监听 socket
int create_server_socket(int port) {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    // 创建 socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        return -1;
    }

    // 设置 socket 选项
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        close(server_fd);
        return -1;
    }

    // 设置地址信息
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // 绑定地址
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

    // 监听连接
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    // 设置为非阻塞
    if (set_nonblocking(server_fd) == -1) {
        close(server_fd);
        return -1;
    }

    return server_fd;
}

// 处理新连接
int handle_new_connection(int kq, int server_fd) {
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct kevent ev;

    // 接受新连接
    client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
        }
        return -1;
    }

    printf("New connection from %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    // 设置客户端 socket 为非阻塞
    if (set_nonblocking(client_fd) == -1) {
        close(client_fd);
        return -1;
    }

    // 添加客户端 socket 到 kqueue 监听读事件
    EV_SET(&ev, client_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) == -1) {
        perror("kevent add client fd");
        close(client_fd);
        return -1;
    }

    return client_fd;
}

// 处理客户端数据
int handle_client_data(int kq, int client_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    // 读取客户端数据
    bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);

    if (bytes_read == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("read");
            return -1;
        }
        // 暂时没有数据可读
        return 0;
    } else if (bytes_read == 0) {
        // 客户端关闭连接
        printf("Client disconnected\n");
        return -1;
    }

    // 添加字符串结束符
    buffer[bytes_read] = '\0';
    printf("Received: %s", buffer);

    // 回显数据给客户端
    if (write(client_fd, buffer, bytes_read) == -1) {
        perror("write");
        return -1;
    }

    return 0;
}

// 添加定时器事件
int add_timer_event(int kq, int timer_id, int interval_ms) {
    struct kevent ev;

    // 添加定时器事件，每 interval_ms 毫秒触发一次
    EV_SET(&ev, timer_id, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, interval_ms, NULL);

    if (kevent(kq, &ev, 1, NULL, 0, NULL) == -1) {
        perror("kevent add timer");
        return -1;
    }

    return 0;
}

int main() {
    int kq, server_fd;
    struct kevent events[MAX_EVENTS];
    int nevents;
    struct kevent ev_set;

    printf("Starting kqueue echo server on port %d\n", SERVER_PORT);

    // 创建 kqueue
    kq = kqueue();
    if (kq == -1) {
        perror("kqueue");
        exit(EXIT_FAILURE);
    }

    // 创建服务器 socket
    server_fd = create_server_socket(SERVER_PORT);
    if (server_fd == -1) {
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d\n", SERVER_PORT);

    // 设置 kevent 结构
    EV_SET(&ev_set, // kevent 实例
        server_fd, // ident: 要监视的文件描述符
        EVFILT_READ, // filter: 事件类型（可读事件）
        EV_ADD, // flags: 添加事件
        0, // fflags: 过滤器特定标志
        0, // data: 过滤器相关数据
        NULL // udata: 用户数据
        );
    // 调用 kevent 注册事件
    // kq 文件描述符
    // &ev_set - 要注册的事件列表
    // 1 - 变更列表中的事件数量
    // NULL - 不接收事件
    // 0 - 接收事件数组大小为 0
    // NULL - 无超时
    if (kevent(kq, &ev_set, 1, NULL, 0, NULL) == -1) {
        perror("kevent add server fd");
        close(server_fd);
        close(kq);
        exit(EXIT_FAILURE);
    }

    // 添加定时器事件，每5秒触发一次
    if (add_timer_event(kq, 1000, 10000) == -1) {
        close(server_fd);
        close(kq);
        exit(EXIT_FAILURE);
    }

    // add_timer_event(kq, 1000, 5000);  // 5秒定时器
    // add_timer_event(kq, 1001, 10000); // 10秒定时器
    // add_timer_event(kq, 1002, 30000); // 30秒定时器

    // 在事件处理中区分不同的定时器
    // if (event->filter == EVFILT_TIMER) {
    //     switch (event->ident) {
    //     case 1000:
    //         printf("5秒定时器触发\n");
    //         break;
    //     case 1001:
    //         printf("10秒定时器触发\n");
    //         break;
    //     case 1002:
    //         printf("30秒定时器触发\n");
    //         break;
    //     }
    // }

    printf("Server started. Waiting for events...\n");

    // 事件循环
    while (1) {
        // 等待事件
        nevents = kevent(kq, NULL, 0, events, MAX_EVENTS, NULL);

        if (nevents == -1) {
            perror("kevent");
            break;
        }

        // 处理事件
        for (int i = 0; i < nevents; i++) {
            struct kevent *event = &events[i];

            // 检查是否有错误
            if (event->flags & EV_ERROR) {
                printf("Event error: %s\n", strerror(event->data));
                continue;
            }

            // 处理定时器事件
            if (event->filter == EVFILT_TIMER) {
                printf("Timer event triggered (ID: %ld)\n", event->ident);
                continue;
            }

            // 处理文件描述符事件
            if (event->ident == server_fd) {
                // 新连接事件
                handle_new_connection(kq, server_fd);
            } else {
                // 客户端数据事件
                if (handle_client_data(kq, event->ident) == -1) {
                    // 客户端断开连接，从 kqueue 中移除
                    EV_SET(&ev_set, event->ident, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                    kevent(kq, &ev_set, 1, NULL, 0, NULL);
                    close(event->ident);
                }
            }
        }
    }

    // 清理资源
    close(server_fd);
    close(kq);

    return 0;
}

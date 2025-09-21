#include <stdio.h>          // 标准输入输出（printf 等）
#include <stdlib.h>         // 内存分配（malloc/free 等）
#include <string.h>         // 字符串操作（memcpy/memmove 等）
#include <unistd.h>         // 系统调用（read/write/close 等）
#include <sys/socket.h>     // 网络编程（socket/bind/listen 等）
#include <netinet/in.h>     // 网络地址结构（sockaddr_in 等）
#include <sys/epoll.h>      // epoll 相关（epoll_create/epoll_wait 等）
#include <arpa/inet.h>      // 网络地址转换（inet_ntop 等）
#include <fcntl.h>          // 关键：定义 F_SETFL、O_NONBLOCK 等宏
#include <errno.h>          // 错误处理（errno 变量，如 EAGAIN 等）

#define MAX_EVENTS 1024
#define BUF_SIZE 4096

// 自定义结构体：封装单个客户端的所有上下文（生产级必需）
typedef struct {
    int fd;                     // 客户端 socket fd
    char ip[INET_ADDRSTRLEN];   // 客户端 IP 地址
    char read_buf[BUF_SIZE];    // 读缓冲区（解决粘包问题）
    int read_len;               // 读缓冲区已存数据长度
    char write_buf[BUF_SIZE];   // 写缓冲区（解决非阻塞写问题）
    int write_len;              // 写缓冲区待发数据长度
    int is_connected;           // 连接状态（1=连接，0=断开）
} ClientConn;

// 创建新的客户端连接上下文
ClientConn* create_client_conn(int fd, const char* ip) {
    ClientConn* conn = (ClientConn*)malloc(sizeof(ClientConn));
    if (!conn) return NULL;
    conn->fd = fd;
    strncpy(conn->ip, ip, INET_ADDRSTRLEN-1);
    conn->read_len = 0;
    conn->write_len = 0;
    conn->is_connected = 1;
    return conn;
}

// 释放客户端连接上下文
void free_client_conn(ClientConn* conn) {
    if (conn) {
        close(conn->fd);
        free(conn);
    }
}

int main() {
    int listen_fd, epoll_fd;
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct epoll_event ev, events[MAX_EVENTS];

    // 1. 创建监听 socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));  // 允许端口复用
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8888);
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    listen(listen_fd, 5);

    // 2. 创建 epoll 实例
    epoll_fd = epoll_create1(0);
    // 监听 listen_fd 的连接事件：用 data.ptr 存自定义标记（区分监听 fd 和客户端 fd）
    ClientConn* listen_conn = create_client_conn(listen_fd, "LISTEN");
    ev.events = EPOLLIN;
    ev.data.ptr = listen_conn;  // 关键：绑定上下文指针
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    printf("Server listening on port 8888...\n");

    while (1) {
        // 3. 等待事件触发（-1 表示永久阻塞）
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; i++) {
            ClientConn* conn = (ClientConn*)events[i].data.ptr;  // 直接获取上下文

            // 4. 处理监听 fd 的连接事件
            if (conn->fd == listen_fd) {
                int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
                printf("New client connected: %s (fd=%d)\n", client_ip, client_fd);

                // 创建客户端上下文，并加入 epoll 监听读事件
                ClientConn* client_conn = create_client_conn(client_fd, client_ip);
                // 边缘触发（ET）+ 非阻塞（避免读/写阻塞整个线程）
                fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL) | O_NONBLOCK);
                ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;  // EPOLLONESHOT 避免多线程竞争
                ev.data.ptr = client_conn;  // 绑定客户端上下文
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
            }
            // 5. 处理客户端 fd 的读事件
            else if (events[i].events & EPOLLIN) {
                if (!conn->is_connected) continue;

                // 读取数据到上下文的读缓冲区（解决粘包）
                while (1) {
                    ssize_t n = read(conn->fd, conn->read_buf + conn->read_len, BUF_SIZE - conn->read_len);
                    if (n > 0) {
                        conn->read_len += n;
                        printf("Received from %s (fd=%d): %.*s\n",
                               conn->ip, conn->fd, conn->read_len, conn->read_buf);

                        // 简单回显：将数据存入写缓冲区（非阻塞写需缓冲）
                        if (conn->write_len + conn->read_len < BUF_SIZE) {
                            memcpy(conn->write_buf + conn->write_len, conn->read_buf, conn->read_len);
                            conn->write_len += conn->read_len;
                            // 触发写事件（告知 epoll 有数据要写）
                            ev.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
                            ev.data.ptr = conn;
                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
                        }
                        conn->read_len = 0;  // 清空读缓冲区
                    } else if (n == 0) {  // 客户端断开
                        printf("Client disconnected: %s (fd=%d)\n", conn->ip, conn->fd);
                        conn->is_connected = 0;
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                        free_client_conn(conn);
                        break;
                    } else {  // 读出错（EAGAIN 表示暂时无数据，ET 模式需退出循环）
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        conn->is_connected = 0;
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                        free_client_conn(conn);
                        break;
                    }
                }
            }
            // 6. 处理客户端 fd 的写事件（非阻塞写）
            else if (events[i].events & EPOLLOUT) {
                if (!conn->is_connected || conn->write_len == 0) continue;

                // 从写缓冲区发送数据
                ssize_t n = write(conn->fd, conn->write_buf, conn->write_len);
                if (n > 0) {
                    // 移动剩余数据（解决部分写入）
                    memmove(conn->write_buf, conn->write_buf + n, conn->write_len - n);
                    conn->write_len -= n;
                    printf("Sent to %s (fd=%d): %.*s\n", conn->ip, conn->fd, (int)n, conn->write_buf);
                }

                // 写完成后，重新监听读事件
                if (conn->write_len == 0) {
                    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    ev.data.ptr = conn;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
                }
            }
        }
    }

    free_client_conn(listen_conn);
    close(listen_fd);
    close(epoll_fd);
    return 0;
}
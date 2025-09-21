#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#define PORT 8888
#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024

void print_current_time(const char* message) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm *tm_info = localtime(&now);
    char time_buffer[64];

    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("%s: %s.%06ld\n", message, time_buffer, tv.tv_usec);
}

int main() {
    int server_fd, new_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int fds[MAX_CLIENTS];  // 存储客户端文件描述符
    fd_set readfds;        // select 读集合
    int max_fd;            // 最大文件描述符

    // 初始化客户端文件描述符数组
    memset(fds, -1, sizeof(fds));

    // 创建服务器 socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket create fail");
        exit(EXIT_FAILURE);
    }

    // 设置 socket 选项，允许端口复用
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt fail");
        exit(EXIT_FAILURE);
    }

    // 绑定地址和端口
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind fail");
        exit(EXIT_FAILURE);
    }

    // 监听连接
    if (listen(server_fd, 5) == -1) {
        perror("listen fail");
        exit(EXIT_FAILURE);
    }

    printf("server start，listen port %d...\n", PORT);
    max_fd = server_fd;
    fds[0] = server_fd;  // 第一个位置存放服务器 socket

    while (1) {
        // 清空并重新设置文件描述符集合
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);  // 添加服务器 socket

        // 添加客户端 socket 到集合
        // max_fd 是 所有被监视的文件描述符中的最大值。
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (fds[i] != -1) {
                FD_SET(fds[i], &readfds);
                if (fds[i] > max_fd) max_fd = fds[i];
            }
        }

        // 等待活动的文件描述符（超时 5 秒）
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        // 同时监视多个socket，避免为每个连接创建线程/进程
        // 等待任何socket变为可读状态
        print_current_time("select begin");
        // 三次握手发生在 select 阻塞期间
        //     1.启动服务端
        //     2.sudo tcpdump -i lo0 port 8888 -n
        //     3.启动客户端
        //     观察 tcpdump 日志可以发现三次握手发生在 select 阻塞期间
        // 客户端                    服务器
        //   |                       |  select() 阻塞
        //   |                       |
        //   |      SYN              |
        //   |---------------------->|
        //   |      SYN-ACK          |
        //   |<----------------------|
        //   |      ACK              |
        //   |---------------------->|
        //   |                       |
        //   |                       |
        //   |                       |
        //   |<----------------------|  select() 返回
        //   |                       |
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        /*  int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval * timeout);
           nfds :委托内核检测的最大文件描述符+1
           readfds : 内核会检测这个文件描述符集合的读缓冲区
           writefds : 内核会检测这个文件描述符集合的写缓冲区
           exceptfds: 内核会检测这个文件描述符集合的异常状态
           timeout: 超时时间
           返回值:
                大于0: 成功,已就绪的文件描述符个数
                -1: 失败
                0: 超时,没有检测到就绪文件描述符
         */
        print_current_time("select end");
        if (activity == -1 && errno != EINTR) {
            perror("select fail");
            continue;
        }

        // select返回后，readfds中只包含就绪的文件描述符
        // 这里判断 server_fd 是否在 readfds 中,如果在说明有客户端连接请求
        if (FD_ISSET(server_fd, &readfds)) {
            // 接受客户端的连接请求并创建一个新的连接套接字。

            print_current_time("Accept called at");
            /* 立即返回
             * int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
             * sockfd：监听套接字的文件描述符
             * addr：指向存储客户端地址信息的缓冲区
             * addrlen：输入时指定 addr 缓冲区大小，返回时包含实际地址长度
             *
             * 连接成功时,会自动填充 client_addr 结构体
             * 获取通讯协议, 端口号,客户端 IP 地址
             */
            if ((new_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len)) == -1) {
                perror("accept fail");
                continue;
            }
            print_current_time("Accept called end");



            // 查找空闲位置存储新客户端
            int i;
            for (i = 1; i < MAX_CLIENTS; i++) {
                if (fds[i] == -1) {
                    fds[i] = new_fd;
                    printf("new connection：%s:%d（fd=%d）\n",
                           inet_ntoa(client_addr.sin_addr),
                           ntohs(client_addr.sin_port),
                           new_fd);
                    break;
                }
            }

            if (i == MAX_CLIENTS) {
                printf("Client limit reached, connection refused\n");
                close(new_fd);
            }
        }

        // 处理客户端消息
        for (int i = 1; i < MAX_CLIENTS; i++) {
            int fd = fds[i];
            if (fd == -1) continue;

            if (FD_ISSET(fd, &readfds)) {
                char buffer[BUFFER_SIZE] = {0};
                ssize_t bytes_read = recv(fd, buffer, BUFFER_SIZE - 1, 0);

                if (bytes_read <= 0) {
                    /*
                     * int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
                     * sockfd：已连接的套接字文件描述符
                     * addr：指向存储对端地址信息的缓冲区
                     * addrlen：输入时指定 addr 缓冲区大小，返回时包含实际地址长度
                     *
                     * 这里获取客户端的地址信息存到 client_addr
                     */
                    getpeername(fd, (struct sockaddr*)&client_addr, &client_len);
                    printf("客户端断开：%s:%d（fd=%d）\n",
                           inet_ntoa(client_addr.sin_addr),
                           ntohs(client_addr.sin_port),
                           fd);
                    close(fd);
                    fds[i] = -1;
                } else {
                    // 显示并回显消息
                    printf("收到 fd=%d 的消息：%s\n", fd, buffer);
                    send(fd, buffer, bytes_read, 0);  // 回显消息
                }
            }
        }
    }

    // 关闭服务器（实际不会执行到这里）
    close(server_fd);
    return 0;
}
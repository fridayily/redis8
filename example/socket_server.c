// server.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 5

void handle_client(int client_fd, struct sockaddr_in *client_addr) {
    char buffer[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    int bytes_received;
    time_t now;

    // inet_ntoa 是一个用于网络编程的函数，用于将 IPv4 地址从二进制格式转换为点分十进制字符串表示。
    printf("Client connected: %s:%d\n",
           inet_ntoa(client_addr->sin_addr),
           ntohs(client_addr->sin_port));

    // 发送欢迎消息
    const char *welcome = "Welcome to TCP Server! Send 'quit' to disconnect.\n";
    // ssize_t send(int sockfd, const void *buf, size_t len, int flags);
    //   sockfd 已连接的客户端套接字描述符
    //   buf（发送缓冲区）
    //   len（数据长度）
    //   flags 控制发送行为的标志
    //       0 - 默认行为，阻塞发送直到数据被复制到内核缓冲区
    //       MSG_DONTWAIT - 非阻塞发送
    //       MSG_NOSIGNAL - 发送时不产生 SIGPIPE 信号
    send(client_fd, welcome, strlen(welcome), 0);

    // 处理客户端消息循环
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        // ssize_t recv(int sockfd, void *buf, size_t len, int flags);
        //     sockfd 已连接的客户端套接字描述符
        //     buf 指向存储接收数据的缓冲区
        //     len（缓冲区长度）
        //     flags 和 send 的 flags 有相同部分, 也有不同部分

        bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                printf("Client disconnected: %s:%d\n",
                       inet_ntoa(client_addr->sin_addr),
                       ntohs(client_addr->sin_port));
            } else {
                perror("recv failed");
            }
            break;
        }

        // 移除换行符
        buffer[strcspn(buffer, "\r\n")] = 0;

        printf("Received from %s:%d: %s\n",
               inet_ntoa(client_addr->sin_addr),
               ntohs(client_addr->sin_port),
               buffer);

        // 处理特殊命令
        if (strcmp(buffer, "quit") == 0) {
            const char *bye = "Goodbye!\n";
            send(client_fd, bye, strlen(bye), 0);
            break;
        } else if (strcmp(buffer, "time") == 0) {
            time(&now);
            snprintf(response, BUFFER_SIZE, "Current time: %s", ctime(&now));
            send(client_fd, response, strlen(response), 0);
        } else if (strcmp(buffer, "help") == 0) {
            const char *help_msg = "Available commands: time, help, quit\n";
            send(client_fd, help_msg, strlen(help_msg), 0);
        } else {
            // 回显消息
            snprintf(response, BUFFER_SIZE, "Echo: %s\n", buffer);
            send(client_fd, response, strlen(response), 0);
        }
    }

    close(client_fd);
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int opt = 1;

    printf("Starting TCP Server on port %d...\n", PORT);

    // 1. 创建套接字
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // 2. 设置套接字选项
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // 3. 配置服务器地址
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有接口
    server_addr.sin_port = htons(PORT);

    // 4. 绑定套接字
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // 5. 开始监听
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d\n", PORT);
    printf("Press Ctrl+C to stop the server\n");

    // 6. 接受客户端连接
    while (1) {
        // int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
        //  sockfd 通过 socket() 创建并经过 bind() 和 listen() 设置的监听套接字
        //  addr 输出参数，用于存储连接客户端的地址信息
        //  addrlen（地址结构长度）
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }

        // 处理客户端（在实际应用中可能需要使用多线程或fork）
        handle_client(client_fd, &client_addr);
    }

    close(server_fd);
    return 0;
}

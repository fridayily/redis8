// client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int connect_to_server(const char *hostname, int port) {
    struct addrinfo hints, *result, *rp;
    int sockfd, s;
    char port_str[6];

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;     // 允许 IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP 流套接字
    hints.ai_flags = 0;
    hints.ai_protocol = 0;

    snprintf(port_str, sizeof(port_str), "%d", port);

    s = getaddrinfo(hostname, port_str, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        return -1;
    }

    // 遍历结果链表，尝试连接
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) {
            continue;
        }

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break; // 连接成功
        }

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(result);

    if (rp == NULL) {
        fprintf(stderr, "Failed to connect to %s:%d\n", hostname, port);
        return -1;
    }

    return sockfd;
}

int main(int argc, char *argv[]) {
    int sockfd;
    char buffer[BUFFER_SIZE];
    char message[BUFFER_SIZE];
    const char *hostname = "127.0.0.1";  // 默认连接本地

    // 可以通过命令行参数指定服务器地址
    if (argc > 1) {
        hostname = argv[1];
    }

    printf("Connecting to server %s:%d...\n", hostname, PORT);

    // 连接到服务器
    sockfd = connect_to_server(hostname, PORT);
    if (sockfd == -1) {
        exit(EXIT_FAILURE);
    }

    printf("Connected to server!\n");
    printf("Type 'help' for available commands, 'quit' to exit\n\n");

    // 接收欢迎消息
    memset(buffer, 0, BUFFER_SIZE);
    if (recv(sockfd, buffer, BUFFER_SIZE - 1, 0) > 0) {
        printf("%s", buffer);
    }

    // 主循环：发送和接收消息
    while (1) {
        printf("> ");
        fflush(stdout);

        // 读取用户输入
        if (fgets(message, BUFFER_SIZE, stdin) == NULL) {
            break;
        }

        // 移除换行符
        message[strcspn(message, "\r\n")] = 0;

        // 发送消息到服务器
        if (send(sockfd, message, strlen(message), 0) < 0) {
            perror("send failed");
            break;
        }

        // 接收服务器响应
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                printf("Server disconnected\n");
            } else {
                perror("recv failed");
            }
            break;
        }

        printf("%s", buffer);

        // 检查是否应该退出
        if (strcmp(message, "quit") == 0) {
            break;
        }
    }

    printf("Disconnecting from server...\n");
    close(sockfd);
    return 0;
}

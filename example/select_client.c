#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 8888
#define BUFFER_SIZE 1024
#define NUM_MESSAGES 5  // 要发送的消息数量

// 要发送的消息数组
const char* messages[] = {
    "First test message",
    "Second test message",
    "Third test message",
    "Fourth test message",
    "Fifth test message",
};

int main(int argc, char *argv[]) {
    int sockfd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    const char *hostname = "127.0.0.1";  // 默认连接本地


    // 创建客户端 socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket create fail");
        exit(EXIT_FAILURE);
    }

    // 设置服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    // 转换IP地址
    if (inet_pton(AF_INET, hostname, &server_addr.sin_addr) <= 0) {
        perror("invalid IP");
        exit(EXIT_FAILURE);
    }

    // 与服务器建立连接
    printf("try to connect to %s:%d...\n", hostname, PORT);
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect fail");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    printf("connect success\n");

    // 发送多条消息
    for (int i = 0; i < NUM_MESSAGES; i++) {
        // 发送消息
        if (send(sockfd, messages[i], strlen(messages[i]), 0) == -1) {
            perror("fail to send message");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        printf("send: %s\n", messages[i]);

        // 等待接收服务器回复
        memset(buffer, 0, BUFFER_SIZE);
        ssize_t bytes_read = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) {
            if (bytes_read < 0) perror("fail to recv data");
            else printf("Server disconnected\n");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        printf("Server response: %s\n", buffer);

        // 稍微延迟一下，让输出更清晰
        sleep(2);
    }

    // 发送结束消息
    const char* exit_msg = "Client finished sending, about to disconnect";
    send(sockfd, exit_msg, strlen(exit_msg), 0);
    printf("\n%s\n", exit_msg);

    // 关闭连接
    close(sockfd);
    printf("Client exited\n");
    return 0;
}

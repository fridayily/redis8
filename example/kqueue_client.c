#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

#define BUFFER_SIZE 1024
#define SERVER_HOST "127.0.0.1"
#define SERVER_PORT 8080

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

// 创建客户端 socket 并连接到服务器
int create_client_socket(const char* host, int port) {
    int client_fd;
    struct sockaddr_in server_addr;

    // 创建 socket
    client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1) {
        perror("socket");
        return -1;
    }

    // 设置服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    // 将点分十进制IP地址转换为二进制格式
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(client_fd);
        return -1;
    }

    // 连接到服务器
    if (connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        if (errno != EINPROGRESS) {
            perror("connect");
            close(client_fd);
            return -1;
        }
    }

    printf("Connected to server %s:%d\n", host, port);
    return client_fd;
}

// 发送消息到服务器
int send_message(int client_fd, const char* message) {
    size_t len = strlen(message);
    ssize_t sent = 0;

    while (sent < len) {
        ssize_t n = write(client_fd, message + sent, len - sent);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 使用 poll 等待 socket 可写
                struct pollfd pfd;
                pfd.fd = client_fd;
                pfd.events = POLLOUT;

                if (poll(&pfd, 1, 5000) <= 0) {  // 5秒超时
                    fprintf(stderr, "Timeout waiting for socket to be writable\n");
                    return -1;
                }
                continue;
            } else {
                perror("write");
                return -1;
            }
        }
        sent += n;
    }

    return 0;
}

// 从服务器接收消息
int receive_message(int client_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    // 使用 poll 等待数据到达，设置超时
    struct pollfd pfd;
    pfd.fd = client_fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, 1000);  // 1秒超时
    if (ret == 0) {
        printf("No data received within timeout\n");
        return 0;  // 超时但不是错误
    } else if (ret == -1) {
        perror("poll");
        return -1;
    }

    // 读取数据
    bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("read");
            return -1;
        }
        return 0;  // 暂时没有数据
    } else if (bytes_read == 0) {
        printf("Server closed the connection\n");
        return -1;  // 连接关闭
    }

    buffer[bytes_read] = '\0';
    printf("Received from server: %s", buffer);

    return 0;
}

// 交互式发送消息
void interactive_mode(int client_fd) {
    char input[BUFFER_SIZE];

    printf("Enter messages to send (type 'quit' to exit):\n");

    while (1) {
        printf("> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }

        // 移除换行符
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "quit") == 0) {
            printf("Disconnecting...\n");
            break;
        }

        if (strlen(input) == 0) {
            continue;
        }

        // 发送消息
        if (send_message(client_fd, input) == -1) {
            fprintf(stderr, "Failed to send message\n");
            break;
        }

        // 添加换行符用于服务器回显
        if (send_message(client_fd, "\n") == -1) {
            fprintf(stderr, "Failed to send newline\n");
            break;
        }

        // 接收回显
        if (receive_message(client_fd) == -1) {
            break;
        }
    }
}

// 发送测试消息
void test_mode(int client_fd) {
    const char* test_messages[] = {
        "Hello, Server!",
        "This is a test message.",
        "Another test message with numbers: 12345",
        "Final test message."
    };

    int num_tests = sizeof(test_messages) / sizeof(test_messages[0]);

    printf("Running %d test messages...\n", num_tests);

    for (int i = 0; i < num_tests; i++) {
        printf("Sending test message %d: %s\n", i + 1, test_messages[i]);

        // 发送消息
        if (send_message(client_fd, test_messages[i]) == -1) {
            fprintf(stderr, "Failed to send test message %d\n", i + 1);
            return;
        }

        // 发送换行符
        if (send_message(client_fd, "\n") == -1) {
            fprintf(stderr, "Failed to send newline for test message %d\n", i + 1);
            return;
        }

        // 接收回显
        printf("Waiting for response...\n");
        if (receive_message(client_fd) == -1) {
            return;
        }

        // 简单延时
        sleep(1);
    }

    printf("Test completed.\n");
}

int main(int argc, char* argv[]) {
    int client_fd;
    int test_mode_flag = 1;

    // 检查命令行参数
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        test_mode_flag = 1;
    }

    printf("Connecting to server at %s:%d\n", SERVER_HOST, SERVER_PORT);

    // 创建客户端 socket
    client_fd = create_client_socket(SERVER_HOST, SERVER_PORT);
    if (client_fd == -1) {
        exit(EXIT_FAILURE);
    }

    // 设置为非阻塞模式
    if (set_nonblocking(client_fd) == -1) {
        close(client_fd);
        exit(EXIT_FAILURE);
    }

    // 根据模式运行
    if (test_mode_flag) {
        test_mode(client_fd);
    } else {
        interactive_mode(client_fd);
    }

    // 关闭连接
    close(client_fd);
    printf("Disconnected from server.\n");

    return 0;
}

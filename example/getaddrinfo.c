// getaddrinfo.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <arpa/inet.h>

// 打印地址信息的函数
void print_addrinfo(struct addrinfo *ai) {
    char ip_str[INET6_ADDRSTRLEN];
    void *addr;
    const char *ipver;
    char port_str[6];

    // 获取地址信息
    if (ai->ai_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)ai->ai_addr;
        addr = &(ipv4->sin_addr);
        snprintf(port_str, sizeof(port_str), "%d", ntohs(ipv4->sin_port));
        ipver = "IPv4";
    } else {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)ai->ai_addr;
        addr = &(ipv6->sin6_addr);
        snprintf(port_str, sizeof(port_str), "%d", ntohs(ipv6->sin6_port));
        ipver = "IPv6";
    }

    // 转换为可读的 IP 地址
    inet_ntop(ai->ai_family, addr, ip_str, sizeof(ip_str));

    printf("  %s: %s port %s\n", ipver, ip_str, port_str);

    // 打印规范主机名（如果可用）
    if (ai->ai_canonname) {
        printf("  Canonical name: %s\n", ai->ai_canonname);
    }
}

// DNS 解析示例
void dns_lookup_example(const char *hostname, const char *port) {
    struct addrinfo hints, *result, *rp;
    int s;

    printf("=== DNS Lookup for %s:%s ===\n", hostname, port);

    // 初始化 hints 结构
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // 允许 IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP 流套接字
    hints.ai_flags = 0;
    hints.ai_protocol = 0;           // 任何协议

    // 执行地址解析
    s = getaddrinfo(hostname, port, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(s));
        return;
    }

    // 遍历结果链表
    int count = 0;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        printf("Address %d:\n", ++count);
        print_addrinfo(rp);
        printf("\n");
    }

    freeaddrinfo(result); // 释放内存
}

// 客户端连接示例
int client_connect_example(const char *hostname, const char *port) {
    struct addrinfo hints, *result, *rp;
    int sockfd, s;

    printf("=== Client Connection to %s:%s ===\n", hostname, port);

    // 初始化 hints 结构
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;     // 允许 IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP 流套接字
    hints.ai_flags = 0;
    hints.ai_protocol = 0;           // 任何协议

    // 执行地址解析
    s = getaddrinfo(hostname, port, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        return -1;
    }

    // 遍历结果链表，尝试连接
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) {
            perror("socket");
            continue;
        }

        printf("Trying to connect...\n");
        print_addrinfo(rp);

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            printf("Successfully connected!\n");
            break; // 连接成功
        }

        printf("Connection failed: %s\n", strerror(errno));
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(result); // 释放内存

    if (rp == NULL) {
        fprintf(stderr, "Failed to connect to %s:%s\n", hostname, port);
        return -1;
    }

    return sockfd;
}

// 服务器端绑定示例
int server_bind_example(const char *port) {
    struct addrinfo hints, *result, *rp;
    int sockfd, s;
    int yes = 1;

    printf("=== Server Binding to port %s ===\n", port);

    // 初始化 hints 结构
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;     // IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_PASSIVE;     // 用于 bind()，表示服务器端

    // NULL 节点表示绑定到所有本地地址
    s = getaddrinfo(NULL, port, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        return -1;
    }

    // 遍历结果，创建并绑定套接字
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) {
            perror("socket");
            continue;
        }

        // 允许地址重用（避免 "Address already in use" 错误）
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
            perror("setsockopt");
            close(sockfd);
            continue;
        }

        printf("Trying to bind...\n");
        print_addrinfo(rp);

        if (bind(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            printf("Successfully bound!\n");
            break; // 绑定成功
        }

        printf("Bind failed: %s\n", strerror(errno));
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(result); // 释放内存

    if (rp == NULL) {
        fprintf(stderr, "Failed to bind to port %s\n", port);
        return -1;
    }

    if (listen(sockfd, 10) == -1) {
        perror("listen");
        close(sockfd);
        return -1;
    }

    printf("Server listening on port %s\n", port);
    return sockfd;
}

// 使用不同标志的示例
void flags_example() {
    struct addrinfo hints, *result;
    int s;

    printf("=== Different getaddrinfo Flags ===\n");

    // 1. AI_PASSIVE 标志（服务器端）
    printf("1. AI_PASSIVE flag (for server):\n");
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    s = getaddrinfo(NULL, "8080", &hints, &result);
    if (s == 0) {
        printf("   Success - suitable for binding\n");
        freeaddrinfo(result);
    }

    // 2. AI_CANONNAME 标志（获取规范名称）
    printf("2. AI_CANONNAME flag (get canonical name):\n");
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    s = getaddrinfo("www.google.com", "80", &hints, &result);
    if (s == 0) {
        if (result->ai_canonname) {
            printf("   Canonical name: %s\n", result->ai_canonname);
        }
        freeaddrinfo(result);
    }

    // 3. AI_NUMERICHOST 标志（只接受数值型地址）
    printf("3. AI_NUMERICHOST flag (numeric host only):\n");
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    s = getaddrinfo("127.0.0.1", "80", &hints, &result);
    if (s == 0) {
        printf("   Success with numeric host\n");
        freeaddrinfo(result);
    } else {
        printf("   Failed as expected: %s\n", gai_strerror(s));
    }
}

int main() {
    printf("getaddrinfo() usage examples\n");
    printf("============================\n\n");

    // 1. DNS 查找示例
    // dns_lookup_example("www.baidu.com", "80");
    // printf("\n");
    //
    // dns_lookup_example("localhost", "22");
    // printf("\n");
    //
    // // 2. 不同标志示例
    // flags_example();
    // printf("\n");

    // 3. 客户端连接示例（注释掉以避免实际连接）

    int client_fd = client_connect_example("www.baidu.com", "80");
    if (client_fd != -1) {
        printf("Connected to server, socket fd: %d\n", client_fd);
        close(client_fd);
    }
    printf("\n");


    // 4. 服务器绑定示例（注释掉以避免实际绑定）

    int server_fd = server_bind_example("8080");
    if (server_fd != -1) {
        printf("Server bound, socket fd: %d\n", server_fd);
        close(server_fd);
    }
    printf("\n");


    // 5. IPv6 示例
    // printf("=== IPv6 Example ===\n");
    // dns_lookup_example("ipv6.google.com", "80");

    return 0;
}

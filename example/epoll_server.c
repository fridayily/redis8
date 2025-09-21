#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

// server
int main(int argc, const char *argv[]) {
    (void) argc;
    (void) argv;
    // 创建监听的套接字
    printf("创建监听套接字\n");
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        perror("socket error");
        exit(1);
    }

    // 绑定
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET; // IPv4 Internet 协议
    serv_addr.sin_port = htons(9999); // htons 主机字节序转网络字节序（16位，用于端口）
    // htonl 主机字节序转网络字节序（32位，用于IP地址）
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 监听所有网络接口

    // 设置端口复用
    int opt = 1;
    // lfd 套接字文件描述符
    // SOL_SOCKET  选项级别，表示套接字级别选项
    // SO_REUSEADDR  选项名称，表示地址重用选项
    //         允许套接字绑定到一个已经被其他套接字使用但处于 TIME_WAIT 状态的地址
    //         设置后允许停在服务器后立即重连
    // 选项值的指针，这里设置为1（启用）
    // sizeof(opt) 选项值的大小
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定端口
    printf("绑定端口\n");

    // 将套接字与具体的网络地址绑定
    // 地址绑定：告诉内核这个套接字应该监听哪个IP地址和端口
    // 网络身份：为服务器在网络中建立身份标识
    // 接收连接：为后续的 listen() 和 accept() 操作做准备
    int ret = bind(lfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
    if (ret == -1) {
        perror("bind error");
        exit(1);
    }

    // 监听
    printf("监听\n");

    // 将套接字从主动连接模式转换为被动监听模式，准备接收客户端连接请求。
    // backlog =3, 最多允许 3 个连接在等待队列中排队, 如果第 4 个连接请求到达而队列已满，可能会被拒绝
    ret = listen(lfd, 3);
    if (ret == -1) {
        perror("listen error");
        exit(1);
    }

    // 现在只有监听的文件描述符
    // 所有的文件描述符对应读写缓冲区状态都是委托内核进行检测的epoll
    // 创建一个epoll模型
    printf("创建一个epoll模型\n");

    // 创建 epoll 实例
    // 当进程执行 exec 系列函数时，自动关闭带有此标志的文件描述符
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        perror("epoll_create");
        exit(0);
    }

    // 往epoll实例中添加需要检测的节点, 现在只有监听的文件描述符
    struct epoll_event ev;
    ev.events = EPOLLIN;    // 水平触发
    ev.data.fd = lfd; // socket 返回的 fd
    printf("往epoll实例中添加需要检测的节点\n");

    // 将监听套接字添加到 epoll 实例
    // epoll 实例的文件描述符
    // POLL_CTL_ADD - 操作类型，表示添加监控事件
    // lfd - 要监控的目标文件描述符（
    // &ev - 指向 epoll_event 结构体的指针，描述要监控的事件
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        exit(0);
    }

    struct epoll_event evs[1024];
    int size = sizeof(evs) / sizeof(struct epoll_event);
    // 持续检测
    while (1) {
        // 调用一次, 检测一次
        // num 表示就绪事件的数量
        int num = epoll_wait(epfd, evs, size, -1);
        for (int i = 0; i < num; ++i) {
            // 取出当前的文件描述符
            int curfd = evs[i].data.fd;
            printf("i = %d,  curfd=%d num=%d \n", i, curfd, num);
            // 判断这个文件描述符是不是用于监听的
            if (curfd == lfd) {
                // 建立新的连接
            //    break;
                printf("准备建立新的连接 lfd = %d\n", lfd);
                int cfd = accept(curfd, NULL, NULL);
                // 新得到的文件描述符添加到epoll模型中, 下一轮循环的时候就可以被检测了
                ev.events = EPOLLIN;    // 读缓冲区是否有数据
                ev.data.fd = cfd;
                ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
                if (ret == -1) {
                    perror("epoll_ctl-accept");
                    exit(0);
                }
            } else {
                // 处理通信的文件描述符
                // 接收数据
                char buf[1024];
                memset(buf, 0, sizeof(buf));
                int len = recv(curfd, buf, sizeof(buf), 0);
                if (len == 0) {
                    printf("客户端已经断开了连接\n");
                    // 将这个文件描述符从epoll模型中删除
                    epoll_ctl(epfd, EPOLL_CTL_DEL, curfd, NULL);
                    close(curfd);
                } else if (len > 0) {
                    printf("客户端say: %s\n", buf);
                    send(curfd, buf, len, 0);
                } else {
                    perror("recv");
                    exit(0);
                }
            }
        }
    }

    close(lfd);
    return 0;
}

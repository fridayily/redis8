#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define MAX_EVENTS 10
#define MAX_PATH_LEN 256

// 文件监视结构体
struct file_monitor {
    int fd;
    char path[MAX_PATH_LEN];
};

// 打印当前时间
void print_current_time() {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buffer[64];
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("[%s] ", time_buffer);
}

// 创建测试文件
void create_test_file(const char* filename) {
    FILE *file = fopen(filename, "w");
    if (file) {
        fprintf(file, "This is a test file for kqueue monitoring.\n");
        fclose(file);
        printf("Created test file: %s\n", filename);
    }
}

// 修改测试文件
void modify_test_file(const char* filename) {
    FILE *file = fopen(filename, "a");
    if (file) {
        fprintf(file, "Modified at: %ld\n", time(NULL));
        fclose(file);
        printf("Modified test file: %s\n", filename);
    }
}

// 文件系统事件监视函数
void monitor_file_system_events() {
    int kq;
    struct kevent ev;
    struct kevent events[MAX_EVENTS];
    struct file_monitor monitors[5];
    int num_monitors = 0;
    int nevents;

    printf("=== 文件系统事件监视示例 ===\n");

    // 创建 kqueue
    kq = kqueue();
    if (kq == -1) {
        perror("kqueue");
        return;
    }

    // 创建测试文件
    const char* test_files[] = {"test1.txt", "test2.txt", "test3.txt"};
    int num_files = sizeof(test_files) / sizeof(test_files[0]);

    for (int i = 0; i < num_files; i++) {
        create_test_file(test_files[i]);

        // 打开文件用于监视
        int fd = open(test_files[i], O_RDONLY);
        if (fd == -1) {
            perror("open file");
            continue;
        }

        // 保存监视信息
        monitors[num_monitors].fd = fd;
        strncpy(monitors[num_monitors].path, test_files[i], MAX_PATH_LEN - 1);
        monitors[num_monitors].path[MAX_PATH_LEN - 1] = '\0';
        num_monitors++;

        // 设置文件监视事件
        // 监视文件写入、删除、扩展等事件
        EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_RENAME,
               0, (void*)test_files[i]);

        if (kevent(kq, &ev, 1, NULL, 0, NULL) == -1) {
            perror("kevent add vnode");
            close(fd);
            num_monitors--;
            continue;
        }

        printf("开始监视文件: %s (fd: %d)\n", test_files[i], fd);
    }

    if (num_monitors == 0) {
        printf("没有文件可以监视\n");
        close(kq);
        return;
    }

    // 添加目录监视
    const char* watch_dir = ".";
    int dir_fd = open(watch_dir, O_RDONLY);
    if (dir_fd != -1) {
        EV_SET(&ev, dir_fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_ATTRIB,
               0, (void*)watch_dir);

        if (kevent(kq, &ev, 1, NULL, 0, NULL) == -1) {
            perror("kevent add directory");
        } else {
            monitors[num_monitors].fd = dir_fd;
            strncpy(monitors[num_monitors].path, watch_dir, MAX_PATH_LEN - 1);
            printf("开始监视目录: %s (fd: %d)\n", watch_dir, dir_fd);
            num_monitors++;
        }
    }

    // 添加定时器事件，定期修改文件
    EV_SET(&ev, 2000, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, 3000, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) == -1) {
        perror("kevent add timer");
    } else {
        printf("添加定时器，每3秒修改一次文件\n");
    }

    printf("\n文件系统监视已启动，等待事件...\n");
    printf("按 Ctrl+C 退出\n\n");

    int timer_count = 0;
    int event_count = 0;
    const int max_events = 20; // 限制事件数量以便演示结束

    // 事件循环
    while (event_count < max_events) {
        // 等待事件（最多等待5秒）
        struct timespec timeout = {5, 0}; // 5秒超时
        nevents = kevent(kq, NULL, 0, events, MAX_EVENTS, &timeout);

        if (nevents == -1) {
            if (errno == EINTR) {
                printf("被信号中断\n");
                break;
            }
            perror("kevent");
            break;
        }

        if (nevents == 0) {
            printf("等待超时，继续等待...\n");
            continue;
        }

        event_count += nevents;

        // 处理事件
        for (int i = 0; i < nevents; i++) {
            print_current_time();

            if (events[i].flags & EV_ERROR) {
                printf("事件错误: %s\n", strerror(events[i].data));
                continue;
            }

            // 处理定时器事件
            if (events[i].filter == EVFILT_TIMER) {
                timer_count++;
                printf("定时器事件 #%d (ID: %ld)\n", timer_count, events[i].ident);

                // 定期修改测试文件
                if (timer_count % 2 == 1) {
                    modify_test_file("test1.txt");
                } else {
                    modify_test_file("test2.txt");
                }

                // 每5次定时器事件重命名一个文件
                if (timer_count % 5 == 0) {
                    char old_name[MAX_PATH_LEN], new_name[MAX_PATH_LEN];
                    snprintf(old_name, sizeof(old_name), "test3.txt");
                    snprintf(new_name, sizeof(new_name), "test3_renamed_%d.txt", timer_count);

                    if (rename(old_name, new_name) == 0) {
                        printf("  重命名文件: %s -> %s\n", old_name, new_name);
                        // 重新创建原文件
                        create_test_file(old_name);
                    }
                }
                continue;
            }

            // 处理文件系统事件
            if (events[i].filter == EVFILT_VNODE) {
                const char* path = (const char*)events[i].udata;
                printf("文件系统事件 - 文件: %s\n", path ? path : "unknown");

                // 解析具体的事件类型
                if (events[i].fflags & NOTE_WRITE) {
                    printf("  [WRITE] 文件被写入\n");
                }
                if (events[i].fflags & NOTE_DELETE) {
                    printf("  [DELETE] 文件被删除\n");
                }
                if (events[i].fflags & NOTE_EXTEND) {
                    printf("  [EXTEND] 文件被扩展\n");
                }
                if (events[i].fflags & NOTE_ATTRIB) {
                    printf("  [ATTRIB] 文件属性被修改\n");
                }
                if (events[i].fflags & NOTE_RENAME) {
                    printf("  [RENAME] 文件被重命名\n");
                }
                if (events[i].fflags & NOTE_LINK) {
                    printf("  [LINK] 文件链接数改变\n");
                }

                // 显示事件数据
                if (events[i].data > 0) {
                    printf("  数据变化: %ld 字节\n", events[i].data);
                }
            }
        }

        printf("\n");
    }

    // 清理资源
    printf("清理资源...\n");
    for (int i = 0; i < num_monitors; i++) {
        close(monitors[i].fd);
    }
    close(kq);

    // 清理测试文件
    printf("删除测试文件...\n");
    for (int i = 0; i < num_files; i++) {
        unlink(test_files[i]);
    }

    // 删除重命名的文件
    for (int i = 1; i <= timer_count; i++) {
        if (i % 5 == 0) {
            char filename[MAX_PATH_LEN];
            snprintf(filename, sizeof(filename), "test3_renamed_%d.txt", i);
            unlink(filename);
        }
    }

    printf("文件系统监视示例完成\n");
}

// 单文件监视示例
void monitor_single_file() {
    int kq, fd;
    struct kevent ev;
    struct kevent events[5];
    const char* filename = "single_test.txt";

    printf("=== 单文件监视示例 ===\n");

    // 创建测试文件
    FILE *file = fopen(filename, "w");
    if (file) {
        fprintf(file, "Single file monitoring test.\n");
        fclose(file);
    }

    // 创建 kqueue
    kq = kqueue();
    if (kq == -1) {
        perror("kqueue");
        return;
    }

    // 打开文件
    fd = open(filename, O_RDONLY);
    if (fd == -1) {
        perror("open");
        close(kq);
        return;
    }

    // 设置监视事件
    EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_ATTRIB,
           0, (void*)filename);

    if (kevent(kq, &ev, 1, NULL, 0, NULL) == -1) {
        perror("kevent add");
        close(fd);
        close(kq);
        return;
    }

    printf("监视文件: %s\n", filename);
    printf("请在另一个终端执行以下命令测试:\n");
    printf("  echo 'test' >> %s\n", filename);
    printf("  touch %s\n", filename);
    printf("  rm %s\n", filename);
    printf("按 Enter 继续...\n");

    getchar();

    // 等待几个事件
    for (int i = 0; i < 3; i++) {
        int nevents = kevent(kq, NULL, 0, events, 5, NULL);
        if (nevents > 0) {
            for (int j = 0; j < nevents; j++) {
                if (events[j].filter == EVFILT_VNODE) {
                    printf("文件事件: ");
                    if (events[j].fflags & NOTE_WRITE) printf("WRITE ");
                    if (events[j].fflags & NOTE_DELETE) printf("DELETE ");
                    if (events[j].fflags & NOTE_EXTEND) printf("EXTEND ");
                    if (events[j].fflags & NOTE_ATTRIB) printf("ATTRIB ");
                    printf("\n");
                }
            }
        }
    }

    // 清理
    close(fd);
    close(kq);
    unlink(filename);
}

int main() {
    printf("kqueue 文件系统事件监视示例\n");

    // 运行完整示例
    monitor_file_system_events();

    printf("\n==================================================\n\n");

    // 运行单文件示例
    // monitor_single_file();

    return 0;
}

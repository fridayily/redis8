#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

// 模拟数据库记录结构
typedef struct {
    int id;
    char data[256];
} Record;

// 模拟数据库表
typedef struct {
    Record *records;
    size_t count;
    size_t capacity;
} DatabaseTable;

// 创建数据库表
DatabaseTable* createTable(size_t initial_capacity) {
    DatabaseTable *table = malloc(sizeof(DatabaseTable));
    table->count = 0;
    table->capacity = initial_capacity;
    // 使用 mmap 分配可被 madvise 管理的内存
    table->records = mmap(NULL, initial_capacity * sizeof(Record),
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return table;
}

// 向表中添加记录
void addRecord(DatabaseTable *table, int id, const char *data) {
    if (table->count >= table->capacity) return;

    table->records[table->count].id = id;
    strncpy(table->records[table->count].data, data, sizeof(table->records[table->count].data) - 1);
    table->records[table->count].data[sizeof(table->records[table->count].data) - 1] = '\0';
    table->count++;
}

// 模拟数据库快照操作（类似 Redis 的 RDB 持久化）
void createSnapshot(DatabaseTable *table) {
    pid_t pid = fork();

    if (pid == 0) {
        // 子进程：执行快照操作
        printf("子进程：开始创建快照...\n");

        // 模拟序列化过程
        printf("子进程：序列化 %zu 条记录...\n", table->count);
        for (size_t i = 0; i < table->count; i++) {
            // 模拟将数据写入磁盘文件
            printf("序列化记录 ID: %d, Data: %s\n", table->records[i].id, table->records[i].data);
        }

        // 快照完成后，释放物理内存（类似 Redis 的 dismissObject）
        size_t table_size = table->capacity * sizeof(Record);
        if (madvise(table->records, table_size, MADV_DONTNEED) == 0) {
            printf("子进程：快照完成，已释放物理内存\n");
        } else {
            perror("madvise 失败");
        }

        // 子进程退出
        exit(0);
    } else if (pid > 0) {
        // 父进程：继续处理请求
        printf("父进程：继续处理新请求...\n");

        // 模拟在子进程创建快照期间，父进程修改数据
        sleep(1);
        addRecord(table, 1001, "父进程新增数据");
        printf("父进程：新增记录 ID: 1001\n");

        // 等待子进程完成
        wait(NULL);
    } else {
        perror("fork 失败");
    }
}

// 释放数据库表
void freeTable(DatabaseTable *table) {
    munmap(table->records, table->capacity * sizeof(Record));
    free(table);
}

int main() {
    // 创建数据库表并添加初始数据
    DatabaseTable *table = createTable(10);
    addRecord(table, 1, "第一条记录");
    addRecord(table, 2, "第二条记录");
    addRecord(table, 3, "第三条记录");

    printf("初始数据加载完成\n");

    // 创建快照（类似 Redis fork 子进程做 RDB 持久化）
    createSnapshot(table);

    // 验证父进程的数据修改是否成功
    printf("验证数据：当前记录数 %zu\n", table->count);
    for (size_t i = 0; i < table->count; i++) {
        printf("记录 %zu: ID=%d, Data=%s\n", i, table->records[i].id, table->records[i].data);
    }

    freeTable(table);
    return 0;
}

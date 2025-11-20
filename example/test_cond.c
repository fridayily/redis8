#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

// 定义队列的最大容量
#define QUEUE_CAPACITY 10

// 队列数据结构
typedef struct {
    int *buffer;          // 缓冲区数组
    int capacity;         // 队列容量
    int size;             // 当前队列大小
    int front;            // 队首索引
    int rear;             // 队尾索引
    pthread_mutex_t mutex; // 互斥锁，保护共享数据
    pthread_cond_t not_full;  // 队列不满条件变量
    pthread_cond_t not_empty; // 队列不空条件变量
} Queue;

// 初始化队列
Queue* queue_init(int capacity) {
    Queue *queue = (Queue*)malloc(sizeof(Queue));
    if (!queue) {
        perror("Failed to allocate queue");
        exit(EXIT_FAILURE);
    }

    queue->buffer = (int*)malloc(sizeof(int) * capacity);
    if (!queue->buffer) {
        perror("Failed to allocate buffer");
        free(queue);
        exit(EXIT_FAILURE);
    }

    queue->capacity = capacity;
    queue->size = 0;
    queue->front = 0;
    queue->rear = 0;

    // 初始化互斥锁和条件变量
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_full, NULL);
    pthread_cond_init(&queue->not_empty, NULL);

    return queue;
}

// 入队操作
void queue_enqueue(Queue *queue, int item) {
    // 获取互斥锁
    pthread_mutex_lock(&queue->mutex);

    // 如果队列已满，等待队列不满条件
    while (queue->size == queue->capacity) {
        printf("队列已满，生产者等待...\n");
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }

    // 添加元素到队尾
    queue->buffer[queue->rear] = item;
    queue->rear = (queue->rear + 1) % queue->capacity; // 循环更新队尾索引
    queue->size++;

    printf("生产者: 入队元素 %d，当前队列大小: %d\n", item, queue->size);

    // 通知等待的消费者队列不为空
    pthread_cond_signal(&queue->not_empty);

    // 释放互斥锁
    pthread_mutex_unlock(&queue->mutex);
}

// 出队操作
int queue_dequeue(Queue *queue) {
    // 获取互斥锁
    pthread_mutex_lock(&queue->mutex);

    // 如果队列为空，等待队列不空条件
    while (queue->size == 0) {
        printf("队列为空，消费者等待...\n");
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    // 从队首取出元素
    int item = queue->buffer[queue->front];
    queue->front = (queue->front + 1) % queue->capacity; // 循环更新队首索引
    queue->size--;

    printf("消费者: 出队元素 %d，当前队列大小: %d\n", item, queue->size);

    // 通知等待的生产者队列不满
    pthread_cond_signal(&queue->not_full);

    // 释放互斥锁
    pthread_mutex_unlock(&queue->mutex);

    return item;
}

// 销毁队列
void queue_destroy(Queue *queue) {
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_full);
    pthread_cond_destroy(&queue->not_empty);
    free(queue->buffer);
    free(queue);
    printf("队列资源已释放\n");
}

// 生产者线程函数
void* producer_thread(void *arg) {
    Queue *queue = (Queue*)arg;

    for (int i = 1; i <= 15; i++) {
        // 生成随机数据（100-199范围的整数）
        int item = 100 + rand() % 100;

        // 将数据入队
        queue_enqueue(queue, item);

        // 模拟生产过程耗时（随机0-1秒）
        usleep((rand() % 1000) * 1000);
    }

    printf("生产者线程完成\n");
    return NULL;
}

// 消费者线程函数
void* consumer_thread(void *arg) {
    Queue *queue = (Queue*)arg;

    for (int i = 1; i <= 15; i++) {
        // 从队列取出数据
        int item = queue_dequeue(queue);

        // 模拟消费过程（实际应用中可以处理数据）
        printf("消费者: 处理数据 %d\n", item);

        // 模拟消费过程耗时（随机1-2秒）
        usleep((1000 + rand() % 1000) * 1000);
    }

    printf("消费者线程完成\n");
    return NULL;
}

int main() {
    // 初始化随机数种子
    srand(time(NULL));

    // 创建并初始化队列
    Queue *queue = queue_init(QUEUE_CAPACITY);

    // 创建生产者和消费者线程
    pthread_t producer_id, consumer_id;

    if (pthread_create(&producer_id, NULL, producer_thread, queue) != 0) {
        perror("Failed to create producer thread");
        exit(EXIT_FAILURE);
    }

    if (pthread_create(&consumer_id, NULL, consumer_thread, queue) != 0) {
        perror("Failed to create consumer thread");
        exit(EXIT_FAILURE);
    }

    // 等待线程结束
    if (pthread_join(producer_id, NULL) != 0) {
        perror("Failed to join producer thread");
        exit(EXIT_FAILURE);
    }

    if (pthread_join(consumer_id, NULL) != 0) {
        perror("Failed to join consumer thread");
        exit(EXIT_FAILURE);
    }

    // 清理队列资源
    queue_destroy(queue);

    printf("主程序执行完成\n");
    return 0;
}
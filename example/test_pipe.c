#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main() {
  int pipe_fd[2];
  pid_t pid;
  char buffer[100];

  // 创建管道
  if (pipe(pipe_fd) == -1) {
    perror("pipe");
    exit(EXIT_FAILURE);
  }

  // 创建子进程
  pid = fork();
  if (pid == -1) {
    perror("fork");
    exit(EXIT_FAILURE);
  }

  if (pid == 0) {
    // 子进程：读取管道
    close(pipe_fd[1]);  // 关闭写端

    // 从管道读取数据
    ssize_t bytes_read = read(pipe_fd[0], buffer, sizeof(buffer) - 1);
    if (bytes_read == -1) {
      perror("read");
      exit(EXIT_FAILURE);
    }

    buffer[bytes_read] = '\0';  // 添加字符串结束符
    printf("Child process read: %s\n", buffer);

    close(pipe_fd[0]);  // 关闭读端
    exit(EXIT_SUCCESS);
  } else {
    // 父进程：写入管道
    close(pipe_fd[0]);  // 关闭读端

    const char *message = "Hello from parent!";
    printf("Parent process writing: %s\n", message);

    // 向管道写入数据
    ssize_t bytes_written = write(pipe_fd[1], message, strlen(message));
    if (bytes_written == -1) {
      perror("write");
      exit(EXIT_FAILURE);
    }

    close(pipe_fd[1]);  // 关闭写端

    // 等待子进程结束
    wait(NULL);
    exit(EXIT_SUCCESS);
  }
}
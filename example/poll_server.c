#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <errno.h>

#define PORT 8888
#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024
#define TIMEOUT 5000  // Poll timeout in milliseconds

int main() {
    int server_fd, new_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct pollfd fds[MAX_CLIENTS + 1];  // +1 for server listening socket
    int nfds = 1;                        // Track number of valid elements in pollfd array
    int activity;

    // Create server socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set port reuse
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // Bind address and port
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_fd, 5) == -1) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server started, listening on port %d...\n", PORT);

    // Initialize pollfd structure, server socket for listening
    fds[0].fd = server_fd;
    fds[0].events = POLLIN;  // Monitor for read events

    // Initialize other client positions
    for (int i = 1; i < MAX_CLIENTS + 1; i++) {
        fds[i].fd = -1;  // -1 indicates unused
        fds[i].events = POLLIN;
    }

    while (1) {
        // Call poll to wait for events
        activity = poll(fds, nfds, TIMEOUT);

        if (activity == -1) {
            if (errno == EINTR) continue;  // Interrupted by signal, continue loop
            perror("poll failed");
            exit(EXIT_FAILURE);
        }

        if (activity == 0) {
            // Timeout, no events
            continue;
        }

        // Check if server socket has new connections
        if (fds[0].revents & POLLIN) {
            if ((new_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len)) == -1) {
                perror("accept failed");
                continue;
            }

            printf("New connection: %s:%d (fd=%d)\n",
                   inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port),
                   new_fd);

            // Find free position to store new client
            int i;
            for (i = 1; i < MAX_CLIENTS + 1; i++) {
                if (fds[i].fd == -1) {
                    fds[i].fd = new_fd;
                    fds[i].events = POLLIN;  // Monitor for read events
                    if (i >= nfds) nfds = i + 1;  // Update number of valid elements
                    break;
                }
            }

            if (i == MAX_CLIENTS + 1) {
                printf("Client limit reached, connection refused\n");
                close(new_fd);
            }
        }

        // Handle client messages
        for (int i = 1; i < nfds; i++) {
            int fd = fds[i].fd;
            if (fd == -1) continue;

            // Check for read events
            if (fds[i].revents & POLLIN) {
                char buffer[BUFFER_SIZE] = {0};
                ssize_t bytes_read = recv(fd, buffer, BUFFER_SIZE - 1, 0);

                if (bytes_read <= 0) {
                    // Connection closed or error
                    getpeername(fd, (struct sockaddr*)&client_addr, &client_len);
                    printf("Client disconnected: %s:%d (fd=%d)\n",
                           inet_ntoa(client_addr.sin_addr),
                           ntohs(client_addr.sin_port),
                           fd);
                    close(fd);
                    fds[i].fd = -1;  // Mark as unused
                } else {
                    // Display and echo message
                    printf("Received message from fd=%d: %s\n", fd, buffer);
                    send(fd, buffer, bytes_read, 0);  // Echo message
                }
            }
        }
    }

    // Close server (won't actually reach here)
    close(server_fd);
    return 0;
}

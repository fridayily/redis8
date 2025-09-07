#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>

#define PORT 8888
#define BUFFER_SIZE 1024
#define NUM_MESSAGES 3  // Number of messages to send

// Preset messages to send
const char* messages[] = {
    "Hello from poll client",
    "This is message 2",
    "Final message, goodbye!"
};

int main(int argc, char *argv[]) {
    int sockfd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    struct pollfd fds[1];  // Only need to monitor socket
    int poll_ret;
    const char *hostname = "127.0.0.1";  // Default to localhost


    // Create client socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    // Convert IP address
    if (inet_pton(AF_INET, hostname, &server_addr.sin_addr) <= 0) {
        perror("invalid server IP");
        exit(EXIT_FAILURE);
    }

    // Connect to server
    printf("Connecting to server %s:%d...\n", hostname, PORT);
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("connection failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    printf("Successfully connected to server\n");

    // Initialize pollfd structure, monitor socket for read events
    fds[0].fd = sockfd;
    fds[0].events = POLLIN;

    // Send preset messages
    for (int i = 0; i < NUM_MESSAGES; i++) {
        // Send message
        if (send(sockfd, messages[i], strlen(messages[i]), 0) == -1) {
            perror("failed to send message");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        printf("Sent: %s\n", messages[i]);

        // Wait for server reply
        poll_ret = poll(fds, 1, 5000);  // 5 second timeout
        if (poll_ret == -1) {
            perror("poll failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        } else if (poll_ret == 0) {
            printf("Timeout waiting for reply\n");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        // Check if there is readable data
        if (fds[0].revents & POLLIN) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes_read = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
            if (bytes_read <= 0) {
                if (bytes_read < 0) perror("receive failed");
                else printf("Server disconnected\n");
                close(sockfd);
                exit(EXIT_FAILURE);
            }
            printf("Server reply: %s\n", buffer);
        }

        sleep(1);  // Small delay for better observation
    }

    // Close connection
    printf("All messages sent, disconnecting\n");
    close(sockfd);
    return 0;
}

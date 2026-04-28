#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/types.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 4242
#define MAX_NAME 64
#define MAX_MSG 1024

ssize_t send_all(int fd, const void *buf, size_t len);
int send_line(int fd, const char *line);
ssize_t recv_line(int fd, char *buf, size_t cap);
void trim_newline(char *s);

static volatile sig_atomic_t running = 1;
static int sockfd = -1;

static void on_signal(int sig) {
    (void)sig;
    running = 0;
    if (sockfd >= 0) {
        send_line(sockfd, "/exit");
        shutdown(sockfd, SHUT_RDWR);
    }
}

static void *receiver(void *arg) {
    int fd = *(int *)arg;
    char line[MAX_MSG + MAX_NAME + 64];
    while (running) {
        ssize_t n = recv_line(fd, line, sizeof(line));
        if (n <= 0) break;
        printf("%s\n", line);
        fflush(stdout);
    }
    running = 0;
    return NULL;
}

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    pthread_t th;
    if (pthread_create(&th, NULL, receiver, &sockfd) != 0) {
        perror("pthread_create");
        close(sockfd);
        return 1;
    }

    char input[MAX_MSG];
    while (running && fgets(input, sizeof(input), stdin)) {
        trim_newline(input);
        if (send_line(sockfd, input) < 0) break;
        if (strcmp(input, "/exit") == 0 || strcmp(input, "4") == 0) break;
    }

    running = 0;
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
    pthread_join(th, NULL);
    return 0;
}

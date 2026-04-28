#include <stddef.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

ssize_t send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

int send_line(int fd, const char *line) {
    if (!line) return -1;
    if (send_all(fd, line, strlen(line)) < 0) return -1;
    if (strlen(line) == 0 || line[strlen(line) - 1] != '\n') {
        if (send_all(fd, "\n", 1) < 0) return -1;
    }
    return 0;
}

ssize_t recv_line(int fd, char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    size_t pos = 0;
    while (pos + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            if (pos == 0) return 0;
            break;
        }
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (ssize_t)pos;
}

void trim_newline(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

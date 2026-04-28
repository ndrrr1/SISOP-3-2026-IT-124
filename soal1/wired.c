#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/types.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 4242
#define BACKLOG 32
#define MAX_NAME 64
#define MAX_MSG 1024
#define MAX_CLIENTS 64
#define ADMIN_NAME "The Knights"
#define ADMIN_PASSWORD "protocol7"

ssize_t send_all(int fd, const void *buf, size_t len);
int send_line(int fd, const char *line);
ssize_t recv_line(int fd, char *buf, size_t cap);
void trim_newline(char *s);

typedef struct {
    int fd;
    int active;
    int is_admin;
    char name[MAX_NAME];
    pthread_t thread;
} Client;

static Client clients[MAX_CLIENTS];
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t server_running = 1;
static int listen_fd = -1;
static time_t server_start;
static FILE *history_fp = NULL;

static void timestamp(char *buf, size_t cap) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void history_log(const char *actor, const char *fmt, ...) {
    pthread_mutex_lock(&log_lock);
    if (!history_fp) history_fp = fopen("history.log", "a");
    if (history_fp) {
        char ts[32];
        char event[MAX_MSG + MAX_NAME + 128];
        timestamp(ts, sizeof(ts));
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(event, sizeof(event), fmt, ap);
        va_end(ap);
        fprintf(history_fp, "[%s] [%s] [%s]\n", ts, actor ? actor : "System", event);
        fflush(history_fp);
    }
    pthread_mutex_unlock(&log_lock);
}

static void broadcast_msg(const char *line, int except_fd) {
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].fd != except_fd) send_line(clients[i].fd, line);
    }
    pthread_mutex_unlock(&clients_lock);
}

static int name_exists_locked(const char *name) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].name, name) == 0) return 1;
    }
    return 0;
}

static int add_client(int fd, const char *name, int is_admin, pthread_t thread) {
    pthread_mutex_lock(&clients_lock);
    int idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) { idx = i; break; }
    }
    if (idx >= 0) {
        clients[idx].fd = fd;
        clients[idx].active = 1;
        clients[idx].is_admin = is_admin;
        clients[idx].thread = thread;
        snprintf(clients[idx].name, sizeof(clients[idx].name), "%s", name);
    }
    pthread_mutex_unlock(&clients_lock);
    return idx;
}

static void remove_client(int fd, char *name_out, size_t name_cap, int *was_admin) {
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].fd == fd) {
            if (name_out) snprintf(name_out, name_cap, "%s", clients[i].name);
            if (was_admin) *was_admin = clients[i].is_admin;
            clients[i].active = 0;
            clients[i].fd = -1;
            clients[i].name[0] = '\0';
            clients[i].is_admin = 0;
            break;
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

static void close_all_clients(void) {
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            send_line(clients[i].fd, "[System] Disconnecting from The Wired...");
            shutdown(clients[i].fd, SHUT_RDWR);
            close(clients[i].fd);
            clients[i].active = 0;
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

static void send_admin_menu(int fd) {
    send_line(fd, "=== THE KNIGHTS CONSOLE ===");
    send_line(fd, "1. Check Active Entities (Users)");
    send_line(fd, "2. Check Server Uptime");
    send_line(fd, "3. Execute Emergency Shutdown");
    send_line(fd, "4. Disconnect");
    send_line(fd, "Command >>");
}

static void handle_admin_command(int fd, const char *cmd) {
    if (strcmp(cmd, "1") == 0 || strcmp(cmd, "/users") == 0) {
        char out[MAX_MSG];
        snprintf(out, sizeof(out), "[Admin] Active NAVI users:");
        send_line(fd, out);
        pthread_mutex_lock(&clients_lock);
        int count = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && !clients[i].is_admin) {
                snprintf(out, sizeof(out), " - %s", clients[i].name);
                send_line(fd, out);
                count++;
            }
        }
        pthread_mutex_unlock(&clients_lock);
        snprintf(out, sizeof(out), "Total NAVI users: %d", count);
        send_line(fd, out);
        history_log("Admin", "RPC_GET_USERS");
    } else if (strcmp(cmd, "2") == 0 || strcmp(cmd, "/uptime") == 0) {
        time_t diff = time(NULL) - server_start;
        char out[MAX_MSG];
        snprintf(out, sizeof(out), "[Admin] Server uptime: %ld seconds", (long)diff);
        send_line(fd, out);
        history_log("Admin", "RPC_GET_UPTIME");
    } else if (strcmp(cmd, "3") == 0 || strcmp(cmd, "/shutdown") == 0) {
        history_log("Admin", "RPC_SHUTDOWN");
        history_log("System", "EMERGENCY SHUTDOWN INITIATED");
        broadcast_msg("[System] EMERGENCY SHUTDOWN INITIATED", -1);
        server_running = 0;
        if (listen_fd >= 0) shutdown(listen_fd, SHUT_RDWR);
        close_all_clients();
    } else if (strcmp(cmd, "4") == 0 || strcmp(cmd, "/exit") == 0) {
        send_line(fd, "[System] Disconnecting from The Wired...");
    } else {
        send_line(fd, "[Admin] Unknown command. Use 1/2/3/4 or /users /uptime /shutdown /exit");
    }
}

static void *client_worker(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    char name[MAX_NAME];
    char pass[MAX_NAME];
    char line[MAX_MSG];
    int is_admin = 0;

    ssize_t r;
    while (1) {
        send_line(fd, "Enter your name:");
        r = recv_line(fd, name, sizeof(name));
        if (r <= 0) {
            close(fd);
            return NULL;
        }
        if (strlen(name) == 0) {
            send_line(fd, "[System] Invalid empty identity.");
            continue;
        }

        pthread_mutex_lock(&clients_lock);
        int duplicate = name_exists_locked(name);
        pthread_mutex_unlock(&clients_lock);
        if (!duplicate) break;

        snprintf(line, sizeof(line), "[System] The identity '%s' is already synchronized in The Wired.", name);
        send_line(fd, line);
        history_log("System", "Duplicate identity rejected: %s", name);
    }

    if (strcmp(name, ADMIN_NAME) == 0) {
        send_line(fd, "Enter Password:");
        r = recv_line(fd, pass, sizeof(pass));
        if (r <= 0 || strcmp(pass, ADMIN_PASSWORD) != 0) {
            send_line(fd, "[System] Authentication failed. Disconnecting...");
            history_log("System", "ADMIN_AUTH_FAIL");
            close(fd);
            return NULL;
        }
        is_admin = 1;
        send_line(fd, "[System] Authentication Successful. Granted Admin privileges.");
    }

    if (add_client(fd, name, is_admin, pthread_self()) < 0) {
        send_line(fd, "[System] The Wired is full. Try again later.");
        close(fd);
        return NULL;
    }

    snprintf(line, sizeof(line), "--- Welcome to The Wired, %s ---", name);
    send_line(fd, line);
    snprintf(line, sizeof(line), "[System] User '%s' connected", name);
    if (!is_admin) broadcast_msg(line, fd);
    history_log("System", "User '%s' connected", name);

    if (is_admin) send_admin_menu(fd);

    while (server_running) {
        r = recv_line(fd, line, sizeof(line));
        if (r <= 0) break;
        if (strlen(line) == 0) continue;

        if (is_admin) {
            handle_admin_command(fd, line);
            if (strcmp(line, "4") == 0 || strcmp(line, "/exit") == 0 || !server_running) break;
            send_admin_menu(fd);
            continue;
        }

        if (strcmp(line, "/exit") == 0) {
            send_line(fd, "[System] Disconnecting from The Wired...");
            break;
        }

        char out[MAX_MSG + MAX_NAME + 8];
        snprintf(out, sizeof(out), "[%s]: %s", name, line);
        broadcast_msg(out, fd);
        history_log("User", "[%s]: %s", name, line);
    }

    char gone[MAX_NAME] = "unknown";
    int was_admin = 0;
    remove_client(fd, gone, sizeof(gone), &was_admin);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    snprintf(line, sizeof(line), "[System] User '%s' disconnected", gone);
    if (!was_admin) broadcast_msg(line, -1);
    history_log("System", "User '%s' disconnected", gone);
    return NULL;
}

static void on_signal(int sig) {
    (void)sig;
    server_running = 0;
    if (listen_fd >= 0) shutdown(listen_fd, SHUT_RDWR);
}

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    server_start = time(NULL);
    history_fp = fopen("history.log", "a");
    history_log("System", "SERVER ONLINE");

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("The Wired is listening on %s:%d\n", SERVER_IP, SERVER_PORT);
    fflush(stdout);

    while (server_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR || !server_running) break;
            perror("accept");
            continue;
        }
        int *pfd = malloc(sizeof(int));
        if (!pfd) { close(cfd); continue; }
        *pfd = cfd;
        pthread_t th;
        if (pthread_create(&th, NULL, client_worker, pfd) != 0) {
            close(cfd);
            free(pfd);
            continue;
        }
        pthread_detach(th);
    }

    close_all_clients();
    if (listen_fd >= 0) close(listen_fd);
    history_log("System", "SERVER OFFLINE");
    if (history_fp) fclose(history_fp);
    return 0;
}

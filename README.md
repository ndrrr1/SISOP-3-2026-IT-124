# SISOP-3-2026-IT-124

* **Nama**  : Ndaru Satria Tama
* **NRP**   : 5027251124
* **Kelas** : C

---

## 1. Struktur Repository

```text
.
├── README.md
├── soal1
│   ├── navi.c
│   ├── protocol.c
│   └── wired.c
└── soal2
    ├── arena.h
    ├── eternal.c
    ├── Makefile
    └── orion.c
```

Struktur repository dibuat sederhana agar setiap soal berada pada folder masing-masing. Pada Soal 1, file pendukung yang digunakan hanya `protocol.c`. Pada Soal 2, konfigurasi struktur data IPC diletakkan di `arena.h`.

---

## 2. Pendahuluan

Pada Modul 3 Sistem Operasi 2026 ini, program yang dibuat berfokus pada komunikasi antarproses, sinkronisasi, socket programming, dan penggunaan IPC.

Secara umum:

* **Soal 1** membuat sistem client-server bernama **The Wired** menggunakan socket TCP dan pthread.
* **Soal 2** membuat game terminal bernama **Battle of Eterion** menggunakan Message Queue, Shared Memory, dan mutex.

Program juga dilengkapi error handling untuk beberapa kondisi, seperti client dijalankan tanpa server, username duplikat, password salah, akun aktif di session lain, gold tidak cukup, cooldown battle, dan server IPC belum berjalan.

---

# 3. Reporting Soal 1

## 3.1 Deskripsi

Soal 1 mengimplementasikan sistem komunikasi **The Wired**. Sistem ini terdiri dari server `wired` dan client `navi`.

Server `wired` menerima banyak client menggunakan socket TCP. Setiap client ditangani oleh thread berbeda sehingga komunikasi dapat berjalan secara bersamaan. Client biasa dapat mengirim pesan, lalu pesan tersebut dibroadcast ke client lain.

Terdapat user khusus bernama **The Knights** yang berperan sebagai admin. Admin harus login menggunakan password `protocol7`. Setelah berhasil login, admin dapat melihat user aktif, melihat uptime server, melakukan emergency shutdown, dan disconnect.

Fitur utama Soal 1:

1. Server menerima banyak client.
2. Client memasukkan identity saat masuk.
3. Identity client harus unik.
4. Pesan user dibroadcast ke user lain.
5. Admin login sebagai `The Knights`.
6. Admin memiliki menu khusus.
7. Aktivitas server dicatat ke `history.log`.
8. Error handling untuk server mati, username kosong, username duplikat, password admin salah, dan command admin tidak valid.

---

## 3.2 File yang Digunakan

| File | Fungsi |
|---|---|
| `wired.c` | Server utama The Wired |
| `navi.c` | Client NAVI |
| `protocol.c` | Helper komunikasi socket |

---

## 3.3 Kode Lengkap

### 3.3.1 `wired.c`

```c
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
```

### 3.3.2 `navi.c`

```c
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
```

### 3.3.3 `protocol.c`

```c
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
```

---

## 3.4 Cara Running dan Hasil per Tahap

### Tahap 1 - Masuk folder Soal 1

```bash
cd ~/Modul3/soal1
```

Hasil yang diharapkan:

```text
Terminal berada di folder soal1.
```

### Tahap 2 - Bersihkan file hasil run lama

```bash
rm -f wired navi *.o history.log
```

Hasil yang diharapkan:

```text
Tidak ada output jika berhasil.
```

### Tahap 3 - Compile program

```bash
gcc -Wall -Wextra -pthread -o wired wired.c protocol.c
gcc -Wall -Wextra -pthread -o navi navi.c protocol.c
```

Hasil yang diharapkan:

```text
Tidak ada error compile.
File wired dan navi berhasil dibuat.
```

Cek file:

```bash
ls
```

Hasil yang diharapkan:

```text
navi  navi.c  protocol.c  wired  wired.c
```

### Tahap 4 - Test error client tanpa server

Sebelum menjalankan server, jalankan client:

```bash
./navi
```

Hasil yang diharapkan:

```text
connect: Connection refused
```

Artinya error handling benar karena client tidak bisa connect saat server belum berjalan.

### Tahap 5 - Jalankan server

Terminal 1:

```bash
cd ~/Modul3/soal1
./wired
```

Hasil yang diharapkan:

```text
The Wired is listening on 127.0.0.1:4242
```

### Tahap 6 - Jalankan client user biasa

Terminal 2:

```bash
cd ~/Modul3/soal1
./navi
```

Input:

```text
alice
```

Hasil yang diharapkan:

```text
Enter your name:
--- Welcome to The Wired, alice ---
```

### Tahap 7 - Test chat antar client

Terminal 3:

```bash
cd ~/Modul3/soal1
./navi
```

Input:

```text
bob
```

Hasil yang diharapkan:

```text
--- Welcome to The Wired, bob ---
```

Di terminal `alice`, ketik:

```text
halo bob
```

Di terminal `bob`, hasil yang diharapkan:

```text
[alice]: halo bob
```

### Tahap 8 - Test username duplikat

Buka client baru:

```bash
./navi
```

Input username yang sudah dipakai:

```text
alice
```

Hasil yang diharapkan:

```text
[System] The identity 'alice' is already synchronized in The Wired.
Enter your name:
```

Masukkan nama lain:

```text
charlie
```

Hasil yang diharapkan:

```text
--- Welcome to The Wired, charlie ---
```

### Tahap 9 - Test admin login benar

Buka client baru:

```bash
./navi
```

Input nama:

```text
The Knights
```

Input password:

```text
protocol7
```

Hasil yang diharapkan:

```text
[System] Authentication Successful. Granted Admin privileges.
=== THE KNIGHTS CONSOLE ===
1. Check Active Entities (Users)
2. Check Server Uptime
3. Execute Emergency Shutdown
4. Disconnect
Command >>
```

### Tahap 10 - Test admin cek user aktif

Input:

```text
1
```

Hasil yang diharapkan:

```text
[Admin] Active NAVI users:
 - alice
 - bob
 - charlie
Total NAVI users: 3
```

Jumlah user dapat berbeda tergantung client yang masih aktif.

### Tahap 11 - Test admin cek uptime

Input:

```text
2
```

Hasil yang diharapkan:

```text
[Admin] Server uptime: <angka> seconds
```

### Tahap 12 - Test password admin salah

Buka client baru:

```bash
./navi
```

Input:

```text
The Knights
salah
```

Hasil yang diharapkan:

```text
[System] Authentication failed. Disconnecting...
```

### Tahap 13 - Test command admin tidak valid

Di menu admin, input:

```text
9
```

Hasil yang diharapkan:

```text
[Admin] Unknown command.
```

### Tahap 14 - Test emergency shutdown

Di menu admin, input:

```text
3
```

Hasil yang diharapkan di client lain:

```text
[System] EMERGENCY SHUTDOWN INITIATED
[System] Disconnecting from The Wired...
```

### Tahap 15 - Cek history log

```bash
cat history.log
```

Contoh hasil:

```text
[YYYY-MM-DD HH:MM:SS] [System] [SERVER ONLINE]
[YYYY-MM-DD HH:MM:SS] [System] [User 'alice' connected]
[YYYY-MM-DD HH:MM:SS] [User] [[alice]: halo bob]
[YYYY-MM-DD HH:MM:SS] [Admin] [RPC_GET_USERS]
[YYYY-MM-DD HH:MM:SS] [Admin] [RPC_GET_UPTIME]
[YYYY-MM-DD HH:MM:SS] [System] [EMERGENCY SHUTDOWN INITIATED]
```

---

## 3.5 Penjelasan Kode per Bagian

### 3.5.1 `wired.c`

`wired.c` adalah server utama. File ini bertugas membuka socket, menerima koneksi client, membuat thread untuk setiap client, mengatur daftar client aktif, menangani admin, melakukan broadcast chat, dan mencatat log ke `history.log`.

Bagian konfigurasi:

```c
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 4242
#define BACKLOG 32
#define MAX_NAME 64
#define MAX_MSG 1024
#define MAX_CLIENTS 64
#define ADMIN_NAME "The Knights"
#define ADMIN_PASSWORD "protocol7"
```

Konfigurasi tersebut menentukan IP server, port, batas antrian, panjang nama, panjang pesan, jumlah maksimal client, serta data login admin.

Struct `Client` menyimpan data setiap client:

```c
typedef struct {
    int fd;
    int active;
    int is_admin;
    char name[MAX_NAME];
    pthread_t thread;
} Client;
```

Fungsi penting pada `wired.c`:

| Fungsi | Penjelasan |
|---|---|
| `timestamp()` | Membuat timestamp untuk log |
| `history_log()` | Menulis aktivitas server ke `history.log` |
| `broadcast_msg()` | Mengirim pesan ke semua client aktif selain pengirim |
| `name_exists_locked()` | Mengecek apakah username sudah dipakai |
| `add_client()` | Menambahkan client ke daftar client aktif |
| `remove_client()` | Menghapus client dari daftar aktif |
| `send_admin_menu()` | Menampilkan menu admin |
| `handle_admin_command()` | Menjalankan command admin |
| `client_worker()` | Thread handler untuk setiap client |
| `close_all_clients()` | Menutup semua client saat shutdown |
| `main()` | Membuat socket server dan menerima koneksi |

Server menggunakan mutex agar data client dan log aman dari race condition.

### 3.5.2 `navi.c`

`navi.c` adalah client. Program ini bertugas membuat koneksi ke server, menerima pesan dari server, dan mengirim input user.

Alur utama `navi.c`:

1. Membuat socket TCP.
2. Menghubungkan socket ke server.
3. Membuat thread receiver.
4. Membaca input dari user.
5. Mengirim input ke server.
6. Keluar jika user mengetik `/exit`.

Thread receiver membuat client bisa menerima pesan dari server sambil tetap bisa mengetik input.

### 3.5.3 `protocol.c`

`protocol.c` adalah file helper untuk komunikasi socket.

Fungsi penting:

| Fungsi | Penjelasan |
|---|---|
| `send_all()` | Memastikan seluruh byte terkirim ke socket |
| `send_line()` | Mengirim pesan string dan menambahkan newline jika perlu |
| `recv_line()` | Membaca pesan dari socket sampai newline |
| `trim_newline()` | Menghapus newline dari input |

File ini membuat `wired.c` dan `navi.c` lebih rapi karena logic kirim/terima pesan tidak ditulis berulang.

---

# 4. Reporting Soal 2

## 4.1 Deskripsi

Soal 2 mengimplementasikan game terminal **Battle of Eterion**. Program terdiri dari server `orion` dan client `eternal`.

Komunikasi antara client dan server menggunakan IPC:

* **Message Queue** untuk request-response.
* **Shared Memory** untuk menyimpan data arena bersama.
* **Process-shared Mutex** untuk mencegah race condition.

Fitur utama Soal 2:

1. Register player.
2. Login player.
3. Profile player.
4. Armory dan pembelian weapon.
5. Match history.
6. Matchmaking player.
7. Bot muncul jika tidak ada lawan setelah 35 detik.
8. Battle realtime dengan tombol `a`, `u`, dan `q`.
9. Reward XP dan gold setelah battle.
10. Error handling untuk server mati, password salah, akun aktif, gold kurang, weapon sudah dimiliki, cooldown, dan ultimate tanpa weapon.

---

## 4.2 File yang Digunakan

| File | Fungsi |
|---|---|
| `arena.h` | Konfigurasi, struktur data, key IPC, command, response, weapon |
| `orion.c` | Server utama game |
| `eternal.c` | Client player |
| `Makefile` | Compile dan cleanup IPC |

---

## 4.3 Kode Lengkap

### 4.3.1 `arena.h`

```c
#ifndef ARENA_H
#define ARENA_H

#include <pthread.h>
#include <time.h>
#include <sys/types.h>

#define MSG_KEY 0x0001234
#define SHM_KEY 0x0005678
#define MAX_USERS 64
#define MAX_BATTLES 32
#define MAX_HISTORY 16
#define MAX_LOGS 5
#define NAME_LEN 32
#define PASS_LEN 32
#define TEXT_LEN 4096
#define BASE_DAMAGE 10
#define BASE_HEALTH 100
#define MATCH_TIMEOUT 35

#define CMD_PING 0
#define CMD_REGISTER 1
#define CMD_LOGIN 2
#define CMD_LOGOUT 3
#define CMD_PROFILE 4
#define CMD_ARMORY 5
#define CMD_HISTORY 6
#define CMD_ENTER_BATTLE 7
#define CMD_BATTLE_STATUS 8
#define CMD_ATTACK 9
#define CMD_ULTIMATE 10
#define CMD_FORFEIT 11
#define CMD_SHUTDOWN 99

#define RSP_OK 0
#define RSP_ERR 1
#define RSP_WAITING 2
#define RSP_BATTLE 3
#define RSP_ENDED 4

typedef struct {
    char name[32];
    int cost;
    int dmg;
} Weapon;

static const Weapon WEAPONS[] = {
    {"Wood Sword", 100, 5},
    {"Iron Sword", 300, 15},
    {"Steel Axe", 600, 30},
    {"Demon Blade", 1500, 60},
    {"God Slayer", 5000, 150}
};
#define WEAPON_COUNT ((int)(sizeof(WEAPONS) / sizeof(WEAPONS[0])))

typedef struct {
    char time_str[16];
    char opponent[NAME_LEN];
    char result[8];
    int xp_gain;
} MatchHistory;

typedef struct {
    int used;
    char username[NAME_LEN];
    char password[PASS_LEN];
    int gold;
    int xp;
    int level;
    int weapons[5];
    pid_t active_pid;
    int battle_id;
    MatchHistory history[MAX_HISTORY];
    int history_count;
} User;

typedef struct {
    int active;        /* 0 empty, 1 running, 2 ended */
    int is_bot;
    int p1_idx, p2_idx;
    pid_t p1_pid, p2_pid;
    char p1_name[NAME_LEN], p2_name[NAME_LEN];
    int p1_hp, p2_hp;
    int p1_maxhp, p2_maxhp;
    time_t start_time;
    time_t last_p1_attack, last_p2_attack, last_bot_attack;
    char logs[MAX_LOGS][128];
    int log_count;
    int p1_result;    /* 1 win, -1 lose, 0 none */
    int p2_result;
    int p1_seen, p2_seen;
} Battle;

typedef struct {
    pthread_mutex_t mutex;
    time_t boot_time;
    pid_t orion_pid;
    User users[MAX_USERS];
    Battle battles[MAX_BATTLES];
    pid_t waiting_pid;
    int waiting_user_idx;
    time_t waiting_since;
} Arena;

typedef struct {
    long mtype;
    pid_t pid;
    int cmd;
    int value;
    char username[NAME_LEN];
    char password[PASS_LEN];
} Request;

typedef struct {
    long mtype;
    int status;
    int logged_in;
    char username[NAME_LEN];
    char text[TEXT_LEN];
} Response;

#define REQ_SIZE (sizeof(Request) - sizeof(long))
#define RSP_SIZE (sizeof(Response) - sizeof(long))

#endif
```

### 4.3.2 `orion.c`

```c
#include "arena.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int msg_id = -1;
static int shm_id = -1;
static Arena *arena = NULL;
static volatile sig_atomic_t running = 1;
static const char *DATA_DIR = "data";
static const char *PLAYERS_FILE = "data/players.db";
static const char *HISTORY_FILE = "data/matches.log";

static void now_hhmm(char *buf, size_t cap) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(buf, cap, "%H:%M", &tmv);
}

static void append_text(char *dst, size_t cap, const char *fmt, ...) {
    size_t len = strlen(dst);
    if (len >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst + len, cap - len, fmt, ap);
    va_end(ap);
}


static void hp_bar(char *dst, size_t cap, int hp, int max_hp) {
    if (max_hp <= 0) max_hp = 1;
    if (hp < 0) hp = 0;
    if (hp > max_hp) hp = max_hp;
    int filled = (hp * 20) / max_hp;
    append_text(dst, cap, "[");
    for (int i = 0; i < 20; i++) append_text(dst, cap, "%c", i < filled ? '#' : '-');
    append_text(dst, cap, "] %3d/%-3d", hp, max_hp);
}

static void format_waiting(int seconds, char *out, size_t cap) {
    if (seconds < 0) seconds = 0;
    int filled = ((MATCH_TIMEOUT - seconds) * 24) / MATCH_TIMEOUT;
    if (filled < 0) filled = 0;
    if (filled > 24) filled = 24;
    snprintf(out, cap,
        "\033[36m"
        "+====================================================+\n"
        "|                 MATCHMAKING GATE                  |\n"
        "+====================================================+\033[0m\n"
        "Searching for an opponent... [%2d s]\n\n[", seconds);
    for (int i = 0; i < 24; i++) append_text(out, cap, "%c", i < filled ? '#' : '.');
    append_text(out, cap, "]\n\nIf no human warrior appears in 35 seconds, a monster will enter the arena.\n");
}

static int valid_name(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= NAME_LEN) return 0;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-') return 0;
    }
    return 1;
}

static int find_user(const char *username) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (arena->users[i].used && strcmp(arena->users[i].username, username) == 0) return i;
    }
    return -1;
}

static int find_user_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (arena->users[i].used && arena->users[i].active_pid == pid) return i;
    }
    return -1;
}

static int pid_alive(pid_t pid) {
    if (pid <= 0) return 0;
    if (kill(pid, 0) == 0) return 1;
    return errno != ESRCH;
}

static int best_weapon_damage(const User *u) {
    int best = 0;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        if (u->weapons[i] && WEAPONS[i].dmg > best) best = WEAPONS[i].dmg;
    }
    return best;
}

static const char *best_weapon_name(const User *u) {
    int best = -1;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        if (u->weapons[i] && (best < 0 || WEAPONS[i].dmg > WEAPONS[best].dmg)) best = i;
    }
    return best < 0 ? "None" : WEAPONS[best].name;
}

static int total_damage(const User *u) {
    return BASE_DAMAGE + (u->xp / 50) + best_weapon_damage(u);
}

static int max_health(const User *u) {
    return BASE_HEALTH + (u->xp / 10);
}

static void recalc_level(User *u) {
    u->level = 1 + (u->xp / 100);
}

static void push_battle_log(Battle *b, const char *fmt, ...) {
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (b->log_count < MAX_LOGS) {
        snprintf(b->logs[b->log_count++], sizeof(b->logs[0]), "%s", line);
    } else {
        memmove(b->logs[0], b->logs[1], (MAX_LOGS - 1) * sizeof(b->logs[0]));
        snprintf(b->logs[MAX_LOGS - 1], sizeof(b->logs[0]), "%s", line);
    }
}

static void ensure_data_dir(void) {
    mkdir(DATA_DIR, 0777);
}

static void save_users(void) {
    ensure_data_dir();
    FILE *fp = fopen(PLAYERS_FILE, "w");
    if (!fp) return;
    for (int i = 0; i < MAX_USERS; i++) {
        User *u = &arena->users[i];
        if (!u->used) continue;
        fprintf(fp, "U|%s|%s|%d|%d|%d|%d,%d,%d,%d,%d\n",
                u->username, u->password, u->gold, u->xp, u->level,
                u->weapons[0], u->weapons[1], u->weapons[2], u->weapons[3], u->weapons[4]);
        for (int j = 0; j < u->history_count && j < MAX_HISTORY; j++) {
            MatchHistory *h = &u->history[j];
            fprintf(fp, "H|%s|%s|%s|%d\n", h->time_str, h->opponent, h->result, h->xp_gain);
        }
        fprintf(fp, "E\n");
    }
    fclose(fp);
}

static void load_users(void) {
    ensure_data_dir();
    FILE *fp = fopen(PLAYERS_FILE, "r");
    if (!fp) return;
    char line[512];
    int cur = -1;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "U|", 2) == 0) {
            int slot = -1;
            for (int i = 0; i < MAX_USERS; i++) if (!arena->users[i].used) { slot = i; break; }
            if (slot < 0) break;
            User *u = &arena->users[slot];
            memset(u, 0, sizeof(*u));
            u->used = 1;
            char *p = line + 2;
            char *tok = strtok(p, "|"); if (!tok) continue; snprintf(u->username, sizeof(u->username), "%s", tok);
            tok = strtok(NULL, "|"); if (!tok) continue; snprintf(u->password, sizeof(u->password), "%s", tok);
            tok = strtok(NULL, "|"); if (!tok) continue; u->gold = atoi(tok);
            tok = strtok(NULL, "|"); if (!tok) continue; u->xp = atoi(tok);
            tok = strtok(NULL, "|"); if (!tok) continue; u->level = atoi(tok);
            tok = strtok(NULL, "|"); if (tok) sscanf(tok, "%d,%d,%d,%d,%d", &u->weapons[0], &u->weapons[1], &u->weapons[2], &u->weapons[3], &u->weapons[4]);
            u->active_pid = 0;
            u->battle_id = -1;
            cur = slot;
        } else if (strncmp(line, "H|", 2) == 0 && cur >= 0) {
            User *u = &arena->users[cur];
            if (u->history_count >= MAX_HISTORY) continue;
            MatchHistory *h = &u->history[u->history_count++];
            char *p = line + 2;
            char *tok = strtok(p, "|"); if (!tok) continue; snprintf(h->time_str, sizeof(h->time_str), "%s", tok);
            tok = strtok(NULL, "|"); if (!tok) continue; snprintf(h->opponent, sizeof(h->opponent), "%s", tok);
            tok = strtok(NULL, "|"); if (!tok) continue; snprintf(h->result, sizeof(h->result), "%s", tok);
            tok = strtok(NULL, "|"); if (tok) h->xp_gain = atoi(tok);
        } else if (strcmp(line, "E") == 0) {
            cur = -1;
        }
    }
    fclose(fp);
}

static void add_history(User *u, const char *opponent, const char *result, int xp_gain) {
    if (!u || !u->used) return;
    if (u->history_count >= MAX_HISTORY) {
        for (int i = 1; i < MAX_HISTORY; i++) u->history[i - 1] = u->history[i];
        u->history_count = MAX_HISTORY - 1;
    }
    MatchHistory *h = &u->history[u->history_count++];
    now_hhmm(h->time_str, sizeof(h->time_str));
    snprintf(h->opponent, sizeof(h->opponent), "%s", opponent);
    snprintf(h->result, sizeof(h->result), "%s", result);
    h->xp_gain = xp_gain;

    FILE *fp = fopen(HISTORY_FILE, "a");
    if (fp) {
        fprintf(fp, "%s|%s|%s|%s|%d\n", h->time_str, u->username, opponent, result, xp_gain);
        fclose(fp);
    }
}

static void send_response(pid_t pid, int status, const char *username, const char *fmt, ...) {
    Response rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.mtype = pid;
    rsp.status = status;
    rsp.logged_in = (username && username[0]) ? 1 : 0;
    if (username) snprintf(rsp.username, sizeof(rsp.username), "%s", username);
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(rsp.text, sizeof(rsp.text), fmt, ap);
        va_end(ap);
    }
    if (msgsnd(msg_id, &rsp, RSP_SIZE, 0) < 0) perror("msgsnd response");
}

static void format_profile(User *u, char *out, size_t cap) {
    snprintf(out, cap,
        "\033[36m"
        "+====================== PROFILE ======================+\n"
        "\033[0m"
        "| Name   : %-43s |\n"
        "| Gold   : %-43d |\n"
        "| Lvl    : %-43d |\n"
        "| XP     : %-43d |\n"
        "| Health : %-43d |\n"
        "| Damage : %-43d |\n"
        "| Weapon : %-43s |\n"
        "\033[36m+=====================================================+\033[0m\n",
        u->username, u->gold, u->level, u->xp, max_health(u), total_damage(u), best_weapon_name(u));
}

static void format_armory(User *u, char *out, size_t cap) {
    snprintf(out, cap,
        "\033[33m"
        "+======================= ARMORY ======================+\n"
        "\033[0m"
        "| Gold: %-46d |\n"
        "+----+---------------+---------+----------+----------+\n"
        "| No | Weapon        | Price   | Bonus    | Status   |\n"
        "+----+---------------+---------+----------+----------+\n",
        u->gold);
    for (int i = 0; i < WEAPON_COUNT; i++) {
        append_text(out, cap, "| %-2d | %-13s | %5d G | +%-6d | %-8s |\n",
                    i + 1, WEAPONS[i].name, WEAPONS[i].cost, WEAPONS[i].dmg,
                    u->weapons[i] ? "OWNED" : "BUY");
    }
    append_text(out, cap,
        "+----+---------------+---------+----------+----------+\n"
        "| 0  | Back                                            |\n"
        "+=====================================================+\n");
}

static void format_history(User *u, char *out, size_t cap) {
    snprintf(out, cap,
        "\033[35m"
        "+==================== MATCH HISTORY ==================+\n"
        "\033[0m"
        "| %-8s | %-18s | %-6s | %-8s |\n"
        "+----------+--------------------+--------+----------+\n",
        "Time", "Opponent", "Res", "XP");
    if (u->history_count == 0) {
        append_text(out, cap, "| No match history yet.                              |\n");
        append_text(out, cap, "+=====================================================+\n");
        return;
    }
    int start = u->history_count > 10 ? u->history_count - 10 : 0;
    for (int i = start; i < u->history_count; i++) {
        MatchHistory *h = &u->history[i];
        append_text(out, cap, "| %-8s | %-18s | %-6s | %+4d XP  |\n", h->time_str, h->opponent, h->result, h->xp_gain);
    }
    append_text(out, cap, "+=====================================================+\n");
}

static int allocate_battle(void) {
    for (int i = 0; i < MAX_BATTLES; i++) {
        if (arena->battles[i].active == 0) return i;
    }
    return -1;
}

static void start_battle(int idx1, int idx2, int is_bot) {
    int bid = allocate_battle();
    if (bid < 0) return;
    Battle *b = &arena->battles[bid];
    memset(b, 0, sizeof(*b));
    b->active = 1;
    b->is_bot = is_bot;
    b->p1_idx = idx1;
    b->p2_idx = idx2;
    b->p1_pid = arena->users[idx1].active_pid;
    b->p2_pid = is_bot ? 0 : arena->users[idx2].active_pid;
    snprintf(b->p1_name, sizeof(b->p1_name), "%s", arena->users[idx1].username);
    snprintf(b->p2_name, sizeof(b->p2_name), "%s", is_bot ? "W1ld Beast" : arena->users[idx2].username);
    b->p1_maxhp = max_health(&arena->users[idx1]);
    b->p2_maxhp = is_bot ? (BASE_HEALTH + arena->users[idx1].level * 10) : max_health(&arena->users[idx2]);
    b->p1_hp = b->p1_maxhp;
    b->p2_hp = b->p2_maxhp;
    b->start_time = time(NULL);
    b->last_p1_attack = b->last_p2_attack = b->last_bot_attack = 0;
    push_battle_log(b, "Match started: %s VS %s", b->p1_name, b->p2_name);
    arena->users[idx1].battle_id = bid;
    if (!is_bot) arena->users[idx2].battle_id = bid;
}

static void apply_rewards(Battle *b) {
    User *p1 = &arena->users[b->p1_idx];
    User *p2 = b->is_bot ? NULL : &arena->users[b->p2_idx];
    if (b->p1_result == 1) {
        p1->xp += 50; p1->gold += 120; recalc_level(p1); add_history(p1, b->p2_name, "WIN", 50);
        if (p2) { p2->xp += 15; p2->gold += 30; recalc_level(p2); add_history(p2, b->p1_name, "LOSS", 15); }
    } else if (b->p1_result == -1) {
        p1->xp += 15; p1->gold += 30; recalc_level(p1); add_history(p1, b->p2_name, "LOSS", 15);
        if (p2) { p2->xp += 50; p2->gold += 120; recalc_level(p2); add_history(p2, b->p1_name, "WIN", 50); }
    }
    save_users();
}

static void finish_battle(Battle *b, int p1_won) {
    if (!b || b->active != 1) return;
    b->active = 2;
    b->p1_result = p1_won ? 1 : -1;
    b->p2_result = p1_won ? -1 : 1;
    push_battle_log(b, p1_won ? "%s wins the battle!" : "%s wins the battle!", p1_won ? b->p1_name : b->p2_name);
    apply_rewards(b);
}

static void maybe_bot_attack(Battle *b) {
    if (!b || b->active != 1 || !b->is_bot) return;
    time_t now = time(NULL);
    if (b->last_bot_attack == 0) b->last_bot_attack = now;
    if (now - b->last_bot_attack >= 2) {
        int dmg = 7 + arena->users[b->p1_idx].level;
        b->p1_hp -= dmg;
        if (b->p1_hp < 0) b->p1_hp = 0;
        push_battle_log(b, "%s hits %s for %d damage!", b->p2_name, b->p1_name, dmg);
        b->last_bot_attack = now;
        if (b->p1_hp <= 0) finish_battle(b, 0);
    }
}

static void format_battle_for_user(Battle *b, pid_t pid, char *out, size_t cap) {
    int user_is_p1 = (pid == b->p1_pid);
    const char *me = user_is_p1 ? b->p1_name : b->p2_name;
    const char *op = user_is_p1 ? b->p2_name : b->p1_name;
    int my_hp = user_is_p1 ? b->p1_hp : b->p2_hp;
    int op_hp = user_is_p1 ? b->p2_hp : b->p1_hp;
    int my_max = user_is_p1 ? b->p1_maxhp : b->p2_maxhp;
    int op_max = user_is_p1 ? b->p2_maxhp : b->p1_maxhp;
    User *me_user = &arena->users[user_is_p1 ? b->p1_idx : b->p2_idx];
    User *op_user = (!b->is_bot) ? &arena->users[user_is_p1 ? b->p2_idx : b->p1_idx] : NULL;
    const char *op_weapon = op_user ? best_weapon_name(op_user) : "Claws";
    int my_dmg = total_damage(me_user);
    int op_dmg = op_user ? total_damage(op_user) : (7 + me_user->level);

    snprintf(out, cap,
        "\033[35m"
        "+========================= ARENA =========================+\n"
        "\033[0m");
    append_text(out, cap, "| %-18s Lvl %-3d Weapon: %-18s |\n", me, me_user->level, best_weapon_name(me_user));
    append_text(out, cap, "| HP  "); hp_bar(out, cap, my_hp, my_max); append_text(out, cap, "  DMG: %-4d      |\n", my_dmg);
    append_text(out, cap, "|--------------------------- VS --------------------------|\n");
    append_text(out, cap, "| %-18s Lvl %-3d Weapon: %-18s |\n", op, op_user ? op_user->level : me_user->level, op_weapon);
    append_text(out, cap, "| HP  "); hp_bar(out, cap, op_hp, op_max); append_text(out, cap, "  DMG: %-4d      |\n", op_dmg);
    append_text(out, cap,
        "+---------------------- COMBAT LOG -----------------------+\n");
    for (int i = 0; i < b->log_count; i++) append_text(out, cap, "| - %-54s |\n", b->logs[i]);
    if (b->log_count == 0) append_text(out, cap, "| - %-54s |\n", "The arena is silent...");
    append_text(out, cap,
        "+---------------------------------------------------------+\n"
        "| [a] Attack  |  [u] Ultimate  |  [q] Give up            |\n"
        "+=========================================================+\n");
}

static int deliver_finished(Battle *b, pid_t pid, char *out, size_t cap) {
    int user_is_p1 = (pid == b->p1_pid);
    int result = user_is_p1 ? b->p1_result : b->p2_result;
    snprintf(out, cap, result == 1 ?
        "\033[32m+====================== VICTORY ======================+\033[0m\n" :
        "\033[31m+====================== DEFEAT =======================+\033[0m\n");
    for (int i = 0; i < b->log_count; i++) append_text(out, cap, "| - %-49s |\n", b->logs[i]);
    append_text(out, cap,
        "+=====================================================+\n"
        "Battle ended. Press ENTER to continue.\n");
    if (user_is_p1) {
        arena->users[b->p1_idx].battle_id = -1;
        b->p1_seen = 1;
    } else {
        if (!b->is_bot) arena->users[b->p2_idx].battle_id = -1;
        b->p2_seen = 1;
    }
    if (b->is_bot || (b->p1_seen && b->p2_seen)) b->active = 0;
    return result;
}

static void clear_pid_state(pid_t pid) {
    int idx = find_user_by_pid(pid);
    if (idx < 0) return;
    User *u = &arena->users[idx];
    if (arena->waiting_pid == pid) {
        arena->waiting_pid = 0;
        arena->waiting_user_idx = -1;
    }
    if (u->battle_id >= 0 && u->battle_id < MAX_BATTLES) {
        Battle *b = &arena->battles[u->battle_id];
        if (b->active == 1) {
            int p1_left = (pid == b->p1_pid);
            push_battle_log(b, "%s left the arena.", p1_left ? b->p1_name : b->p2_name);
            finish_battle(b, p1_left ? 0 : 1);
        }
    }
    u->active_pid = 0;
    u->battle_id = -1;
    save_users();
}

static void handle_register(Request *req) {
    if (!valid_name(req->username)) {
        send_response(req->pid, RSP_ERR, NULL, "Username invalid. Use letters, numbers, '_' or '-' only.");
        return;
    }
    if (strlen(req->password) == 0 || strlen(req->password) >= PASS_LEN) {
        send_response(req->pid, RSP_ERR, NULL, "Password invalid or too long.");
        return;
    }
    if (find_user(req->username) >= 0) {
        send_response(req->pid, RSP_ERR, NULL, "Username '%s' already registered.", req->username);
        return;
    }
    int slot = -1;
    for (int i = 0; i < MAX_USERS; i++) if (!arena->users[i].used) { slot = i; break; }
    if (slot < 0) {
        send_response(req->pid, RSP_ERR, NULL, "Eterion is full. Registration rejected.");
        return;
    }
    User *u = &arena->users[slot];
    memset(u, 0, sizeof(*u));
    u->used = 1;
    snprintf(u->username, sizeof(u->username), "%s", req->username);
    snprintf(u->password, sizeof(u->password), "%s", req->password);
    u->gold = 150;
    u->xp = 0;
    u->level = 1;
    u->battle_id = -1;
    save_users();
    send_response(req->pid, RSP_OK, NULL, "Account created! Default stats: Gold=150, Lvl=1, XP=0.");
}

static void handle_login(Request *req) {
    int idx = find_user(req->username);
    if (idx < 0 || strcmp(arena->users[idx].password, req->password) != 0) {
        send_response(req->pid, RSP_ERR, NULL, "Login failed. Username or password is wrong.");
        return;
    }
    User *u = &arena->users[idx];
    if (u->active_pid && !pid_alive(u->active_pid)) u->active_pid = 0;
    if (u->active_pid && u->active_pid != req->pid) {
        send_response(req->pid, RSP_ERR, NULL, "This account is already active in another session.");
        return;
    }
    u->active_pid = req->pid;
    u->battle_id = -1;
    save_users();
    char profile[TEXT_LEN];
    format_profile(u, profile, sizeof(profile));
    send_response(req->pid, RSP_OK, u->username, "Welcome!\n%s", profile);
}

static void require_login(Request *req, int *idx_out) {
    int idx = find_user_by_pid(req->pid);
    if (idx < 0) {
        send_response(req->pid, RSP_ERR, NULL, "You are not logged in.");
        *idx_out = -1;
    } else {
        *idx_out = idx;
    }
}

static void handle_enter_battle(Request *req) {
    int idx; require_login(req, &idx); if (idx < 0) return;
    User *u = &arena->users[idx];
    if (u->battle_id >= 0) {
        Battle *b = &arena->battles[u->battle_id];
        char out[TEXT_LEN] = "";
        if (b->active == 2) {
            deliver_finished(b, req->pid, out, sizeof(out));
            send_response(req->pid, RSP_ENDED, u->username, "%s", out);
        } else {
            format_battle_for_user(b, req->pid, out, sizeof(out));
            send_response(req->pid, RSP_BATTLE, u->username, "%s", out);
        }
        return;
    }
    if (arena->waiting_pid == req->pid) {
        int remain = MATCH_TIMEOUT - (int)(time(NULL) - arena->waiting_since);
        if (remain < 0) remain = 0;
        char wait_out[TEXT_LEN]; format_waiting(remain, wait_out, sizeof(wait_out)); send_response(req->pid, RSP_WAITING, u->username, "%s", wait_out);
        return;
    }
    if (arena->waiting_pid > 0 && arena->waiting_user_idx >= 0 && arena->waiting_pid != req->pid && pid_alive(arena->waiting_pid)) {
        int other = arena->waiting_user_idx;
        arena->waiting_pid = 0;
        arena->waiting_user_idx = -1;
        start_battle(other, idx, 0);
        Battle *b = &arena->battles[u->battle_id];
        char out[TEXT_LEN] = "";
        format_battle_for_user(b, req->pid, out, sizeof(out));
        send_response(req->pid, RSP_BATTLE, u->username, "%s", out);
        return;
    }
    arena->waiting_pid = req->pid;
    arena->waiting_user_idx = idx;
    arena->waiting_since = time(NULL);
    char wait_out[TEXT_LEN]; format_waiting(MATCH_TIMEOUT, wait_out, sizeof(wait_out)); send_response(req->pid, RSP_WAITING, u->username, "%s", wait_out);
}

static void handle_battle_status(Request *req) {
    int idx; require_login(req, &idx); if (idx < 0) return;
    User *u = &arena->users[idx];
    if (arena->waiting_pid == req->pid) {
        int elapsed = (int)(time(NULL) - arena->waiting_since);
        if (elapsed >= MATCH_TIMEOUT) {
            arena->waiting_pid = 0;
            arena->waiting_user_idx = -1;
            start_battle(idx, -1, 1);
            Battle *b = &arena->battles[u->battle_id];
            push_battle_log(b, "No human found. Monster appears!");
            char out[TEXT_LEN] = "";
            format_battle_for_user(b, req->pid, out, sizeof(out));
            send_response(req->pid, RSP_BATTLE, u->username, "%s", out);
        } else {
            char wait_out[TEXT_LEN]; format_waiting(MATCH_TIMEOUT - elapsed, wait_out, sizeof(wait_out)); send_response(req->pid, RSP_WAITING, u->username, "%s", wait_out);
        }
        return;
    }
    if (u->battle_id < 0) {
        send_response(req->pid, RSP_ERR, u->username, "No active battle.");
        return;
    }
    Battle *b = &arena->battles[u->battle_id];
    if (b->active == 1) maybe_bot_attack(b);
    char out[TEXT_LEN] = "";
    if (b->active == 2) {
        deliver_finished(b, req->pid, out, sizeof(out));
        send_response(req->pid, RSP_ENDED, u->username, "%s", out);
    } else {
        format_battle_for_user(b, req->pid, out, sizeof(out));
        send_response(req->pid, RSP_BATTLE, u->username, "%s", out);
    }
}

static void handle_attack(Request *req, int ultimate) {
    int idx; require_login(req, &idx); if (idx < 0) return;
    User *u = &arena->users[idx];
    if (u->battle_id < 0) {
        send_response(req->pid, RSP_ERR, u->username, "No active battle.");
        return;
    }
    Battle *b = &arena->battles[u->battle_id];
    if (b->active == 2) {
        char out[TEXT_LEN] = "";
        deliver_finished(b, req->pid, out, sizeof(out));
        send_response(req->pid, RSP_ENDED, u->username, "%s", out);
        return;
    }
    int user_is_p1 = (req->pid == b->p1_pid);
    time_t now = time(NULL);
    time_t *last = user_is_p1 ? &b->last_p1_attack : &b->last_p2_attack;
    if (*last && now - *last < 1) {
        char out[TEXT_LEN] = "";
        format_battle_for_user(b, req->pid, out, sizeof(out));
        append_text(out, sizeof(out), "\nCooldown active. Wait %ld second(s).\n", 1 - (long)(now - *last));
        send_response(req->pid, RSP_BATTLE, u->username, "%s", out);
        return;
    }
    int dmg = total_damage(u);
    if (ultimate) {
        if (best_weapon_damage(u) <= 0) {
            char out[TEXT_LEN] = "";
            format_battle_for_user(b, req->pid, out, sizeof(out));
            append_text(out, sizeof(out), "\nUltimate failed: buy a weapon first.\n");
            send_response(req->pid, RSP_BATTLE, u->username, "%s", out);
            return;
        }
        dmg *= 3;
    }
    *last = now;
    if (user_is_p1) {
        b->p2_hp -= dmg; if (b->p2_hp < 0) b->p2_hp = 0;
        push_battle_log(b, "%s %s %s for %d damage!", b->p1_name, ultimate ? "ULTS" : "hits", b->p2_name, dmg);
        if (b->p2_hp <= 0) finish_battle(b, 1);
    } else {
        b->p1_hp -= dmg; if (b->p1_hp < 0) b->p1_hp = 0;
        push_battle_log(b, "%s %s %s for %d damage!", b->p2_name, ultimate ? "ULTS" : "hits", b->p1_name, dmg);
        if (b->p1_hp <= 0) finish_battle(b, 0);
    }
    if (b->active == 1) maybe_bot_attack(b);
    char out[TEXT_LEN] = "";
    if (b->active == 2) {
        deliver_finished(b, req->pid, out, sizeof(out));
        send_response(req->pid, RSP_ENDED, u->username, "%s", out);
    } else {
        format_battle_for_user(b, req->pid, out, sizeof(out));
        send_response(req->pid, RSP_BATTLE, u->username, "%s", out);
    }
}

static void handle_forfeit(Request *req) {
    int idx; require_login(req, &idx); if (idx < 0) return;
    User *u = &arena->users[idx];
    if (arena->waiting_pid == req->pid) {
        arena->waiting_pid = 0;
        arena->waiting_user_idx = -1;
        send_response(req->pid, RSP_OK, u->username, "Matchmaking cancelled.");
        return;
    }
    if (u->battle_id >= 0) {
        Battle *b = &arena->battles[u->battle_id];
        if (b->active == 1) {
            int p1_forfeit = (req->pid == b->p1_pid);
            push_battle_log(b, "%s gave up.", p1_forfeit ? b->p1_name : b->p2_name);
            finish_battle(b, p1_forfeit ? 0 : 1);
            char out[TEXT_LEN] = "";
            deliver_finished(b, req->pid, out, sizeof(out));
            send_response(req->pid, RSP_ENDED, u->username, "%s", out);
            return;
        }
    }
    send_response(req->pid, RSP_ERR, u->username, "Nothing to forfeit.");
}

static void handle_request(Request *req) {
    pthread_mutex_lock(&arena->mutex);
    switch (req->cmd) {
        case CMD_PING: send_response(req->pid, RSP_OK, NULL, "Orion is here."); break;
        case CMD_REGISTER: handle_register(req); break;
        case CMD_LOGIN: handle_login(req); break;
        case CMD_LOGOUT: {
            int idx = find_user_by_pid(req->pid);
            if (idx >= 0) {
                char uname[NAME_LEN]; snprintf(uname, sizeof(uname), "%s", arena->users[idx].username);
                clear_pid_state(req->pid);
                send_response(req->pid, RSP_OK, NULL, "Logged out from %s.", uname);
            } else send_response(req->pid, RSP_ERR, NULL, "You are not logged in.");
            break;
        }
        case CMD_PROFILE: {
            int idx; require_login(req, &idx); if (idx >= 0) { char out[TEXT_LEN]; format_profile(&arena->users[idx], out, sizeof(out)); send_response(req->pid, RSP_OK, arena->users[idx].username, "%s", out); }
            break;
        }
        case CMD_ARMORY: {
            int idx; require_login(req, &idx); if (idx < 0) break;
            User *u = &arena->users[idx];
            if (req->value == 0) { char out[TEXT_LEN]; format_armory(u, out, sizeof(out)); send_response(req->pid, RSP_OK, u->username, "%s", out); }
            else if (req->value < 1 || req->value > WEAPON_COUNT) send_response(req->pid, RSP_ERR, u->username, "Invalid weapon choice.");
            else {
                int w = req->value - 1;
                if (u->weapons[w]) send_response(req->pid, RSP_ERR, u->username, "You already own %s.", WEAPONS[w].name);
                else if (u->gold < WEAPONS[w].cost) send_response(req->pid, RSP_ERR, u->username, "Not enough gold. Need %d G, you have %d G.", WEAPONS[w].cost, u->gold);
                else { u->gold -= WEAPONS[w].cost; u->weapons[w] = 1; save_users(); send_response(req->pid, RSP_OK, u->username, "Purchased %s. Best weapon now: %s.", WEAPONS[w].name, best_weapon_name(u)); }
            }
            break;
        }
        case CMD_HISTORY: {
            int idx; require_login(req, &idx); if (idx >= 0) { char out[TEXT_LEN]; format_history(&arena->users[idx], out, sizeof(out)); send_response(req->pid, RSP_OK, arena->users[idx].username, "%s", out); }
            break;
        }
        case CMD_ENTER_BATTLE: handle_enter_battle(req); break;
        case CMD_BATTLE_STATUS: handle_battle_status(req); break;
        case CMD_ATTACK: handle_attack(req, 0); break;
        case CMD_ULTIMATE: handle_attack(req, 1); break;
        case CMD_FORFEIT: handle_forfeit(req); break;
        case CMD_SHUTDOWN: send_response(req->pid, RSP_OK, NULL, "Orion shutting down."); running = 0; break;
        default: send_response(req->pid, RSP_ERR, NULL, "Unknown command."); break;
    }
    pthread_mutex_unlock(&arena->mutex);
}

static void cleanup(int remove_ipc) {
    if (arena) shmdt(arena);
    if (remove_ipc) {
        if (msg_id >= 0) msgctl(msg_id, IPC_RMID, NULL);
        if (shm_id >= 0) shmctl(shm_id, IPC_RMID, NULL);
    }
}

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

static void init_ipc(void) {
    msg_id = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msg_id < 0) { perror("msgget"); exit(1); }
    shm_id = shmget(SHM_KEY, sizeof(Arena), IPC_CREAT | 0666);
    if (shm_id < 0) { perror("shmget"); exit(1); }
    arena = (Arena *)shmat(shm_id, NULL, 0);
    if (arena == (void *)-1) { perror("shmat"); exit(1); }

    memset(arena, 0, sizeof(*arena));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&arena->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    arena->boot_time = time(NULL);
    arena->orion_pid = getpid();
    arena->waiting_user_idx = -1;
    for (int i = 0; i < MAX_USERS; i++) arena->users[i].battle_id = -1;
    load_users();
}

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    init_ipc();
    printf("\033[36m+==============================================+\n|              ORION IS READY                  |\n+==============================================+\033[0m\nPID: %d\n", getpid());
    fflush(stdout);

    while (running) {
        Request req;
        ssize_t n = msgrcv(msg_id, &req, REQ_SIZE, 1, IPC_NOWAIT);
        if (n < 0) {
            if (errno == ENOMSG) {
                usleep(100000);
                continue;
            }
            if (errno == EINTR) continue;
            perror("msgrcv");
            break;
        }
        handle_request(&req);
    }

    pthread_mutex_lock(&arena->mutex);
    for (int i = 0; i < MAX_USERS; i++) {
        arena->users[i].active_pid = 0;
        arena->users[i].battle_id = -1;
    }
    save_users();
    pthread_mutex_unlock(&arena->mutex);
    printf("Orion stopped.\n");
    cleanup(1);
    return 0;
}
```

### 4.3.3 `eternal.c`

```c
#include "arena.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int msg_id = -1;
static pid_t mypid;
static int logged_in = 0;
static char current_user[NAME_LEN];
static struct termios old_term;
static int raw_enabled = 0;

static void disable_raw(void) {
    if (raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
        raw_enabled = 0;
    }
}

static void enable_raw(void) {
    if (raw_enabled) return;
    if (tcgetattr(STDIN_FILENO, &old_term) == 0) {
        struct termios raw = old_term;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        raw_enabled = 1;
    }
}

static void trim(char *s) {
    s[strcspn(s, "\n")] = '\0';
}

static void ask(const char *prompt, char *buf, size_t cap) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, cap, stdin)) buf[0] = '\0';
    trim(buf);
}


static void clear_screen(void) {
    printf("\033[2J\033[H");
}

static void print_logo(void) {
    printf("\033[36m");
    printf("  ____        _   _   _        ___   _____   _____   _____   ____   ___   ___   _   _\n");
    printf(" | __ )  ___ | |_| |_| | ___  / _ \\ |  ___| | ____| |_   _| |  _ \\ |_ _| / _ \\ | \\ | |\n");
    printf(" |  _ \\ / _ \\| __| __| |/ _ \\| | | || |_    |  _|     | |   | |_) | | | | | | ||  \\| |\n");
    printf(" | |_) |  __/| |_| |_| |  __/| |_| ||  _|   | |___    | |   |  _ <  | | | |_| || |\\  |\n");
    printf(" |____/ \\___| \\__|\\__|_|\\___| \\___/ |_|     |_____|   |_|   |_| \\_\\|___| \\___/ |_| \\_|\n");
    printf("\033[0m");
}

static int connect_ipc(void) {
    msg_id = msgget(MSG_KEY, 0666);
    if (msg_id < 0) {
        printf("Orion are you there?\n");
        return -1;
    }

    int shm_id = shmget(SHM_KEY, sizeof(Arena), 0666);
    if (shm_id < 0) {
        printf("Orion are you there?\n");
        return -1;
    }

    Arena *probe = (Arena *)shmat(shm_id, NULL, 0);
    if (probe == (void *)-1) {
        printf("Orion are you there?\n");
        return -1;
    }

    pid_t orion_pid = probe->orion_pid;
    int alive = (orion_pid > 0 && kill(orion_pid, 0) == 0);
    shmdt(probe);

    if (!alive) {
        printf("Orion are you there?\n");
        return -1;
    }

    return 0;
}

static int send_req(int cmd, const char *username, const char *password, int value, Response *rsp) {
    Request req;
    memset(&req, 0, sizeof(req));
    req.mtype = 1;
    req.pid = mypid;
    req.cmd = cmd;
    req.value = value;
    if (username) snprintf(req.username, sizeof(req.username), "%s", username);
    if (password) snprintf(req.password, sizeof(req.password), "%s", password);
    if (msgsnd(msg_id, &req, REQ_SIZE, 0) < 0) {
        perror("msgsnd");
        return -1;
    }
    if (!rsp) return 0;
    while (1) {
        ssize_t n = msgrcv(msg_id, rsp, RSP_SIZE, mypid, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("msgrcv");
            return -1;
        }
        break;
    }
    logged_in = rsp->logged_in;
    if (rsp->username[0]) snprintf(current_user, sizeof(current_user), "%s", rsp->username);
    if (!logged_in) current_user[0] = '\0';
    return rsp->status;
}

static void print_rsp(Response *rsp) {
    printf("%s\n", rsp->text);
}

static void do_register(void) {
    char u[NAME_LEN], p[PASS_LEN];
    ask("Username: ", u, sizeof(u));
    ask("Password: ", p, sizeof(p));
    Response rsp;
    send_req(CMD_REGISTER, u, p, 0, &rsp);
    print_rsp(&rsp);
}

static void do_login(void) {
    char u[NAME_LEN], p[PASS_LEN];
    ask("Username: ", u, sizeof(u));
    ask("Password: ", p, sizeof(p));
    Response rsp;
    send_req(CMD_LOGIN, u, p, 0, &rsp);
    print_rsp(&rsp);
}

static void do_logout(void) {
    Response rsp;
    send_req(CMD_LOGOUT, NULL, NULL, 0, &rsp);
    print_rsp(&rsp);
}

static void pause_enter(void) {
    printf("Press ENTER to continue...");
    fflush(stdout);
    char tmp[8];
    fgets(tmp, sizeof(tmp), stdin);
}

static void armory_menu(void) {
    while (1) {
        Response rsp;
        send_req(CMD_ARMORY, NULL, NULL, 0, &rsp);
        print_rsp(&rsp);
        char ch[16];
        ask("Choice: ", ch, sizeof(ch));
        int choice = atoi(ch);
        if (choice == 0) return;
        send_req(CMD_ARMORY, NULL, NULL, choice, &rsp);
        print_rsp(&rsp);
        pause_enter();
    }
}

static void render_battle(Response *rsp) {
    printf("\033[2J\033[H");
    print_rsp(rsp);
    fflush(stdout);
}

static void battle_mode(void) {
    Response rsp;
    int st = send_req(CMD_ENTER_BATTLE, NULL, NULL, 0, &rsp);
    render_battle(&rsp);
    enable_raw();
    while (1) {
        if (st == RSP_ENDED || st == RSP_ERR) break;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int sel = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) <= 0) continue;
            if (c == 'a' || c == 'A') st = send_req(CMD_ATTACK, NULL, NULL, 0, &rsp);
            else if (c == 'u' || c == 'U') st = send_req(CMD_ULTIMATE, NULL, NULL, 0, &rsp);
            else if (c == 'q' || c == 'Q') st = send_req(CMD_FORFEIT, NULL, NULL, 0, &rsp);
            else continue;
        } else {
            st = send_req(CMD_BATTLE_STATUS, NULL, NULL, 0, &rsp);
        }
        render_battle(&rsp);
        if (st == RSP_ENDED || st == RSP_ERR || st == RSP_OK) break;
    }
    disable_raw();
    char tmp[8];
    fgets(tmp, sizeof(tmp), stdin);
}

static void logged_menu(void) {
    while (logged_in) {
        clear_screen();
        print_logo();
        printf("\n\033[35m+====================== ETERION ======================+\033[0m\n");
        printf("| Logged in as: %-39s |\n", current_user);
        printf("+-----------------------------------------------------+\n");
        printf("| 1. Profile                                          |\n");
        printf("| 2. Battle                                           |\n");
        printf("| 3. Armory                                           |\n");
        printf("| 4. Match History                                    |\n");
        printf("| 5. Logout                                           |\n");
        printf("| 6. Exit                                             |\n");
        printf("+=====================================================+\n");
        printf("Choice: ");
        fflush(stdout);
        char ch[16];
        if (!fgets(ch, sizeof(ch), stdin)) break;
        int choice = atoi(ch);
        Response rsp;
        clear_screen();
        switch (choice) {
            case 1:
                send_req(CMD_PROFILE, NULL, NULL, 0, &rsp);
                print_rsp(&rsp);
                pause_enter();
                break;
            case 2:
                battle_mode();
                break;
            case 3:
                armory_menu();
                break;
            case 4:
                send_req(CMD_HISTORY, NULL, NULL, 0, &rsp);
                print_rsp(&rsp);
                pause_enter();
                break;
            case 5:
                do_logout();
                return;
            case 6:
                do_logout();
                exit(0);
            default:
                printf("Invalid choice.\n");
                pause_enter();
        }
    }
}

static void interactive(void) {
    while (1) {
        clear_screen();
        print_logo();
        printf("\n\033[33m+==================== MAIN GATE ======================+\033[0m\n");
        printf("| 1. Register                                         |\n");
        printf("| 2. Login                                            |\n");
        printf("| 3. Exit                                             |\n");
        printf("+=====================================================+\n");
        printf("Choice: ");
        fflush(stdout);
        char ch[16];
        if (!fgets(ch, sizeof(ch), stdin)) break;
        int choice = atoi(ch);
        clear_screen();
        switch (choice) {
            case 1: do_register(); pause_enter(); break;
            case 2: do_login(); if (logged_in) logged_menu(); else pause_enter(); break;
            case 3: return;
            default: printf("Invalid choice.\n"); pause_enter(); break;
        }
    }
}

static int batch_mode(int argc, char **argv) {
    if (argc < 2) return 0;
    Response rsp;
    if (strcmp(argv[1], "--register") == 0 && argc >= 4) {
        send_req(CMD_REGISTER, argv[2], argv[3], 0, &rsp);
        print_rsp(&rsp);
        return 1;
    }
    if (strcmp(argv[1], "--login-profile") == 0 && argc >= 4) {
        send_req(CMD_LOGIN, argv[2], argv[3], 0, &rsp);
        print_rsp(&rsp);
        if (rsp.status == RSP_OK) {
            send_req(CMD_PROFILE, NULL, NULL, 0, &rsp);
            print_rsp(&rsp);
            send_req(CMD_LOGOUT, NULL, NULL, 0, &rsp);
        }
        return 1;
    }
    if (strcmp(argv[1], "--buy") == 0 && argc >= 5) {
        send_req(CMD_LOGIN, argv[2], argv[3], 0, &rsp);
        if (rsp.status != RSP_OK) { print_rsp(&rsp); return 1; }
        send_req(CMD_ARMORY, NULL, NULL, atoi(argv[4]), &rsp);
        print_rsp(&rsp);
        send_req(CMD_LOGOUT, NULL, NULL, 0, &rsp);
        return 1;
    }
    if (strcmp(argv[1], "--history") == 0 && argc >= 4) {
        send_req(CMD_LOGIN, argv[2], argv[3], 0, &rsp);
        if (rsp.status != RSP_OK) { print_rsp(&rsp); return 1; }
        send_req(CMD_HISTORY, NULL, NULL, 0, &rsp);
        print_rsp(&rsp);
        send_req(CMD_LOGOUT, NULL, NULL, 0, &rsp);
        return 1;
    }
    if (strcmp(argv[1], "--shutdown-orion") == 0) {
        send_req(CMD_SHUTDOWN, NULL, NULL, 0, &rsp);
        print_rsp(&rsp);
        return 1;
    }
    return 0;
}

static void on_signal(int sig) {
    (void)sig;
    disable_raw();
    if (msg_id >= 0) {
        Response rsp;
        send_req(CMD_LOGOUT, NULL, NULL, 0, &rsp);
    }
    exit(0);
}

int main(int argc, char **argv) {
    mypid = getpid();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    atexit(disable_raw);

    if (connect_ipc() < 0) return 1;
    if (batch_mode(argc, argv)) return 0;
    interactive();
    if (logged_in) {
        Response rsp;
        send_req(CMD_LOGOUT, NULL, NULL, 0, &rsp);
    }
    return 0;
}
```

### 4.3.4 `Makefile`

```makefile
CC = gcc
CFLAGS = -Wall -pthread
LDFLAGS = -lrt

all: server client

server: orion.c arena.h
	$(CC) $(CFLAGS) orion.c -o orion $(LDFLAGS)

client: eternal.c arena.h
	$(CC) $(CFLAGS) eternal.c -o eternal $(LDFLAGS)

clean:
	rm -f orion eternal

clear_ipc:
	-ipcs -q | grep 0x0001234 | awk '{print $$2}' | xargs -r ipcrm -q
	-ipcs -m | grep 0x0005678 | awk '{print $$2}' | xargs -r ipcrm -m
	-ipcs -s | grep 0x0009012 | awk '{print $$2}' | xargs -r ipcrm -s
```

---

## 4.4 Cara Running dan Hasil per Tahap

### Tahap 1 - Reset proses lama

```bash
pkill -f orion
pkill -f eternal
pkill -f wired
pkill -f navi
```

Hasil yang diharapkan:

```text
Tidak ada output jika proses lama tidak sedang berjalan.
```

### Tahap 2 - Masuk folder Soal 2

```bash
cd ~/Modul3/soal2
```

### Tahap 3 - Compile ulang

```bash
make clean
make clear_ipc
find . -exec touch {} \;
make
```

Hasil yang diharapkan:

```text
gcc -Wall -pthread orion.c -o orion -lrt
gcc -Wall -pthread eternal.c -o eternal -lrt
```

Cek file:

```bash
ls
```

Hasil yang diharapkan:

```text
arena.h  eternal  eternal.c  Makefile  orion  orion.c
```

### Tahap 4 - Test error eternal tanpa orion

```bash
make clear_ipc
./eternal
```

Hasil yang diharapkan:

```text
Orion are you there?
```

Artinya client berhasil mendeteksi bahwa server `orion` belum berjalan.

### Tahap 5 - Jalankan orion

Terminal 1:

```bash
cd ~/Modul3/soal2
make clear_ipc
./orion
```

Hasil yang diharapkan:

```text
+==============================================+
|                 ORION READY                  |
+==============================================+
PID: xxxx
Waiting for Eternals...
```

Catatan penting:

```text
Jangan menjalankan make clear_ipc setelah orion menyala.
```

### Tahap 6 - Jalankan eternal

Terminal 2:

```bash
cd ~/Modul3/soal2
./eternal
```

Hasil yang diharapkan:

```text
+===================== MAIN GATE =====================+
| 1. Register                                        |
| 2. Login                                           |
| 3. Exit                                            |
+=====================================================+
Choice:
```

### Tahap 7 - Register akun

Input:

```text
1
Username: rootkids
Password: 12321
```

Hasil yang diharapkan:

```text
Account created! Default stats: Gold=150, Lvl=1, XP=0.
```

### Tahap 8 - Test duplicate register

Input:

```text
1
Username: rootkids
Password: 12321
```

Hasil yang diharapkan:

```text
Username 'rootkids' already registered.
```

### Tahap 9 - Test login password salah

Input:

```text
2
Username: rootkids
Password: salah
```

Hasil yang diharapkan:

```text
Login failed. Username or password is wrong.
```

### Tahap 10 - Login benar

Input:

```text
2
Username: rootkids
Password: 12321
```

Hasil yang diharapkan:

```text
Welcome!
```

Lalu muncul menu player:

```text
+====================== ETERION ======================+
| Logged in as: rootkids                              |
| 1. Profile                                          |
| 2. Battle                                           |
| 3. Armory                                           |
| 4. Match History                                    |
| 5. Logout                                           |
| 6. Exit                                             |
+=====================================================+
Choice:
```

### Tahap 11 - Test profile

Input:

```text
1
```

Hasil yang diharapkan:

```text
Name   : rootkids
Gold   : 150
Lvl    : 1
XP     : 0
Health : 100
Damage : 10
Weapon : None
```

### Tahap 12 - Test armory

Input:

```text
3
```

Hasil yang diharapkan:

```text
Wood Sword
Iron Sword
Steel Axe
Demon Blade
God Slayer
```

### Tahap 13 - Test gold tidak cukup

Di armory pilih weapon mahal:

```text
5
```

Hasil yang diharapkan:

```text
Not enough gold. Need 5000 G, you have 150 G.
```

### Tahap 14 - Test beli weapon

Di armory pilih:

```text
1
```

Hasil yang diharapkan:

```text
Purchased Wood Sword. Best weapon now: Wood Sword.
```

Setelah itu profile berubah:

```text
Gold   : 50
Damage : 15
Weapon : Wood Sword
```

### Tahap 15 - Test weapon sudah dimiliki

Di armory pilih lagi:

```text
1
```

Hasil yang diharapkan:

```text
You already own Wood Sword.
```

### Tahap 16 - Test match history kosong

Input:

```text
4
```

Hasil yang diharapkan:

```text
No match history yet.
```

### Tahap 17 - Test akun aktif di session lain

Buka terminal client baru:

```bash
cd ~/Modul3/soal2
./eternal
```

Login dengan akun yang sama:

```text
2
Username: rootkids
Password: 12321
```

Hasil yang diharapkan:

```text
This account is already active in another session.
```

### Tahap 18 - Test battle melawan bot

Di akun `rootkids`, pilih:

```text
2
```

Jika tidak ada player lain, tunggu sekitar 35 detik. Hasil yang diharapkan:

```text
W1ld Beast
[a] Attack | [u] Ultimate | [q] Give up
```

Tekan:

```text
a
```

Hasil yang diharapkan:

```text
rootkids hits W1ld Beast for 15 damage!
```

Tekan `a` berkali-kali dengan cepat. Hasil yang diharapkan:

```text
Cooldown active. Wait 1 second(s).
```

Tekan:

```text
u
```

Hasil yang diharapkan jika sudah punya weapon:

```text
rootkids ULTS W1ld Beast for 45 damage!
```

Tekan:

```text
q
```

Hasil yang diharapkan:

```text
DEFEAT
Battle ended. Press ENTER to continue.
```

---

## 4.5 Penjelasan Kode per Bagian

### 4.5.1 `arena.h`

`arena.h` berisi konfigurasi utama game, seperti key IPC, command request, response code, struktur user, struktur battle, dan daftar weapon.

Bagian penting:

| Bagian | Fungsi |
|---|---|
| `MSG_KEY` | Key Message Queue |
| `SHM_KEY` | Key Shared Memory |
| `CMD_REGISTER` sampai `CMD_SHUTDOWN` | Jenis request client |
| `RSP_OK`, `RSP_ERR`, `RSP_WAITING`, `RSP_BATTLE`, `RSP_ENDED` | Status response server |
| `Weapon` | Data weapon |
| `User` | Data player |
| `Battle` | Data pertandingan |
| `Arena` | Data utama pada Shared Memory |
| `Request` | Pesan dari client ke server |
| `Response` | Balasan dari server ke client |

### 4.5.2 `orion.c`

`orion.c` adalah server utama game. Server membuat IPC, memproses request, mengatur user, mengatur matchmaking, menjalankan battle, memberi reward, dan menyimpan data player.

Fungsi penting:

| Fungsi | Penjelasan |
|---|---|
| `init_ipc()` | Membuat Message Queue dan Shared Memory |
| `cleanup()` | Membersihkan IPC saat server berhenti |
| `save_users()` | Menyimpan data player ke file |
| `load_users()` | Memuat data player dari file |
| `find_user()` | Mencari user berdasarkan username |
| `find_user_by_pid()` | Mencari user berdasarkan PID client |
| `total_damage()` | Menghitung damage total |
| `max_health()` | Menghitung health maksimal |
| `format_profile()` | Membuat tampilan profile |
| `format_armory()` | Membuat tampilan armory |
| `format_history()` | Membuat tampilan history |
| `start_battle()` | Membuat battle baru |
| `handle_enter_battle()` | Mengatur matchmaking |
| `handle_battle_status()` | Mengirim status battle terbaru |
| `handle_attack()` | Memproses attack dan ultimate |
| `maybe_bot_attack()` | Membuat bot menyerang otomatis |
| `finish_battle()` | Menyelesaikan battle |
| `apply_rewards()` | Memberikan XP dan gold |
| `handle_forfeit()` | Memproses surrender |
| `handle_request()` | Pusat pemrosesan semua request |

Server menggunakan mutex pada Shared Memory agar data tetap aman saat diakses banyak process.

### 4.5.3 `eternal.c`

`eternal.c` adalah client player. Client menampilkan menu, membaca input, mengirim request ke server, menerima response, dan menampilkan hasilnya ke terminal.

Fungsi penting:

| Fungsi | Penjelasan |
|---|---|
| `connect_ipc()` | Mengecek apakah Orion aktif |
| `send_req()` | Mengirim request dan menerima response |
| `print_main_gate()` | Menampilkan menu awal |
| `print_player_menu()` | Menampilkan menu player |
| `do_register()` | Register akun |
| `do_login()` | Login akun |
| `do_logout()` | Logout akun |
| `armory_menu()` | Menampilkan dan memproses pembelian weapon |
| `battle_mode()` | Menjalankan battle realtime |
| `enable_raw()` | Mengubah terminal agar membaca input per karakter |
| `disable_raw()` | Mengembalikan mode terminal |
| `render_battle()` | Menampilkan arena battle |
| `batch_mode()` | Mode test cepat via command line |

### 4.5.4 `Makefile`

`Makefile` mempermudah compile dan cleanup.

Target penting:

| Target | Fungsi |
|---|---|
| `all` | Compile server dan client |
| `server` | Compile `orion.c` menjadi `orion` |
| `client` | Compile `eternal.c` menjadi `eternal` |
| `clean` | Menghapus executable hasil compile |
| `clear_ipc` | Menghapus IPC sisa run sebelumnya |

---

# 6. Kesimpulan

Pada Modul 3 ini, saya mengimplementasikan dua program yang berhubungan dengan konsep Sistem Operasi.

Soal 1 menggunakan socket TCP dan pthread untuk membuat sistem client-server. Program ini memiliki fitur multi-client, broadcast chat, admin console, emergency shutdown, dan log aktivitas.

Soal 2 menggunakan IPC berupa Message Queue, Shared Memory, dan mutex untuk membuat game battle realtime. Program ini memiliki fitur register, login, profile, armory, matchmaking, battle melawan player atau bot, reward, dan match history.

Melalui pengerjaan modul ini, saya memahami cara process berkomunikasi, cara server menangani banyak client, cara mengatur data bersama, dan cara mencegah race condition menggunakan mutex.

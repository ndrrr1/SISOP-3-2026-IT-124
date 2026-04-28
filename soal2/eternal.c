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

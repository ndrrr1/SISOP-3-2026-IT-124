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

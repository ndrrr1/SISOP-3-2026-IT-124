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

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <ncurses.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_SENSORS 100
#define MAX_PWMS 100
#define MAX_PATH 256
#define MAX_PROFILES 32

#define CONFIG_PATH "/etc/tuxfan.conf"
#define PID_PATH "/var/run/tuxfan.pid"

// Hardware data structures (sysfs-agnostic)
typedef struct {
    char display_name[128];
    char hw_name[64];       // e.g., "nct6775"
    char feat_name[64];     // e.g., "pwm1" or "temp1_input"
    char path[MAX_PATH];    // Dynamically resolved absolute path
} HwItem;

HwItem sensors[MAX_SENSORS];
int sensor_count = 0;

HwItem pwms[MAX_PWMS];
int pwm_count = 0;

// Profile configuration and state
typedef struct {
    char pwm_hw[64];
    char pwm_feat[64];
    char sensor_hw[64];
    char sensor_feat[64];

    char pwm_path[MAX_PATH];
    char sensor_path[MAX_PATH];
    int pwm_idx;
    int sensor_idx;

    int curve_t[4];
    int curve_p[4];
    int active;
    time_t kickstart_until;
    int last_pwm_val;
    int last_enable_val;
} FanProfile;

FanProfile* profiles = NULL;
int profile_count = 0;
int current_profile = 0;

volatile sig_atomic_t daemon_running = 0;
pthread_t engine_thread;
pthread_mutex_t profiles_mutex = PTHREAD_MUTEX_INITIALIZER;
int pid_lock_fd = -1;

// --- SYSTEM LOCKS & LOGGING ---
int check_and_lock_pid() {
    pid_lock_fd = open(PID_PATH, O_RDWR | O_CREAT, 0644);
    if (pid_lock_fd < 0) return -1;
    if (lockf(pid_lock_fd, F_TLOCK, 0) < 0) {
        close(pid_lock_fd);
        pid_lock_fd = -1;
        return -1; // Locked by another process
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d\n", getpid());
    write(pid_lock_fd, buf, strlen(buf));
    return pid_lock_fd;
}

// --- FILE SYSTEM & HW HELPERS ---
int read_int_file(const char* path) {
    if (!path || path[0] == '\0') return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int val = 0;
    if (fscanf(f, "%d", &val) != 1) val = 0;
    fclose(f);
    return val;
}

// Fail-Safe function for temperatures
int read_temp_file(const char* path) {
    if (!path || path[0] == '\0') return 999000;
    FILE* f = fopen(path, "r");
    if (!f) return 999000; // Hardware offline -> Force MAX PWM!
    int val = 0;
    if (fscanf(f, "%d", &val) != 1) val = 999000;
    fclose(f);
    return val;
}

void write_int_file(const char* path, int val) {
    if (!path || path[0] == '\0') return;
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d", val);
        fclose(f);
    }
}

int get_current_rpm(const char* pwm_path) {
    if (!pwm_path || strlen(pwm_path) == 0) return 0;
    char rpm_path[MAX_PATH];
    strncpy(rpm_path, pwm_path, MAX_PATH - 1);
    rpm_path[MAX_PATH - 1] = '\0';

    char *p = strrchr(rpm_path, '/');
    if (p) p = strstr(p, "pwm");
    else p = strstr(rpm_path, "pwm");

    if (p) {
        memcpy(p, "fan", 3);
        strncat(rpm_path, "_input", MAX_PATH - strlen(rpm_path) - 1);
        return read_int_file(rpm_path);
    }
    return 0;
}

void scan_hardware() {
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;
    struct dirent *dir;
    sensor_count = 0;
    pwm_count = 0;

    while ((dir = readdir(d)) != NULL) {
        if (strncmp(dir->d_name, "hwmon", 5) == 0) {
            char base_path[MAX_PATH], name_path[MAX_PATH], name[64] = "Unknown";

            snprintf(base_path, sizeof(base_path), "/sys/class/hwmon/%.230s", dir->d_name);
            snprintf(name_path, sizeof(name_path), "%.240s/name", base_path);

            FILE* f = fopen(name_path, "r");
            if (f) { fscanf(f, "%63s", name); fclose(f); }

            DIR *hw_d = opendir(base_path);
            if (hw_d) {
                struct dirent *hw_dir;
                while ((hw_dir = readdir(hw_d)) != NULL) {
                    if (strncmp(hw_dir->d_name, "temp", 4) == 0 && strstr(hw_dir->d_name, "_input") && sensor_count < MAX_SENSORS) {
                        char label_path[MAX_PATH], label[64] = "";
                        char temp_id[16] = "";
                        sscanf(hw_dir->d_name, "%15[^_]", temp_id);

                        snprintf(label_path, sizeof(label_path), "%.230s/%.15s_label", base_path, temp_id);

                        FILE* lf = fopen(label_path, "r");
                        if (lf) { fgets(label, 63, lf); label[strcspn(label, "\n")] = 0; fclose(lf); }

                        snprintf(sensors[sensor_count].hw_name, 64, "%.63s", name);
                        snprintf(sensors[sensor_count].feat_name, 64, "%.63s", hw_dir->d_name);
                        snprintf(sensors[sensor_count].path, MAX_PATH, "%.200s/%.50s", base_path, hw_dir->d_name);

                        if (strlen(label) > 0 && strcmp(label, "temp1") != 0)
                            snprintf(sensors[sensor_count].display_name, 128, "%.60s - %.15s [%.40s]", name, temp_id, label);
                        else
                            snprintf(sensors[sensor_count].display_name, 128, "%.60s - %.15s", name, temp_id);

                        sensor_count++;
                    }
                    if (strncmp(hw_dir->d_name, "pwm", 3) == 0 && !strstr(hw_dir->d_name, "_") && pwm_count < MAX_PWMS) {
                        snprintf(pwms[pwm_count].hw_name, 64, "%.63s", name);
                        snprintf(pwms[pwm_count].feat_name, 64, "%.63s", hw_dir->d_name);
                        snprintf(pwms[pwm_count].path, MAX_PATH, "%.200s/%.50s", base_path, hw_dir->d_name);
                        snprintf(pwms[pwm_count].display_name, 128, "%.60s - %.50s", name, hw_dir->d_name);

                        pwm_count++;
                    }
                }
                closedir(hw_d);
            }
        }
    }
    closedir(d);
}

// --- PROFILE MANAGEMENT ---
void init_profile(FanProfile* p) {
    p->curve_t[0] = 30; p->curve_t[1] = 50; p->curve_t[2] = 65; p->curve_t[3] = 80;
    p->curve_p[0] = 0;  p->curve_p[1] = 30; p->curve_p[2] = 60; p->curve_p[3] = 100;
    p->pwm_idx = -1; p->sensor_idx = -1;
    p->pwm_hw[0] = '\0'; p->pwm_feat[0] = '\0';
    p->sensor_hw[0] = '\0'; p->sensor_feat[0] = '\0';
    p->pwm_path[0] = '\0'; p->sensor_path[0] = '\0';
    p->active = 0;
    p->kickstart_until = 0;
    p->last_pwm_val = -1;
    p->last_enable_val = -1;
}

void add_profile() {
    if (profile_count >= MAX_PROFILES) return;
    profile_count++;
    profiles = realloc(profiles, profile_count * sizeof(FanProfile));
    init_profile(&profiles[profile_count - 1]);
}

void remove_profile(int idx) {
    if (profile_count <= 1) { init_profile(&profiles[0]); return; }
    for (int i = idx; i < profile_count - 1; i++) profiles[i] = profiles[i + 1];
    profile_count--;
    profiles = realloc(profiles, profile_count * sizeof(FanProfile));
    if (current_profile >= profile_count) current_profile = profile_count - 1;
}

void ensure_profile_exists(int idx) {
    while (profile_count <= idx && profile_count < MAX_PROFILES) add_profile();
}

// --- ENGINE LOGIC ---
int calculate_duty(FanProfile* prof, int temp) {
    if (temp <= prof->curve_t[0]) return prof->curve_p[0];
    if (temp >= prof->curve_t[3]) return prof->curve_p[3];

    for (int i = 0; i < 3; i++) {
        if (temp >= prof->curve_t[i] && temp <= prof->curve_t[i+1]) {
            if (prof->curve_t[i+1] == prof->curve_t[i]) return prof->curve_p[i+1];
            if (prof->curve_p[i] == 0 && temp < prof->curve_t[i+1]) return 0;

            float ratio = (float)(temp - prof->curve_t[i]) / (prof->curve_t[i+1] - prof->curve_t[i]);
            return prof->curve_p[i] + (int)((prof->curve_p[i+1] - prof->curve_p[i]) * ratio);
        }
    }
    return 100;
}

void* fan_engine_thread(void* arg) {
    (void)arg;
    syslog(LOG_INFO, "Fan engine loop started successfully.");

    while (daemon_running) {
        time_t now = time(NULL);

        pthread_mutex_lock(&profiles_mutex);
        int local_count = profile_count;
        FanProfile* snap = malloc(local_count * sizeof(FanProfile));
        memcpy(snap, profiles, local_count * sizeof(FanProfile));
        pthread_mutex_unlock(&profiles_mutex);

        for (int i = 0; i < local_count; i++) {
            FanProfile *p = &snap[i];
            if (p->active && strlen(p->pwm_path) > 0 && strlen(p->sensor_path) > 0) {

                int current_temp = read_temp_file(p->sensor_path) / 1000;
                int current_rpm  = get_current_rpm(p->pwm_path);

                int target_pwm_pct = calculate_duty(p, current_temp);

                // 999C is the hardware Fail-Safe trigger
                if (current_temp >= 999) {
                    target_pwm_pct = 100;
                    if (p->last_pwm_val != 255) {
                        syslog(LOG_ERR, "HARDWARE FAILURE: Sensor %s (%s) offline! Forcing 100%% PWM.", p->sensor_hw, p->sensor_feat);
                    }
                }

                int target_pwm_val = (target_pwm_pct * 255) / 100;
                int need_kickstart = (target_pwm_pct > 0 && current_rpm == 0 && p->kickstart_until == 0);

                char enable_path[MAX_PATH];
                snprintf(enable_path, sizeof(enable_path), "%.240s_enable", p->pwm_path);

                pthread_mutex_lock(&profiles_mutex);
                for (int j = 0; j < profile_count; j++) {
                    if (strcmp(profiles[j].pwm_path, p->pwm_path) == 0 && strcmp(profiles[j].sensor_path, p->sensor_path) == 0) {

                        if (profiles[j].last_enable_val != 1) {
                            write_int_file(enable_path, 1);
                            profiles[j].last_enable_val = 1;
                        }

                        if (need_kickstart) profiles[j].kickstart_until = now + 1;

                        if (profiles[j].kickstart_until > now) {
                            if (profiles[j].last_pwm_val != 255) {
                                write_int_file(p->pwm_path, 255);
                                profiles[j].last_pwm_val = 255;
                            }
                        } else {
                            if (profiles[j].kickstart_until <= now && profiles[j].kickstart_until != 0) {
                                profiles[j].kickstart_until = 0;
                            }
                            if (profiles[j].last_pwm_val != target_pwm_val) {
                                write_int_file(p->pwm_path, target_pwm_val);
                                profiles[j].last_pwm_val = target_pwm_val;
                            }
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&profiles_mutex);
            }
        }
        free(snap);
        sleep(2);
    }
    syslog(LOG_INFO, "Fan engine loop stopped.");
    return NULL;
}

// --- SIGNAL HANDLER (SAFETY SHUTDOWN) ---
void handle_signal(int sig) {
    daemon_running = 0;
    syslog(LOG_INFO, "Received signal %d. Restoring hardware defaults...", sig);

    pthread_mutex_lock(&profiles_mutex);
    for (int i = 0; i < profile_count; i++) {
        if (strlen(profiles[i].pwm_path) > 0) {
            char enable_path[MAX_PATH];
            snprintf(enable_path, sizeof(enable_path), "%.240s_enable", profiles[i].pwm_path);
            write_int_file(enable_path, 2);
            write_int_file(profiles[i].pwm_path, 255);
        }
    }
    pthread_mutex_unlock(&profiles_mutex);

    if (pid_lock_fd >= 0) { close(pid_lock_fd); unlink(PID_PATH); }
    closelog();

    endwin();
    exit(0);
}

// --- CONFIGURATION ---
void load_config() {
    if (profiles) free(profiles);
    profiles = NULL;
    profile_count = 0;
    add_profile();

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int p_idx, t_idx, val; char str[128];

        if (sscanf(line, "p%d_pwm_hw=%127s", &p_idx, str) == 2 && p_idx < MAX_PROFILES) {
            ensure_profile_exists(p_idx); strcpy(profiles[p_idx].pwm_hw, str); profiles[p_idx].active = 1;
        }
        else if (sscanf(line, "p%d_pwm_feat=%127s", &p_idx, str) == 2 && p_idx < MAX_PROFILES) {
            ensure_profile_exists(p_idx); strcpy(profiles[p_idx].pwm_feat, str);
        }
        else if (sscanf(line, "p%d_sensor_hw=%127s", &p_idx, str) == 2 && p_idx < MAX_PROFILES) {
            ensure_profile_exists(p_idx); strcpy(profiles[p_idx].sensor_hw, str);
        }
        else if (sscanf(line, "p%d_sensor_feat=%127s", &p_idx, str) == 2 && p_idx < MAX_PROFILES) {
            ensure_profile_exists(p_idx); strcpy(profiles[p_idx].sensor_feat, str);
        }
        else if (sscanf(line, "p%d_t%d=%d", &p_idx, &t_idx, &val) == 3 && p_idx < MAX_PROFILES) {
            ensure_profile_exists(p_idx); if (t_idx >= 0 && t_idx < 4) profiles[p_idx].curve_t[t_idx] = val;
        }
        else if (sscanf(line, "p%d_p%d=%d", &p_idx, &t_idx, &val) == 3 && p_idx < MAX_PROFILES) {
            ensure_profile_exists(p_idx); if (t_idx >= 0 && t_idx < 4) profiles[p_idx].curve_p[t_idx] = val;
        }
    }
    fclose(f);

    // Dynamic Binding: Match loaded names to current sysfs paths
    for (int p = 0; p < profile_count; p++) {
        if (strlen(profiles[p].pwm_hw) > 0) {
            for (int i=0; i<pwm_count; i++) {
                if(strcmp(pwms[i].hw_name, profiles[p].pwm_hw) == 0 && strcmp(pwms[i].feat_name, profiles[p].pwm_feat) == 0) {
                    profiles[p].pwm_idx = i;
                    strcpy(profiles[p].pwm_path, pwms[i].path);
                    break;
                }
            }
        }
        if (strlen(profiles[p].sensor_hw) > 0) {
            for (int i=0; i<sensor_count; i++) {
                if(strcmp(sensors[i].hw_name, profiles[p].sensor_hw) == 0 && strcmp(sensors[i].feat_name, profiles[p].sensor_feat) == 0) {
                    profiles[p].sensor_idx = i;
                    strcpy(profiles[p].sensor_path, sensors[i].path);
                    break;
                }
            }
        }
    }
}

void save_config() {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (f) {
        for (int i = 0; i < profile_count; i++) {
            if (profiles[i].active || strlen(profiles[i].pwm_hw) > 0) {
                fprintf(f, "p%d_pwm_hw=%s\n", i, profiles[i].pwm_hw);
                fprintf(f, "p%d_pwm_feat=%s\n", i, profiles[i].pwm_feat);
                fprintf(f, "p%d_sensor_hw=%s\n", i, profiles[i].sensor_hw);
                fprintf(f, "p%d_sensor_feat=%s\n", i, profiles[i].sensor_feat);
                for(int j=0; j<4; j++) fprintf(f, "p%d_t%d=%d\n", i, j, profiles[i].curve_t[j]);
                for(int j=0; j<4; j++) fprintf(f, "p%d_p%d=%d\n", i, j, profiles[i].curve_p[j]);
            }
        }
        fclose(f);
        syslog(LOG_INFO, "Config updated in %s", CONFIG_PATH);
    } else {
        syslog(LOG_ERR, "Failed to write config to %s", CONFIG_PATH);
    }
}

// --- TUI INTERFACE ---
enum { C_MAIN = 1, C_SEL, C_TITLE, C_GRAPH_LINE, C_GRAPH_POINT, C_CURR, C_SHADOW };

void draw_frame(int w, int h) {
    attron(COLOR_PAIR(C_MAIN));

    mvaddch(2, 0, ACS_LTEE);
    mvhline(2, 1, ACS_HLINE, w - 2);
    mvaddch(2, w - 1, ACS_RTEE);
    mvvline(3, w/2 - 2, ACS_VLINE, h - 6);
    mvaddch(2, w/2 - 2, ACS_TTEE);
    mvaddch(h - 3, w/2 - 2, ACS_BTEE);
    mvaddch(h - 3, 0, ACS_LTEE);
    mvhline(h - 3, 1, ACS_HLINE, w - 2);
    mvaddch(h - 3, w - 1, ACS_RTEE);

    attron(COLOR_PAIR(C_TITLE) | A_BOLD);
    mvhline(0, 0, ' ', w);
    mvprintw(0, (w - 14) / 2, "TuxFanControl v3.0");
    attroff(A_BOLD);

    attron(COLOR_PAIR(C_MAIN));
    mvprintw(h - 2, 1, "[^/v] Nav  [<-/->] Switch  [+] Add Profile  [-] Del Profile");
    mvprintw(h - 1, 1, "[Enter] Edit Item      [F10] Save & Apply       [ESC] Exit ");
}

void draw_curve_graph(int ox, int oy, int w, int h, int current_temp, FanProfile* prof) {
    int bottom_y = oy + h, left_x = ox;

    attron(COLOR_PAIR(C_MAIN));
    attron(A_DIM);
    for(int i=1; i<=w; i++) mvaddch(bottom_y - h/2, left_x + i, '.');
    for(int j=1; j<=h; j++) mvaddch(bottom_y - j, left_x + w/2, '.');
    attroff(A_DIM);

    for(int i=1; i<=w; i++) mvaddch(bottom_y, left_x + i, ACS_HLINE);
    for(int i=1; i<=h; i++) mvaddch(bottom_y - i, left_x, ACS_VLINE);
    mvaddch(bottom_y, left_x, ACS_LLCORNER);

    mvprintw(oy - 1, left_x - 1, "PWM%%");
    mvprintw(bottom_y + 1, left_x + w - 6, "Temp(C)");
    mvprintw(bottom_y + 1, left_x, "0");
    mvprintw(bottom_y + 1, left_x + w/2 - 1, "50");
    mvprintw(bottom_y + 1, left_x + w - 2, "100");
    mvprintw(bottom_y, left_x - 3, "  0");
    mvprintw(bottom_y - h/2, left_x - 3, " 50");
    mvprintw(oy, left_x - 4, "100");

    attron(COLOR_PAIR(C_GRAPH_LINE) | A_BOLD);
    for(int i=1; i<=w; i++) {
        int t = (i * 100) / w;
        int pwm = calculate_duty(prof, t);
        int plot_y = bottom_y - (pwm * h) / 100;

        if (plot_y < oy) { plot_y = oy; }
        if (plot_y > bottom_y) { plot_y = bottom_y; }

        mvaddch(plot_y, left_x + i, '*');
    }

    attron(COLOR_PAIR(C_GRAPH_POINT) | A_BOLD);
    for(int p=0; p<4; p++) {
        int px = (prof->curve_t[p] * w) / 100;
        int py = bottom_y - (prof->curve_p[p] * h) / 100;
        if(px >= 0 && px <= w && py >= oy && py <= bottom_y) mvaddch(py, left_x + px, 'O');
    }
    attroff(A_BOLD);

    if (current_temp >= 0 && current_temp < 999) {
        int px = (current_temp * w) / 100;
        if (px > w) px = w;
        int pwm = calculate_duty(prof, current_temp);
        int py = bottom_y - (pwm * h) / 100;
        if (px >= 0 && px <= w && py >= oy && py <= bottom_y) {
            attron(COLOR_PAIR(C_CURR) | A_BOLD);
            mvaddch(py, left_x + px, 'X');
            attroff(COLOR_PAIR(C_CURR) | A_BOLD);
        }
    }
    attron(COLOR_PAIR(C_MAIN));
}

void draw_shadow(int x, int y, int w, int h) {
    attron(COLOR_PAIR(C_SHADOW) | A_DIM);
    for (int i = 1; i <= h; i++) {
        if (x + w < COLS) mvaddch(y + i, x + w, (mvinch(y + i, x + w) & A_CHARTEXT));
        if (x + w + 1 < COLS) mvaddch(y + i, x + w + 1, (mvinch(y + i, x + w + 1) & A_CHARTEXT));
    }
    for (int i = 2; i <= w + 1; i++) {
        if (y + h < LINES && x + i < COLS) {
            mvaddch(y + h, x + i, (mvinch(y + h, x + i) & A_CHARTEXT));
        }
    }
    attroff(COLOR_PAIR(C_SHADOW) | A_DIM);
    refresh();
}

int popup_menu(const char* title, HwItem* items, int count, int current_idx, int is_sensor) {
    int w = 64, h = 15;
    int x = (COLS - w) / 2, y = (LINES - h) / 2;
    int sel = current_idx >= 0 ? current_idx : 0;
    int offset = 0;

    draw_shadow(x, y, w, h);

    WINDOW *win = newwin(h, w, y, x);
    keypad(win, TRUE);
    wbkgd(win, COLOR_PAIR(C_MAIN));
    nodelay(win, TRUE);

    while(1) {
        werase(win);
        wattron(win, COLOR_PAIR(C_MAIN));
        box(win, 0, 0);
        wattron(win, COLOR_PAIR(C_TITLE) | A_BOLD);
        mvwprintw(win, 0, 2, " %s ", title);
        wattroff(win, COLOR_PAIR(C_TITLE) | A_BOLD);

        for (int i = 0; i < h - 2; i++) {
            int idx = offset + i;
            if (idx >= count) continue;

            if (idx == sel) wattron(win, COLOR_PAIR(C_SEL) | A_BOLD);
            else wattron(win, COLOR_PAIR(C_MAIN));

            char display_str[128];
            if (is_sensor) snprintf(display_str, sizeof(display_str), "%-44.44s | %3d C", items[idx].display_name, read_temp_file(items[idx].path) / 1000);
            else snprintf(display_str, sizeof(display_str), "%-60.60s", items[idx].display_name);

            mvwprintw(win, i + 1, 1, " %-60.60s ", display_str);
        }
        wrefresh(win); napms(100);

        int ch = wgetch(win);
        if (ch == ERR) continue;
        if (ch == 27) { sel = -1; break; }
        else if (ch == KEY_UP && sel > 0) { sel--; if(sel < offset) offset--; }
        else if (ch == KEY_DOWN && sel < count - 1) { sel++; if(sel >= offset + h - 2) offset++; }
        else if (ch == '\n') break;
    }
    delwin(win);
    return sel;
}

int edit_number(const char* title, int current_val) {
    int w = 36, h = 6;
    int x = (COLS - w) / 2, y = (LINES - h) / 2;
    draw_shadow(x, y, w, h);
    WINDOW *win = newwin(h, w, y, x);
    keypad(win, TRUE);
    wbkgd(win, COLOR_PAIR(C_MAIN));
    werase(win);
    box(win, 0, 0);

    wattron(win, COLOR_PAIR(C_TITLE) | A_BOLD);
    mvwprintw(win, 1, 2, "%s", title);
    wattroff(win, A_BOLD);
    wattron(win, COLOR_PAIR(C_MAIN));
    mvwprintw(win, 3, 2, "New Value: [      ]");
    wrefresh(win);

    echo(); curs_set(1); wtimeout(win, -1);
    char buf[10] = {0};
    wmove(win, 3, 14);
    wgetnstr(win, buf, 4);

    noecho(); curs_set(0); delwin(win);
    if (strlen(buf) > 0) return atoi(buf);
    return current_val;
}

void run_tui() {
    int is_root = (getuid() == 0);
    if (!is_root) {
        printf("Info: Running as non-root. TUI can visualize, but saving to /etc requires sudo.\n");
        sleep(2);
    }

    initscr(); start_color(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    timeout(500);

    if (COLS < 80 || LINES < 24) {
        endwin();
        fprintf(stderr, "Error: Terminal size must be at least 80x24!\n");
        exit(1);
    }

    init_pair(C_MAIN, COLOR_WHITE, COLOR_BLUE);
    init_pair(C_SEL, COLOR_BLACK, COLOR_CYAN);
    init_pair(C_TITLE, COLOR_YELLOW, COLOR_BLUE);
    init_pair(C_GRAPH_LINE, COLOR_CYAN, COLOR_BLUE);
    init_pair(C_GRAPH_POINT, COLOR_YELLOW, COLOR_BLUE);
    init_pair(C_CURR, COLOR_WHITE, COLOR_RED);
    init_pair(C_SHADOW, COLOR_WHITE, COLOR_BLACK);

    bkgd(COLOR_PAIR(C_MAIN));

    int sel_idx = 0;
    const int menu_items = 13;
    char* labels[] = {
        "Active Profile", "", "Target PWM", "Source Sensor", "",
        "Min Temp (C)", "Min Duty (%)", "Low Temp (C)", "Low Duty (%)",
        "Mid Temp (C)", "Mid Duty (%)", "Max Temp (C)", "Max Duty (%)"
    };

    int poll_counter = 0, cached_temp = -1, cached_rpm = -1;

    while(1) {
        erase(); draw_frame(COLS, LINES);

        int ly = 4;
        attron(COLOR_PAIR(C_MAIN) | A_BOLD);
        mvprintw(ly++, 2, "Enterprise Grade Fan Logic");
        attroff(A_BOLD);
        ly++;

        pthread_mutex_lock(&profiles_mutex);

        FanProfile *sp = &profiles[current_profile];
        int s_idx = sp->sensor_idx;
        int p_idx_hw = sp->pwm_idx;

        char s_path[MAX_PATH]; strcpy(s_path, s_idx >= 0 ? sensors[s_idx].path : "");
        char p_path[MAX_PATH]; strcpy(p_path, p_idx_hw >= 0 ? pwms[p_idx_hw].path : "");

        if (poll_counter <= 0) {
            if (s_idx >= 0) cached_temp = read_temp_file(s_path) / 1000; else cached_temp = -1;
            if (p_idx_hw >= 0) cached_rpm = get_current_rpm(p_path); else cached_rpm = -1;
            poll_counter = 4;
        }
        poll_counter--;

        for (int i = 0; i < menu_items; i++) {
            if (i == 1 || i == 4) { ly++; continue; }
            if (i == sel_idx) attron(COLOR_PAIR(C_SEL) | A_BOLD);
            else attron(COLOR_PAIR(C_MAIN));

            mvprintw(ly, 2, " %-24s ", labels[i]);

            if (i == 0) mvprintw(ly, 28, "[   %d / %d   ]", current_profile + 1, profile_count);
            else if (i == 2) mvprintw(ly, 28, "[%-15.15s]", sp->pwm_idx >= 0 ? pwms[sp->pwm_idx].hw_name : "Select...");
            else if (i == 3) mvprintw(ly, 28, "[%-15.15s]", sp->sensor_idx >= 0 ? sensors[sp->sensor_idx].hw_name : "Select...");
            else if (i >= 5 && i <= 12) {
                int is_t = (i % 2 != 0);
                int idx = (i - 5) / 2;
                mvprintw(ly, 28, "[%3d]", is_t ? sp->curve_t[idx] : sp->curve_p[idx]);
            }
            ly++;
        }

        int rx = COLS/2 + 2, ry = 4;
        attron(COLOR_PAIR(C_TITLE) | A_BOLD);
        mvprintw(ry++, rx, "Fan Curve Visualization:");
        attroff(A_BOLD); ry++;

        draw_curve_graph(rx + 4, ry, 30, 10, cached_temp, sp);

        ry += 13;
        attron(COLOR_PAIR(C_TITLE) | A_BOLD);
        mvprintw(ry++, rx, "System Diagnostics:");
        attron(COLOR_PAIR(C_MAIN));
        attroff(A_BOLD);

        if (sp->sensor_idx >= 0) {
            if (cached_temp >= 999) {
                attron(COLOR_PAIR(C_CURR) | A_BOLD);
                mvprintw(ry++, rx, " SENSOR ERROR! FAIL-SAFE ENGAGED");
                attron(COLOR_PAIR(C_MAIN));
            } else {
                mvprintw(ry++, rx, " Target Temp: %d C", cached_temp);
            }
            mvprintw(ry++, rx, " Target PWM : %d %%", (cached_temp>=999)? 100 : calculate_duty(sp, cached_temp));
        } else {
            mvprintw(ry++, rx, " Sensor not selected."); ry++;
        }

        if (sp->pwm_idx >= 0) {
            mvprintw(ry++, rx, " Current RPM: %d", cached_rpm);
            if (calculate_duty(sp, cached_temp) > 0 && cached_rpm == 0) {
                attron(COLOR_PAIR(C_CURR) | A_BOLD);
                mvprintw(ry++, rx, " STATUS: KICK-STARTING! ");
                attron(COLOR_PAIR(C_MAIN));
            }
        }

        pthread_mutex_unlock(&profiles_mutex);

        if (daemon_running) {
            attron(COLOR_PAIR(C_CURR) | A_BOLD);
            mvprintw(LINES - 4, 2, " ENGINE: RUNNING IN BACKGROUND ");
            attron(COLOR_PAIR(C_MAIN));
        }

        refresh();

        int ch = getch();
        if (ch == ERR) continue;

        if (ch == 27) break;
        else if (ch == '+' || ch == '=') {
            pthread_mutex_lock(&profiles_mutex); add_profile(); current_profile = profile_count - 1; poll_counter = 0; pthread_mutex_unlock(&profiles_mutex);
        }
        else if (ch == '-' || ch == '_') {
            pthread_mutex_lock(&profiles_mutex); remove_profile(current_profile); poll_counter = 0; pthread_mutex_unlock(&profiles_mutex);
        }
        else if (ch == KEY_F(10)) {
            if (!is_root) {
                attron(COLOR_PAIR(C_CURR) | A_BOLD);
                mvprintw(LINES - 1, COLS/2 - 15, " ERROR: RUN WITH SUDO TO SAVE! ");
                attroff(COLOR_PAIR(C_CURR) | A_BOLD);
                refresh(); napms(1500);
            } else {
                pthread_mutex_lock(&profiles_mutex); save_config(); pthread_mutex_unlock(&profiles_mutex);

                if (!daemon_running) {
                    int check_fd = open(PID_PATH, O_RDWR);
                    int is_locked = 0;
                    if (check_fd >= 0) {
                        if (lockf(check_fd, F_TEST, 0) < 0) is_locked = 1;
                        close(check_fd);
                    }
                    if (is_locked) {
                        attron(COLOR_PAIR(C_CURR) | A_BOLD);
                        mvprintw(LINES - 1, COLS/2 - 22, " SAVED. RESTART SYSTEMD SERVICE TO APPLY! ");
                        attroff(COLOR_PAIR(C_CURR) | A_BOLD);
                        refresh(); napms(2000);
                    } else {
                        check_and_lock_pid();
                        daemon_running = 1;
                        pthread_create(&engine_thread, NULL, fan_engine_thread, NULL);
                    }
                } else {
                    attron(COLOR_PAIR(C_TITLE) | A_BOLD);
                    mvprintw(LINES - 1, COLS/2 - 6, " SAVED ");
                    attroff(COLOR_PAIR(C_TITLE) | A_BOLD);
                    refresh(); napms(1000);
                }
            }
        }
        else if (ch == KEY_UP && sel_idx > 0) { sel_idx--; if (sel_idx == 4 || sel_idx == 1) sel_idx--; }
        else if (ch == KEY_DOWN && sel_idx < menu_items - 1) { sel_idx++; if (sel_idx == 4 || sel_idx == 1) sel_idx++; }
        else if (ch == KEY_LEFT && sel_idx == 0) { if (current_profile > 0) { current_profile--; poll_counter = 0; } }
        else if (ch == KEY_RIGHT && sel_idx == 0) { if (current_profile < profile_count - 1) { current_profile++; poll_counter = 0; } }
        else if (ch == '\n') {
            pthread_mutex_lock(&profiles_mutex);
            FanProfile *ep = &profiles[current_profile];
            int e_pwm_idx = ep->pwm_idx, e_sen_idx = ep->sensor_idx;
            pthread_mutex_unlock(&profiles_mutex);

            if (sel_idx == 0) { current_profile = (current_profile + 1) % profile_count; poll_counter = 0; }
            else if (sel_idx == 2) {
                int res = popup_menu("Select PWM Controller", pwms, pwm_count, e_pwm_idx, 0);
                if (res >= 0) {
                    pthread_mutex_lock(&profiles_mutex);
                    profiles[current_profile].pwm_idx = res;
                    strcpy(profiles[current_profile].pwm_hw, pwms[res].hw_name);
                    strcpy(profiles[current_profile].pwm_feat, pwms[res].feat_name);
                    strcpy(profiles[current_profile].pwm_path, pwms[res].path);
                    profiles[current_profile].active = 1;
                    pthread_mutex_unlock(&profiles_mutex); poll_counter = 0;
                }
            } else if (sel_idx == 3) {
                int res = popup_menu("Select Temperature Sensor", sensors, sensor_count, e_sen_idx, 1);
                if (res >= 0) {
                    pthread_mutex_lock(&profiles_mutex);
                    profiles[current_profile].sensor_idx = res;
                    strcpy(profiles[current_profile].sensor_hw, sensors[res].hw_name);
                    strcpy(profiles[current_profile].sensor_feat, sensors[res].feat_name);
                    strcpy(profiles[current_profile].sensor_path, sensors[res].path);
                    profiles[current_profile].active = 1;
                    pthread_mutex_unlock(&profiles_mutex); poll_counter = 0;
                }
            } else if (sel_idx >= 5) {
                int is_t = (sel_idx % 2 != 0), idx = (sel_idx - 5) / 2;
                char prompt[64]; snprintf(prompt, sizeof(prompt), "Set %s", labels[sel_idx]);
                pthread_mutex_lock(&profiles_mutex); int current = is_t ? profiles[current_profile].curve_t[idx] : profiles[current_profile].curve_p[idx]; pthread_mutex_unlock(&profiles_mutex);
                int n = edit_number(prompt, current);
                pthread_mutex_lock(&profiles_mutex);
                if (is_t) { if (n < 0) n = 0; if (n > 120) n = 120; profiles[current_profile].curve_t[idx] = n; }
                else { if (n < 0) n = 0; if (n > 100) n = 100; profiles[current_profile].curve_p[idx] = n; }
                pthread_mutex_unlock(&profiles_mutex);
            }
        }
    }
    endwin();
}

int main(int argc, char** argv) {
    openlog("tuxfan", LOG_PID | LOG_CONS, LOG_DAEMON);

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    scan_hardware();
    load_config();

    if (argc > 1 && strcmp(argv[1], "--daemon") == 0) {
        int has_active = 0;
        for(int i=0; i<profile_count; i++) if(profiles[i].active) has_active = 1;

        if (!has_active) {
            syslog(LOG_ERR, "No active profiles found. Exiting daemon.");
            fprintf(stderr, "Error: No active profiles found. Run TUI to configure.\n");
            return 1;
        }

        if (check_and_lock_pid() < 0) {
            syslog(LOG_ERR, "Failed to start daemon: another instance is already running.");
            fprintf(stderr, "Error: TuxFanControl daemon is already running!\n");
            return 1;
        }

        syslog(LOG_INFO, "Starting TuxFanControl daemon background service.");
        daemon_running = 1;
        fan_engine_thread(NULL);

        if (pid_lock_fd >= 0) { close(pid_lock_fd); unlink(PID_PATH); }
        closelog();
        return 0;
    }

    run_tui();

    if (daemon_running) { daemon_running = 0; pthread_join(engine_thread, NULL); }
    if (profiles) free(profiles);
    if (pid_lock_fd >= 0) { close(pid_lock_fd); unlink(PID_PATH); }

    closelog();
    return 0;
}

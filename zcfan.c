#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MILLIC_TO_C(n) (n / 1000)
#define FAN_CONTROL_FILE "/proc/acpi/ibm/fan"
#define TEMP_INVALID INT_MIN
#define TEMP_MIN INT_MIN + 1
#define NS_IN_SEC 1000000000L  // 1 second in nanoseconds
#define THRESHOLD_NS 200000000 // 0.2 seconds

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define DEFAULT_WATCHDOG_SECS 120
#define S_DEFAULT_WATCHDOG_SECS STR(DEFAULT_WATCHDOG_SECS)

#define info(fmt, ...) fprintf(stderr, "[INF] " fmt, ##__VA_ARGS__)
#define err(fmt, ...) fprintf(stderr, "[ERR] " fmt, ##__VA_ARGS__)
#define max(x, y) ((x) > (y) ? (x) : (y))
#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "FATAL: !(%s) at %s:%s:%d\n", #x, __FILE__,        \
                    __func__, __LINE__);                                       \
            abort();                                                           \
        }                                                                      \
    } while (0)

#define CONFIG_MAX_STRLEN 15
#define S_CONFIG_MAX_STRLEN STR(CONFIG_MAX_STRLEN)

#define MAX_IGNORED_SENSORS 1024
#define SENSOR_NAME_MAX 256
static char ignored_sensors_arr[MAX_IGNORED_SENSORS][SENSOR_NAME_MAX];
static size_t num_to_ignore_sensors = 0;
static size_t num_ignored_sensors = 0;

#define MAX_SENSOR_FDS 4096
static int sensor_fds[MAX_SENSOR_FDS];
static size_t num_sensor_fds = 0;

/*
 * Dynamic fan rules: up to MAX_TIERS active tiers + 1 "off" sentinel.
 * Rules are stored highest-to-lowest threshold order.
 * The last entry is always the "off" sentinel (threshold = TEMP_MIN).
 */
#define MAX_TIERS 8

struct Rule {
    char tpacpi_level[CONFIG_MAX_STRLEN + 1];
    int threshold;
    char name[32];
};

static struct Rule rules[MAX_TIERS + 1]; /* +1 for the "off" sentinel */
static size_t num_tiers = 0;            /* active tiers, excludes "off" */

static void init_default_rules(void) {
    rules[0] = (struct Rule){"full-speed", 90, "maximum"};
    rules[1] = (struct Rule){"4", 80, "medium"};
    rules[2] = (struct Rule){"1", 70, "low"};
    rules[3] = (struct Rule){"0", TEMP_MIN, "off"};
    num_tiers = 3;
}

static struct timespec last_watchdog_ping = {0, 0};
static time_t watchdog_secs = DEFAULT_WATCHDOG_SECS;
static int temp_hysteresis = 10;
static const unsigned int tick_hysteresis = 3;
/* Number of consecutive seconds a higher fan level must persist before
 * actually ramping the fan up. Configurable via /etc/zcfan.conf.
 */
static unsigned int up_delay_ticks = 3;
static char output_buf[512];
static const struct Rule *current_rule = NULL;
static volatile sig_atomic_t run = 1;
static volatile sig_atomic_t pending_sleep = 0;
static volatile sig_atomic_t pending_resume = 0;
static int first_tick = 1; /* Stop running if errors are immediate */

enum resume_state {
    RESUME_NOT_DETECTED,
    RESUME_DETECTED,
};

static void exit_if_first_tick(void) {
    if (first_tick) {
        err("Quitting due to failure during first run\n");
        exit(1);
    }
}

static int64_t timespec_diff_ns(const struct timespec *start,
                                const struct timespec *end) {
    return ((int64_t)end->tv_sec - (int64_t)start->tv_sec) * NS_IN_SEC +
           (end->tv_nsec - start->tv_nsec);
}

static enum resume_state detect_suspend(void) {
    static struct timespec monotonic_prev, boottime_prev;
    struct timespec monotonic_now, boottime_now;

    expect(clock_gettime(CLOCK_MONOTONIC_COARSE, &monotonic_now) == 0);
    expect(clock_gettime(CLOCK_BOOTTIME, &boottime_now) == 0);

    if (monotonic_prev.tv_sec == 0 && monotonic_prev.tv_nsec == 0) {
        monotonic_prev = monotonic_now;
        boottime_prev = boottime_now;
        return RESUME_NOT_DETECTED;
    }

    int64_t delta_monotonic = timespec_diff_ns(&monotonic_prev, &monotonic_now);
    int64_t delta_boottime = timespec_diff_ns(&boottime_prev, &boottime_now);

    monotonic_prev = monotonic_now;
    boottime_prev = boottime_now;

    return delta_boottime > delta_monotonic + THRESHOLD_NS
               ? RESUME_DETECTED
               : RESUME_NOT_DETECTED;
}

static void fscanf_ignore_sensor(FILE *f, long pos) {
    char sensor_name[SENSOR_NAME_MAX];
    int ret = fscanf(f, "ignore_sensor %255s ", sensor_name);
    if (ret == 1) {
        expect(num_to_ignore_sensors < MAX_IGNORED_SENSORS);
        snprintf(ignored_sensors_arr[num_to_ignore_sensors], SENSOR_NAME_MAX,
                 "%s", sensor_name);
        num_to_ignore_sensors++;
    } else {
        expect(fseek(f, pos, SEEK_SET) == 0);
    }
}

static bool is_sensor_name_ignored(DIR *sensor_dir) {
    int name_fd = openat(dirfd(sensor_dir), "name", O_RDONLY);
    if (name_fd < 0)
        return false;
    char sensor_name[SENSOR_NAME_MAX];
    ssize_t name_len = read(name_fd, sensor_name, SENSOR_NAME_MAX - 1);
    close(name_fd);
    if (name_len <= 0)
        return false;
    sensor_name[name_len] = '\0';
    sensor_name[strcspn(sensor_name, "\n")] = '\0';
    for (size_t i = 0; i < num_to_ignore_sensors; i++) {
        if (strcmp(sensor_name, ignored_sensors_arr[i]) == 0)
            return true;
    }
    return false;
}

static int full_speed_supported(void) {
    FILE *f = fopen(FAN_CONTROL_FILE, "re");
    char line[256]; // If exceeded, we'll just read again
    int found = 0;

    expect(f);

    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "full-speed") != NULL) {
            found = 1;
            break;
        }
    }

    fclose(f);
    return found;
}

static void add_sensor_fds(DIR *sensor_dir) {
    struct dirent *sensor_file;
    while ((sensor_file = readdir(sensor_dir)) != NULL) {
        if (strncmp(sensor_file->d_name, "temp", 4) != 0 ||
            !strstr(sensor_file->d_name, "_input"))
            continue;
        expect(num_sensor_fds < MAX_SENSOR_FDS);
        int temp_fd = openat(dirfd(sensor_dir), sensor_file->d_name, O_RDONLY);
        if (temp_fd < 0)
            continue;
        sensor_fds[num_sensor_fds++] = temp_fd;
    }
}

static void populate_sensor_fds(void) {
    int hwmon_fd = open("/sys/class/hwmon", O_RDONLY | O_DIRECTORY);
    if (hwmon_fd < 0) {
        err("open(/sys/class/hwmon): %s\n", strerror(errno));
        exit_if_first_tick();
    }
    DIR *hwmon_dir = fdopendir(hwmon_fd);
    if (!hwmon_dir) {
        err("fdopendir(/sys/class/hwmon): %s\n", strerror(errno));
        exit_if_first_tick();
    }

    struct dirent *hwmon_entry;
    while ((hwmon_entry = readdir(hwmon_dir)) != NULL) {
        int sensor_dir_fd = openat(dirfd(hwmon_dir), hwmon_entry->d_name,
                                   O_RDONLY | O_DIRECTORY);
        if (sensor_dir_fd < 0)
            continue;
        DIR *sensor_dir = fdopendir(sensor_dir_fd);
        if (!sensor_dir) {
            close(sensor_dir_fd);
            continue;
        }
        if (is_sensor_name_ignored(sensor_dir)) {
            num_ignored_sensors++;
            closedir(sensor_dir);
            continue;
        }
        add_sensor_fds(sensor_dir);
        closedir(sensor_dir);
    }
    closedir(hwmon_dir);
}

/* The kernel supports reading new values without reopening the FD */
static int read_temp_fd(int fd) {
    char buf[32];
    if (lseek(fd, 0, SEEK_SET) < 0)
        return TEMP_INVALID;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return TEMP_INVALID;
    buf[n] = '\0';
    int val;
    return (sscanf(buf, "%d", &val) == 1) ? val : TEMP_INVALID;
}

static int get_max_temp(void) {
    int max_temp = TEMP_INVALID;
    for (size_t i = 0; i < num_sensor_fds; i++) {
        int temp = read_temp_fd(sensor_fds[i]);
        max_temp = max(max_temp, temp);
    }

    if (max_temp == TEMP_INVALID) {
        err("Couldn't find any valid temperature\n");
        exit_if_first_tick();
        return TEMP_INVALID;
    }

    return MILLIC_TO_C(max_temp);
}

#define write_fan_level(level) write_fan("level", level)

static int write_fan(const char *command, const char *value) {
    FILE *f = fopen(FAN_CONTROL_FILE, "we");
    int ret;

    if (!f) {
        err("%s: fopen: %s%s\n", FAN_CONTROL_FILE, strerror(errno),
            errno == ENOENT ? " (is thinkpad_acpi loaded?)" : "");
        exit_if_first_tick();
        return -errno;
    }

    expect(setvbuf(f, NULL, _IONBF, 0) == 0); /* Make fprintf see errors */
    ret = fprintf(f, "%s %s", command, value);
    if (ret < 0) {
        err("%s: write: %s%s\n", FAN_CONTROL_FILE, strerror(errno),
            errno == EINVAL ? " (did you enable fan_control=1?)" : "");
        exit_if_first_tick();
        fclose(f);
        return -errno;
    }
    expect(clock_gettime(CLOCK_MONOTONIC_COARSE, &last_watchdog_ping) == 0);
    fclose(f);
    return 0;
}

static void write_watchdog_timeout(const time_t timeout) {
    char timeout_s[sizeof(S_DEFAULT_WATCHDOG_SECS)]; /* max timeout value */
    int ret =
        snprintf(timeout_s, sizeof(timeout_s), "%" PRIuMAX, (uintmax_t)timeout);
    expect(ret >= 0 && (size_t)ret < sizeof(timeout_s));
    write_fan("watchdog", timeout_s);
}

enum set_fan_status {
    FAN_LEVEL_NOT_SET,
    FAN_LEVEL_SET,
    FAN_LEVEL_INVALID,
};

static enum set_fan_status set_fan_level(void) {
    int max_temp = get_max_temp(), temp_penalty = 0;
    static unsigned int tick_penalty = tick_hysteresis;

    // fan-up stabilization
    static unsigned int up_delay_counter = 0;

    if (tick_penalty > 0) {
        tick_penalty--;
    }

    if (max_temp == TEMP_INVALID) {
        write_fan_level("full-speed");
        return FAN_LEVEL_INVALID;
    }

    /* Iterate num_tiers + 1 to include the "off" sentinel */
    for (size_t i = 0; i <= num_tiers; i++) {
        const struct Rule *rule = rules + i;

        if (rule == current_rule) {
            if (tick_penalty) {
                // Must wait longer until able to move down levels
                return FAN_LEVEL_NOT_SET;
            }
            temp_penalty = temp_hysteresis;
        }

        // -------- FAN-UP with Delay --------
        if (rule < current_rule) { // rule index smaller means higher fan level
            if ((rule->threshold - temp_penalty) < max_temp) {
                if (up_delay_ticks > 0) {
                    up_delay_counter++;
                    if (up_delay_counter < up_delay_ticks) {
                        return FAN_LEVEL_NOT_SET; // not ready yet
                    }
                }
                // OK, promote
                up_delay_counter = 0;
                current_rule = rule;
                tick_penalty = tick_hysteresis;
                printf("[FAN] Temperature %dC, fan UP to %s\n",
                       max_temp, rule->name);
                write_fan_level(rule->tpacpi_level);
                return FAN_LEVEL_SET;
            } else {
                up_delay_counter = 0;
            }
            continue;
        }

        // -------- FAN-DOWN (no delay) --------
        if (rule->threshold < temp_penalty ||
            (rule->threshold - temp_penalty) < max_temp) {
            if (rule != current_rule) {
                current_rule = rule;
                tick_penalty = tick_hysteresis;
                printf("[FAN] Temperature now %dC, fan set to %s\n", max_temp,
                       rule->name);
                write_fan_level(rule->tpacpi_level);
                return FAN_LEVEL_SET;
            }
            return FAN_LEVEL_NOT_SET;
        }
    }

    err("No threshold matched?\n");
    return FAN_LEVEL_INVALID;
}

#define WATCHDOG_GRACE_PERIOD_SECS 2
static void maybe_ping_watchdog(void) {
    struct timespec now;

    expect(current_rule);
    expect(clock_gettime(CLOCK_MONOTONIC_COARSE, &now) == 0);

    if (detect_suspend() == RESUME_DETECTED) {
        // On resume, some models need a manual fan write again, or they will
        // revert to "auto".
        info("Clock jump detected, possible resume. Rewriting fan level\n");
        write_fan_level(current_rule->tpacpi_level);
    }

    if (now.tv_sec - last_watchdog_ping.tv_sec <
        (watchdog_secs - WATCHDOG_GRACE_PERIOD_SECS)) {
        return;
    }

    // Transitioning from level 0 -> level 0 can cause a brief fan spinup on
    // some models, so don't reset the timer by write_fan_level().
    write_watchdog_timeout(watchdog_secs);
}

#define CONFIG_PATH "/etc/zcfan.conf"
#define fscanf_int_for_key(f, pos, name, dest)                                 \
    do {                                                                       \
        int val;                                                               \
        if (fscanf(f, name " %d ", &val) == 1) {                               \
            dest = val;                                                        \
        } else {                                                               \
            expect(fseek(f, pos, SEEK_SET) == 0);                              \
        }                                                                      \
    } while (0)

#define fscanf_str_for_key(f, pos, name, dest)                                 \
    do {                                                                       \
        char val[CONFIG_MAX_STRLEN + 1];                                       \
        if (fscanf(f, name " %" S_CONFIG_MAX_STRLEN "s ", val) == 1) {         \
            snprintf(dest, sizeof(dest), "%s", val);                           \
        } else {                                                               \
            expect(fseek(f, pos, SEEK_SET) == 0);                              \
        }                                                                      \
    } while (0)

/* Compare rules by threshold descending for qsort */
static int rule_cmp_desc(const void *a, const void *b) {
    const struct Rule *ra = (const struct Rule *)a;
    const struct Rule *rb = (const struct Rule *)b;
    if (rb->threshold > ra->threshold) return 1;
    if (rb->threshold < ra->threshold) return -1;
    return 0;
}

/* Try to parse a "tier <temp> <level>" line. Returns 1 on success, 0 otherwise. */
static int fscanf_tier(FILE *f, long pos) {
    int temp;
    char level[CONFIG_MAX_STRLEN + 1];

    if (fscanf(f, "tier %d %" S_CONFIG_MAX_STRLEN "s ", &temp, level) == 2) {
        if (num_tiers >= MAX_TIERS) {
            err("%s: too many tiers (max %d)\n", CONFIG_PATH, MAX_TIERS);
            exit(1);
        }
        struct Rule *r = &rules[num_tiers];
        snprintf(r->tpacpi_level, sizeof(r->tpacpi_level), "%s", level);
        r->threshold = temp;
        snprintf(r->name, sizeof(r->name), "tier%zu", num_tiers + 1);
        num_tiers++;
        return 1;
    }

    expect(fseek(f, pos, SEEK_SET) == 0);
    return 0;
}

static void get_config(void) {
    FILE *f;
    int up_delay_ticks_tmp = (int)up_delay_ticks;
    bool has_tier_lines = false;

    /* Legacy config holders — only applied if no tier lines found */
    int legacy_max_temp = 90, legacy_med_temp = 80, legacy_low_temp = 70;
    char legacy_max_level[CONFIG_MAX_STRLEN + 1] = "full-speed";
    char legacy_med_level[CONFIG_MAX_STRLEN + 1] = "4";
    char legacy_low_level[CONFIG_MAX_STRLEN + 1] = "1";
    bool has_legacy_keys = false;

    f = fopen(CONFIG_PATH, "re");
    if (!f) {
        if (errno != ENOENT) {
            err("%s: fopen: %s\n", CONFIG_PATH, strerror(errno));
            exit_if_first_tick();
        }
        return;
    }

    /* Reset tiers for fresh parse — defaults already set by init_default_rules() */
    num_tiers = 0;

    while (!feof(f)) {
        long pos = ftell(f);
        int ch;
        expect(pos >= 0);

        /* Try tier lines first */
        if (fscanf_tier(f, pos)) {
            has_tier_lines = true;
            continue;
        }

        /* Legacy keys — parse into temp holders */
        {
            long before = ftell(f);
            fscanf_int_for_key(f, pos, "max_temp", legacy_max_temp);
            if (ftell(f) != before) { has_legacy_keys = true; continue; }
        }
        {
            long before = ftell(f);
            fscanf_int_for_key(f, pos, "med_temp", legacy_med_temp);
            if (ftell(f) != before) { has_legacy_keys = true; continue; }
        }
        {
            long before = ftell(f);
            fscanf_int_for_key(f, pos, "low_temp", legacy_low_temp);
            if (ftell(f) != before) { has_legacy_keys = true; continue; }
        }
        {
            long before = ftell(f);
            fscanf_str_for_key(f, pos, "max_level", legacy_max_level);
            if (ftell(f) != before) { has_legacy_keys = true; continue; }
        }
        {
            long before = ftell(f);
            fscanf_str_for_key(f, pos, "med_level", legacy_med_level);
            if (ftell(f) != before) { has_legacy_keys = true; continue; }
        }
        {
            long before = ftell(f);
            fscanf_str_for_key(f, pos, "low_level", legacy_low_level);
            if (ftell(f) != before) { has_legacy_keys = true; continue; }
        }

        /* Common keys */
        fscanf_int_for_key(f, pos, "watchdog_secs", watchdog_secs);
        fscanf_int_for_key(f, pos, "temp_hysteresis", temp_hysteresis);
        fscanf_int_for_key(f, pos, "up_delay_ticks", up_delay_ticks_tmp);
        fscanf_ignore_sensor(f, pos);
        if (ftell(f) == pos) {
            while ((ch = fgetc(f)) != EOF && ch != '\n') {}
        }
    }

    fclose(f);

    if (has_tier_lines) {
        /* Warn if legacy keys were also present */
        if (has_legacy_keys) {
            info("tier lines found; ignoring legacy max_temp/med_temp/low_temp keys\n");
        }
        /* Sort tiers descending by threshold */
        qsort(rules, num_tiers, sizeof(struct Rule), rule_cmp_desc);

        /* Validate no duplicate thresholds */
        for (size_t i = 1; i < num_tiers; i++) {
            if (rules[i].threshold == rules[i - 1].threshold) {
                err("%s: duplicate tier threshold %dC\n",
                    CONFIG_PATH, rules[i].threshold);
                exit(1);
            }
        }

        /* Re-number tier names after sort */
        for (size_t i = 0; i < num_tiers; i++) {
            snprintf(rules[i].name, sizeof(rules[i].name), "tier%zu", i + 1);
        }
    } else {
        /* Legacy mode: build 3 rules from legacy holders */
        num_tiers = 3;
        rules[0] = (struct Rule){{0}, legacy_max_temp, "maximum"};
        snprintf(rules[0].tpacpi_level, sizeof(rules[0].tpacpi_level),
                 "%s", legacy_max_level);

        rules[1] = (struct Rule){{0}, legacy_med_temp, "medium"};
        snprintf(rules[1].tpacpi_level, sizeof(rules[1].tpacpi_level),
                 "%s", legacy_med_level);

        rules[2] = (struct Rule){{0}, legacy_low_temp, "low"};
        snprintf(rules[2].tpacpi_level, sizeof(rules[2].tpacpi_level),
                 "%s", legacy_low_level);
    }

    /* Append "off" sentinel */
    rules[num_tiers] = (struct Rule){"0", TEMP_MIN, "off"};

    /* Maximum value handled by the kernel is 120, and
     * (watchdog_secs - WATCHDOG_GRACE_PERIOD_SECS) must stay positive. */
    if (watchdog_secs < WATCHDOG_GRACE_PERIOD_SECS ||
        watchdog_secs > DEFAULT_WATCHDOG_SECS) {
        err("%s: value for the watchdog_secs directive has to be between %d and %d\n",
            CONFIG_PATH, WATCHDOG_GRACE_PERIOD_SECS, DEFAULT_WATCHDOG_SECS);
        exit(1);
    }

    /* Commit parsed value and validate up_delay_ticks to a reasonable bound */
    up_delay_ticks = (unsigned int)up_delay_ticks_tmp;
    if (up_delay_ticks > 60) {
        err("%s: value for the up_delay_ticks directive must be between 0 and 60\n", CONFIG_PATH);
        exit(1);
    }
}

static void print_thresholds(void) {
    for (size_t i = 0; i < num_tiers; i++) {
        const struct Rule *rule = rules + i;
        printf("[CFG] At %dC fan is set to %s (level %s)\n",
               rule->threshold, rule->name, rule->tpacpi_level);
    }
    if (up_delay_ticks > 0) {
        printf("[CFG] Fan ramp-up delay: %u tick%s\n",
               up_delay_ticks, up_delay_ticks == 1 ? "" : "s");
    }
    printf("[CFG] Ignored %zu present sensors based on config\n",
           num_ignored_sensors);
}

static void stop(int sig) {
    (void)sig;
    run = 0;
}

static void handle_sigpwr(int sig) {
    (void)sig;
    pending_sleep = 1;
}

static void handle_sigusr2(int sig) {
    (void)sig;
    pending_resume = 1;
}

int main(int argc, char *argv[]) {
    const struct sigaction sa_exit = {
        .sa_handler = stop,
    };

    (void)argv;

    if (argc != 1) {
        printf("zcfan: Zero-configuration ThinkPad fan daemon.\n\n");
        printf("  [any argument]     Show this help\n\n");
        printf("See the zcfan(1) man page for details.\n");
        return 0;
    }

    init_default_rules();
    get_config();
    expect(sigaction(SIGTERM, &sa_exit, NULL) == 0);
    expect(sigaction(SIGINT, &sa_exit, NULL) == 0);
    expect(sigaction(SIGPWR,
                     &(const struct sigaction){.sa_handler = handle_sigpwr},
                     NULL) == 0);
    expect(sigaction(SIGUSR2,
                     &(const struct sigaction){.sa_handler = handle_sigusr2},
                     NULL) == 0);

    expect(setvbuf(stdout, output_buf, _IOLBF, sizeof(output_buf)) == 0);

    /* Check if the highest tier uses "full-speed" and it's not supported */
    if (strcmp(rules[0].tpacpi_level, "full-speed") == 0 &&
        !full_speed_supported()) {
        err("level \"full-speed\" not supported, using level 7\n");
        snprintf(rules[0].tpacpi_level, sizeof(rules[0].tpacpi_level), "7");
    }

    write_watchdog_timeout(watchdog_secs);
    populate_sensor_fds();
    print_thresholds();

    int fan_control_enabled = 1;

    while (run) {
        if (fan_control_enabled) {
            enum set_fan_status set = set_fan_level();
            if (set != FAN_LEVEL_SET) {
                maybe_ping_watchdog();
            }
        }
        if (run) {
            sleep(1);
            first_tick = 0;
        }
        if (pending_sleep) {
            pending_sleep = 0;
            info("Fan control disabled for sleep\n");
            if (write_fan_level("auto") == 0)
                write_watchdog_timeout(0);
            fan_control_enabled = 0;
        }
        if (pending_resume) {
            pending_resume = 0;
            info("Fan control enabled for resume\n");
            fan_control_enabled = 1;
            expect(current_rule);
            write_fan_level(current_rule->tpacpi_level);
            write_watchdog_timeout(watchdog_secs);
        }
    }

    printf("[FAN] Quit requested, reenabling thinkpad_acpi fan control\n");
    if (write_fan_level("auto") == 0) {
        write_watchdog_timeout(0);
    }
    for (size_t i = 0; i < num_sensor_fds; i++) {
        close(sensor_fds[i]);
    }
}

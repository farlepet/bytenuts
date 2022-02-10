#include <ncurses.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "bytenuts.h"
#include "cheerios.h"
#include "ingest.h"

#define USAGE ( \
"USAGE\n\n" \
"bytenuts [OPTIONS] <serial path>\n" \
"\nConfigs get loaded from ${HOME}/.bytenuts/config (if file exists)\n" \
"\nOPTIONS\n\n" \
"-h: show this help\n" \
"-b <baud>: set a baud rate (default 115200)\n" \
"-l <path>: log all output to the given file\n" \
"-c <path>: load a config from the given path rather than the default\n" \
"--colors=<0|1>: turn 8-bit ANSI colors off/on\n" \
"--echo=<0|1>: turn input echoing off/on\n" \
"--no_crlf=<0|1>: choose to send LF and not CRLF on input\n" \
"--escape=<char>: change the default ctrl+b escape character\n" \
)

static int parse_args(int argc, char **argv);
static int load_configs();
static speed_t string_to_speed(const char *speed);
static const char *speed_to_string(speed_t speed);

static bytenuts_t bytenuts;

int
bytenuts_run(int argc, char **argv)
{
    memset(&bytenuts, 0, sizeof(bytenuts_t));

    if (parse_args(argc, argv)) {
        printf(USAGE);
        return -1;
    }

    if (load_configs()) {
        return -1;
    }

    bytenuts.serial_fd = serial_open(bytenuts.config.serial_path, bytenuts.config.baud);
    if (bytenuts.serial_fd < 0) {
        return -1;
    }

    /* use pseudo-terminals for testing purposes */
    if (!strcmp(bytenuts.config.serial_path, "/dev/ptmx")) {
        grantpt(bytenuts.serial_fd);
        unlockpt(bytenuts.serial_fd);
    }

    // Initialize ncurses
    initscr();
    raw();
    noecho();
#if 0
    mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
#endif

    // setup status, output, and input windows
    bytenuts.status_win = newwin(1, COLS, LINES - 2, 0);
    wmove(bytenuts.status_win, 0, 0);
    wrefresh(bytenuts.status_win);
    bytenuts.out_win = newwin(LINES - 2, COLS, 0, 0);
    wmove(bytenuts.out_win, 0, 0);
    wrefresh(bytenuts.out_win);
    bytenuts.in_win = newwin(1, COLS, LINES - 1, 0);
    keypad(bytenuts.in_win, TRUE);
    wtimeout(bytenuts.in_win, 1);
    wmove(bytenuts.in_win, 0, 0);
    wrefresh(bytenuts.in_win);

    bytenuts.ingest_status = strdup("");
    bytenuts.cheerios_status = strdup("");
    bytenuts_set_status(STATUS_BYTENUTS, "%s", bytenuts.config.serial_path);

    // Initialize in and out threads
    if (pthread_mutex_init(&bytenuts.lock, NULL)) {
        return -1;
    }

    if (pthread_mutex_init(&bytenuts.term_lock, NULL)) {
        return -1;
    }

    if (pthread_cond_init(&bytenuts.stop_cond, NULL)) {
        return -1;
    }

    cheerios_start(&bytenuts);
    ingest_start(&bytenuts);

    if (!strcmp(bytenuts.config.serial_path, "/dev/ptmx")) {
        char info[128];
        snprintf(info, sizeof(info), "Opened PTY port %s", ptsname(bytenuts.serial_fd));
        cheerios_info(info);
    }

    pthread_mutex_lock(&bytenuts.lock);
    pthread_cond_wait(&bytenuts.stop_cond, &bytenuts.lock);
    pthread_mutex_unlock(&bytenuts.lock);

    bytenuts_kill();

    return 0;
}

int
bytenuts_stop()
{
    pthread_mutex_lock(&bytenuts.lock);
    pthread_cond_signal(&bytenuts.stop_cond);
    pthread_mutex_unlock(&bytenuts.lock);
    return 0;
}

void
bytenuts_kill()
{
    ingest_stop();
    cheerios_stop();

    delwin(bytenuts.status_win);
    delwin(bytenuts.in_win);
    delwin(bytenuts.out_win);
    endwin();

    close(bytenuts.serial_fd);
}

int
bytenuts_print_stats()
{
    char st_line[128];

    sprintf(st_line, "colors: %s\r\n", bytenuts.config.colors ? "enabled" : "disabled");
    cheerios_insert(st_line, strlen(st_line));
    sprintf(st_line, "echo: %s\r\n", bytenuts.config.echo ? "enabled" : "disabled");
    cheerios_insert(st_line, strlen(st_line));
    sprintf(st_line, "no_crlf: %s\r\n", bytenuts.config.no_crlf ? "enabled" : "disabled");
    cheerios_insert(st_line, strlen(st_line));
    sprintf(st_line, "escape: %c\r\n", bytenuts.config.escape);
    cheerios_insert(st_line, strlen(st_line));
    sprintf(st_line, "baud: %s\r\n", speed_to_string(bytenuts.config.baud));
    cheerios_insert(st_line, strlen(st_line));
    sprintf(st_line, "config_path: %s\r\n", bytenuts.config.config_path);
    cheerios_insert(st_line, strlen(st_line));
    sprintf(st_line, "log_path: %s\r\n", bytenuts.config.log_path);
    cheerios_insert(st_line, strlen(st_line));
    sprintf(st_line, "serial_path: %s\r\n", bytenuts.config.serial_path);
    cheerios_insert(st_line, strlen(st_line));

    return 0;
}

int
bytenuts_set_status(int user, const char *fmt, ...)
{
    int cx, cy;
    char *new_status;
    int len;
    va_list ap;

    va_start(ap, fmt);
    len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    new_status = calloc(1, len + 1);
    va_start(ap, fmt);
    vsnprintf(new_status, len+1, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&bytenuts.lock);
    switch (user) {
    case STATUS_BYTENUTS:
        if (bytenuts.bytenuts_status)
            free(bytenuts.bytenuts_status);
        bytenuts.bytenuts_status = new_status;
        break;
    case STATUS_INGEST:
        if (bytenuts.ingest_status)
            free(bytenuts.ingest_status);
        bytenuts.ingest_status = new_status;
        break;
    case STATUS_CHEERIOS:
        if (bytenuts.cheerios_status)
            free(bytenuts.cheerios_status);
        bytenuts.cheerios_status = new_status;
        break;
    case STATUS_CMDPAGE:
        if (bytenuts.cmdpg_status)
            free(bytenuts.cmdpg_status);
        bytenuts.cmdpg_status = new_status;
        break;
    default:
        free(new_status);
        pthread_mutex_unlock(&bytenuts.lock);
        return 0;
    }
    pthread_mutex_unlock(&bytenuts.lock);

    pthread_mutex_lock(&bytenuts.term_lock);

    getyx(bytenuts.in_win, cy, cx);
    curs_set(0);
    wmove(bytenuts.status_win, 0, 0);

    int width = getmaxx(bytenuts.status_win);
    for (int i = 0; i < width - 1; i++) {
        waddch(bytenuts.status_win, '-');
    }
    waddch(bytenuts.status_win, '|');
    wmove(bytenuts.status_win, 0, 0);
    wprintw(
        bytenuts.status_win, "|--%s--|--%s--|--%s--|--%s--|",
        bytenuts.bytenuts_status, bytenuts.ingest_status,
        bytenuts.cheerios_status, bytenuts.cmdpg_status
    );

    wmove(bytenuts.in_win, cy, cx);
    curs_set(1);
    wrefresh(bytenuts.status_win);
    refresh();

    pthread_mutex_unlock(&bytenuts.term_lock);

    return 0;
}

int
bytenuts_update_screen_size()
{
    pthread_mutex_lock(&bytenuts.term_lock);

    wresize(bytenuts.status_win, 1, COLS);
    mvwin(bytenuts.status_win, LINES - 2, 0);
    wrefresh(bytenuts.status_win);

    wresize(bytenuts.out_win, LINES - 2, COLS);
    mvwin(bytenuts.out_win, 0, 0);
    wrefresh(bytenuts.out_win);

    wresize(bytenuts.in_win, 1, COLS);
    mvwin(bytenuts.in_win, LINES - 1, 0);
    wrefresh(bytenuts.in_win);

    pthread_mutex_unlock(&bytenuts.term_lock);

    cheerios_insert("", 0);

    return 0;
}

static int
parse_args(int argc, char **argv)
{
    if (argc < 2)
        return -1;

    if (argc == 2 && !strcmp(argv[1], "-h"))
        return -1;

    /* setup default options first */
    bytenuts.config = CONFIG_DEFAULT;
    {
        char *home = getenv("HOME");
        char path[] = "/.config/bytenuts/config";
        bytenuts.config.config_path = malloc(strlen(home) + strlen(path) + 1);
        sprintf(bytenuts.config.config_path, "%s%s", home, path);
    }

    for (int i = 1; i < argc - 1; i++) {
        size_t arg_len = strlen(argv[i]);

        if (!strcmp(argv[i], "-h")) {
            return -1;
        }
        else if (!strcmp(argv[i], "-b")) {
            i++;
            if (i == argc - 1)
                return -1;

            bytenuts.config.baud = string_to_speed(argv[i]);
        }
        else if (!strcmp(argv[i], "-l")) {
            i++;
            if (i == argc - 1)
                return -1;

            bytenuts.config.log_path = strdup(argv[i]);
        }
        else if (!strcmp(argv[i], "-c")) {
            i++;
            if (i == argc - 1)
                return -1;

            bytenuts.config.config_path = strdup(argv[i]);
        }
        else if (arg_len == 10 && !memcmp(argv[i], "--colors=", 9)) {
            if (argv[i][9] == '1') {
                bytenuts.config.colors = 1;
            } else if (argv[i][9] == '0') {
                bytenuts.config.colors = 0;
            }
            bytenuts.config_overrides[0] = 1;
        }
        else if (arg_len == 8 && !memcmp(argv[i], "--echo=", 7)) {
            if (argv[i][7] == '1') {
                bytenuts.config.echo = 1;
            } else if (argv[i][7] == '0') {
                bytenuts.config.echo = 0;
            }
            bytenuts.config_overrides[1] = 1;
        }
        else if (arg_len == 11 && !memcmp(argv[i], "--no_crlf=", 10)) {
            if (argv[i][10] == '1') {
                bytenuts.config.no_crlf = 1;
            } else if (argv[i][10] == '0') {
                bytenuts.config.no_crlf = 0;
            }
            bytenuts.config_overrides[2] = 1;
        }
        else if (arg_len == 10 && !memcmp(argv[i], "--escape=", 9)) {
            bytenuts.config.escape = argv[i][9];
            bytenuts.config_overrides[3] = 1;
        }
        else {
            return -1;
        }
    }

    bytenuts.config.serial_path = strdup(argv[argc - 1]);

    return 0;
}

static int
load_configs()
{
    FILE *fd = NULL;
    char line[128];

    fd = fopen(bytenuts.config.config_path, "r");

    if (!fd) {
        return 0;
    }

    while (fgets(line, sizeof(line), fd) != NULL) {
        if (!bytenuts.config_overrides[0] && !memcmp(line, "colors=", 7)) {
            if (line[7] == '0')
                bytenuts.config.colors = 0;
            else if (line[7] == '1')
                bytenuts.config.colors = 1;
        }
        else if (!bytenuts.config_overrides[1] && !memcmp(line, "echo=", 5)) {
            if (line[5] == '0')
                bytenuts.config.echo = 0;
            else if (line[5] == '1')
                bytenuts.config.echo = 1;
        }
        else if (!bytenuts.config_overrides[2] && !memcmp(line, "no_crlf=", 8)) {
            if (line[8] == '0')
                bytenuts.config.no_crlf = 0;
            else if (line[8] == '1')
                bytenuts.config.no_crlf = 1;
        }
        else if (!bytenuts.config_overrides[3] && !memcmp(line, "escape=", 7)) {
            bytenuts.config.escape = line[7];
        }
    }

    return 0;
}

static speed_t
string_to_speed(const char *speed)
{
    if (!strcmp("50", speed))
        return B50;
    else if (!strcmp("75", speed))
        return B75;
    else if (!strcmp("110", speed))
        return B110;
    else if (!strcmp("134", speed))
        return B134;
    else if (!strcmp("150", speed))
        return B150;
    else if (!strcmp("200", speed))
        return B200;
    else if (!strcmp("300", speed))
        return B300;
    else if (!strcmp("600", speed))
        return B600;
    else if (!strcmp("1200", speed))
        return B1200;
    else if (!strcmp("1800", speed))
        return B1800;
    else if (!strcmp("2400", speed))
        return B2400;
    else if (!strcmp("4800", speed))
        return B4800;
    else if (!strcmp("9600", speed))
        return B9600;
    else if (!strcmp("19200", speed))
        return B19200;
    else if (!strcmp("38400", speed))
        return B38400;
    else if (!strcmp("57600", speed))
        return B57600;
    else if (!strcmp("115200", speed))
        return B1152000;
    else if (!strcmp("230400", speed))
        return B230400;
    else if (!strcmp("460800", speed))
        return B460800;
    else if (!strcmp("500000", speed))
        return B500000;
    else if (!strcmp("576000", speed))
        return B576000;
    else if (!strcmp("921600", speed))
        return B921600;
    else if (!strcmp("1000000", speed))
        return B1000000;
    else if (!strcmp("1152000", speed))
        return B1152000;
    else if (!strcmp("1500000", speed))
        return B1500000;
    else if (!strcmp("2000000", speed))
        return B2000000;
    else if (!strcmp("2500000", speed))
        return B2500000;
    else if (!strcmp("3000000", speed))
        return B3000000;
    else if (!strcmp("3500000", speed))
        return B3500000;
    else if (!strcmp("4000000", speed))
        return B4000000;
    else
        return B0;
}

static const char *
speed_to_string(speed_t speed)
{
    if (speed == B50)
        return "B50";
    else if (speed == B75)
        return "B75";
    else if (speed == B110)
        return "B110";
    else if (speed == B134)
        return "B134";
    else if (speed == B150)
        return "B150";
    else if (speed == B200)
        return "B200";
    else if (speed == B300)
        return "B300";
    else if (speed == B600)
        return "B600";
    else if (speed == B1200)
        return "B1200";
    else if (speed == B1800)
        return "B1800";
    else if (speed == B2400)
        return "B2400";
    else if (speed == B4800)
        return "B4800";
    else if (speed == B9600)
        return "B9600";
    else if (speed == B19200)
        return "B19200";
    else if (speed == B38400)
        return "B38400";
    else if (speed == B57600)
        return "B57600";
    else if (speed == B115200)
        return "B1152000";
    else if (speed == B230400)
        return "B230400";
    else if (speed == B460800)
        return "B460800";
    else if (speed == B500000)
        return "B500000";
    else if (speed == B576000)
        return "B576000";
    else if (speed == B921600)
        return "B921600";
    else if (speed == B1000000)
        return "B1000000";
    else if (speed == B1152000)
        return "B1152000";
    else if (speed == B1500000)
        return "B1500000";
    else if (speed == B2000000)
        return "B2000000";
    else if (speed == B2500000)
        return "B2500000";
    else if (speed == B3000000)
        return "B3000000";
    else if (speed == B3500000)
        return "B3500000";
    else if (speed == B4000000)
        return "B4000000";
    else
        return "";
}
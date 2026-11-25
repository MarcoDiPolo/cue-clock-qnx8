#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef __QNXNTO__
#define _QNX_SOURCE 1
#endif
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif
#ifdef __QNXNTO__
#include <signal.h>
#include <sys/neutrino.h>
#include <sys/utsname.h>
#endif

/* The sheet is small enough to validate before the presentation starts. */
enum { max_cues = 256, max_caption = 231, max_show_ms = 3600000 };
typedef struct {
    long start_ms, end_ms;
    char caption[max_caption + 1];
} Cue;

static int parse_ms(const char *s, long *value) {
    char *end;
    if (!isdigit((unsigned char)*s)) return 0;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || *end || n > max_show_ms) return 0;
    *value = n;
    return 1;
}

static int valid_caption(const unsigned char *p) {
    while (*p) {
        unsigned char first = *p++;
        if (first < 0x80) {
            if (first < 0x20 || first == 0x7f) return 0;
            continue;
        }
        if (first < 0xc2 || first > 0xf4) return 0;
        unsigned trailing = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
        if ((first == 0xc2 && *p < 0xa0) || (first == 0xe0 && *p < 0xa0) ||
            (first == 0xed && *p >= 0xa0) || (first == 0xf0 && *p < 0x90) ||
            (first == 0xf4 && *p > 0x8f)) return 0;
        for (unsigned i = 0; i < trailing; ++i)
            if ((*p++ & 0xc0) != 0x80) return 0;
    }
    return 1;
}

static int read_sheet(const char *name, Cue *cues, size_t *count) {
    FILE *f = fopen(name, "rb");
    if (!f) { perror(name); return 0; }
    char line[512];
    unsigned row = 0;
    for (;;) {
        size_t n = 0;
        int ch = fgetc(f);
        if (ch == EOF) break;
        ++row;
        while (ch != EOF && ch != '\n') {
            if (ch == 0 || n == sizeof line - 1) goto invalid;
            line[n++] = (char)ch;
            ch = fgetc(f);
        }
        if (n && line[n-1] == '\r' && ch == '\n') --n;
        line[n] = '\0';
        if (memchr(line, '\r', n)) goto invalid;
        if (!*line || line[0] == '#') continue;
        char *a = strchr(line, '|');
        if (!a || *count == max_cues) goto invalid;
        *a++ = 0;
        char *b = strchr(a, '|');
        if (!b) goto invalid;
        *b++ = 0;
        Cue *c = cues + *count;
        if (!parse_ms(line, &c->start_ms) || !parse_ms(a, &c->end_ms) ||
            c->end_ms <= c->start_ms || (*count && c->start_ms < cues[*count-1].end_ms) ||
            !*b || strlen(b) > max_caption) goto invalid;
        if (!valid_caption((const unsigned char *)b)) goto invalid;
        strcpy(c->caption, b);
        ++*count;
    }
    if (ferror(f)) { perror(name); fclose(f); return 0; }
    fclose(f);
    if (!*count) fprintf(stderr, "Empty cue sheet.\n");
    return *count > 0;
invalid:
    fprintf(stderr, "Invalid cue sheet at line %u.\n", row);
    fclose(f);
    return 0;
}

static int64_t now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency)) return -1;
    return (counter.QuadPart / frequency.QuadPart) * INT64_C(1000000000) +
           (counter.QuadPart % frequency.QuadPart) * INT64_C(1000000000) / frequency.QuadPart;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return (int64_t)t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
#endif
}

#ifdef __QNXNTO__
static int deadline_channel, deadline_connection;
static timer_t deadline_timer;

static void close_deadline_timer(void) {
    timer_delete(deadline_timer);
    ConnectDetach(deadline_connection);
    ChannelDestroy(deadline_channel);
}

static int open_deadline_timer(void) {
    struct sigevent event = {0};
    deadline_channel = ChannelCreate(_NTO_CHF_PRIVATE);
    if (deadline_channel < 0) { perror("ChannelCreate"); return 0; }
    deadline_connection = ConnectAttach(0, 0, deadline_channel, _NTO_SIDE_CHANNEL, 0);
    if (deadline_connection < 0) {
        perror("ConnectAttach");
        ChannelDestroy(deadline_channel);
        return 0;
    }
    SIGEV_PULSE_INIT(&event, deadline_connection, SIGEV_PULSE_PRIO_INHERIT, _PULSE_CODE_MINAVAIL, 0);
    if (timer_create(CLOCK_MONOTONIC, &event, &deadline_timer) < 0) {
        perror("timer_create");
        ConnectDetach(deadline_connection);
        ChannelDestroy(deadline_channel);
        return 0;
    }
    return 1;
}
#endif

static int wait_until(int64_t target) {
#ifdef _WIN32
    for (;;) {
        int64_t now = now_ns();
        if (now < 0) return 0;
        int64_t left = target - now;
        if (left <= 0) return 1;
        Sleep((DWORD)((left + 999999) / 1000000));
    }
#elif defined(__QNXNTO__)
    struct itimerspec alarm = {0};
    struct _pulse pulse;
    alarm.it_value.tv_sec = (time_t)(target / 1000000000);
    alarm.it_value.tv_nsec = (long)(target % 1000000000);
    if (timer_settime(deadline_timer, TIMER_ABSTIME, &alarm, NULL) < 0) {
        perror("timer_settime");
        return 0;
    }
    for (;;) {
        int result = MsgReceivePulse(deadline_channel, &pulse, sizeof pulse, NULL);
        if (result >= 0 && pulse.code == _PULSE_CODE_MINAVAIL) return 1;
        if (result < 0 && errno != EINTR) { perror("MsgReceivePulse"); return 0; }
    }
#else
    struct timespec due = {(time_t)(target / 1000000000), (long)(target % 1000000000)};
    int rc;
    do { rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL); } while (rc == EINTR);
    if (rc) { errno = rc; perror("clock_nanosleep"); }
    return rc == 0;
#endif
}

static void vtt_time(long ms) {
    printf("%02ld:%02ld:%02ld.%03ld", ms/3600000, ms/60000%60, ms/1000%60, ms%1000);
}

static int export_vtt(const Cue *cues, size_t n) {
    puts("WEBVTT\n");
    for (size_t i = 0; i < n; ++i) {
        vtt_time(cues[i].start_ms);
        fputs(" --> ", stdout);
        vtt_time(cues[i].end_ms);
        putchar('\n');
        for (const char *p = cues[i].caption; *p; ++p) {
            if (*p == '&') fputs("&amp;", stdout);
            else if (*p == '<') fputs("&lt;", stdout);
            else if (*p == '>') fputs("&gt;", stdout);
            else putchar(*p);
        }
        puts("\n");
    }
    return !ferror(stdout);
}

typedef struct {
    unsigned briefly_shown, skipped, timed_out;
} OperatorDecisions;

#ifndef _WIN32
static int choice_input = STDIN_FILENO;

static int open_operator_input(void) {
    if (!isatty(STDIN_FILENO)) return 1;
    const char *terminal = ttyname(STDIN_FILENO);
    if (!terminal) { perror("operator terminal"); return 0; }
    choice_input = open(terminal, O_RDONLY | O_NONBLOCK | O_NOCTTY);
    if (choice_input < 0) { perror("open operator terminal"); return 0; }
    return 1;
}
#endif

static int operator_key(void) {
    unsigned char key;
#ifdef _WIN32
    if (_isatty(_fileno(stdin))) return _kbhit() ? _getch() : 0;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (GetFileType(input) == FILE_TYPE_PIPE) {
        DWORD available = 0;
        if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL) || !available) return 0;
    } else if (GetFileType(input) != FILE_TYPE_DISK) {
        return 0;
    }
    return _read(_fileno(stdin), &key, 1) == 1 ? key : 0;
#else
    struct pollfd input = {choice_input, POLLIN, 0};
    if (poll(&input, 1, 0) < 1 || !(input.revents & POLLIN)) return 0;
    return read(choice_input, &key, 1) == 1 ? key : 0;
#endif
}

static int offer_expired_cue(const Cue *cue, size_t number, int64_t origin,
                             int64_t next_start, OperatorDecisions *decisions) {
    const int64_t ms = 1000000;
    int64_t now = now_ns();
    if (now < 0) return 0;
    int64_t limit = now + 6500 * ms;
    if (next_start < limit) limit = next_start;
    printf("DECISION cue %zu | s: skip, b: show briefly, then Enter | available %lld ms\n",
           number, (long long)(limit > now ? (limit - now) / ms : 0));
    fflush(stdout);
    while (now < limit) {
        int key = operator_key();
        now = now_ns();
        if (now < 0) return 0;
        if (now >= limit) break;
        if (key == 's' || key == 'S') {
            ++decisions->skipped;
            printf("CHOICE cue %zu | skip\n", number);
            return 1;
        }
        if (key == 'b' || key == 'B') {
            int64_t clear_at = now + 650 * ms;
            if (clear_at > next_start) clear_at = next_start;
            long elapsed = (long)((now - origin) / ms);
            ++decisions->briefly_shown;
            printf("CHOICE cue %zu | brief\n", number);
            printf("[%04ld] SHOW cue %zu | late %ld | %s\n", elapsed, number,
                   elapsed - cue->start_ms, cue->caption);
            fflush(stdout);
            if (!wait_until(clear_at)) return 0;
            now = now_ns();
            if (now < 0) return 0;
            printf("[%04ld] CLEAR cue %zu | scheduled %ld\n",
                   (long)((now - origin) / ms), number, (long)((clear_at - origin) / ms));
            return 1;
        }
        int64_t next_check = now + 40 * ms;
        if (!wait_until(next_check < limit ? next_check : limit)) return 0;
        now = now_ns();
        if (now < 0) return 0;
    }
    ++decisions->timed_out;
    printf("CHOICE cue %zu | timeout\n", number);
    return 1;
}

static int perform(const Cue *cues, size_t n, long delay_cue, long delay_ms, int interactive) {
#ifdef __QNXNTO__
    struct utsname system;
    struct timespec resolution;
    if (uname(&system) == 0)
        printf("Target: %s %s %s\n", system.sysname, system.release, system.machine);
    if (clock_getres(CLOCK_MONOTONIC, &resolution) == 0)
        printf("Monotonic clock resolution: %lld ns\n",
               (long long)resolution.tv_sec * 1000000000 + resolution.tv_nsec);
    puts("Deadline delivery: private QNX timer pulses");
#endif
    int64_t origin = now_ns();
    if (origin < 0) return 0;
    printf("Cue Clock | %zu cues | delays in ms\n", n);
    size_t expired = 0;
    OperatorDecisions decisions = {0};
    for (size_t i = 0; i < n; ++i) {
        const Cue *c = cues + i;
        int64_t target = origin + (int64_t)c->start_ms * 1000000;
        if ((long)i + 1 == delay_cue) target += (int64_t)delay_ms * 1000000;
        if (!wait_until(target)) return 0;
        int64_t now = now_ns();
        if (now < 0) return 0;
        long elapsed = (long)((now - origin) / 1000000);
        if (elapsed >= c->end_ms) {
            printf("[%04ld] EXPIRED cue %zu | late %ld\n", elapsed, i+1, elapsed-c->start_ms);
            ++expired;
            if (interactive) {
                int64_t next_start = i + 1 < n ? origin + (int64_t)cues[i+1].start_ms * 1000000
                                               : now + INT64_C(7150000000);
                if (!offer_expired_cue(c, i+1, origin, next_start, &decisions)) return 0;
            }
        } else {
            printf("[%04ld] SHOW cue %zu | late %ld | %s\n", elapsed, i+1, elapsed-c->start_ms, c->caption);
            fflush(stdout);
            if (!wait_until(origin + (int64_t)c->end_ms * 1000000)) return 0;
            now = now_ns();
            if (now < 0) return 0;
            elapsed = (long)((now - origin) / 1000000);
            printf("[%04ld] CLEAR cue %zu | scheduled %ld\n", elapsed, i+1, c->end_ms);
        }
        fflush(stdout);
    }
    printf("Summary: %zu presented, %zu expired.\n", n-expired, expired);
    if (interactive) printf("Operator: %u briefly shown, %u skipped, %u timed out.\n",
                            decisions.briefly_shown, decisions.skipped, decisions.timed_out);
    return !ferror(stdout);
}

int main(int argc, char **argv) {
    long delay_cue = 0, delay_ms = 0;
    int interactive = argc >= 3 && strcmp(argv[argc-1], "--operator") == 0;
    if (interactive) --argc;
    int vtt = argc == 3 && strcmp(argv[2], "--vtt") == 0;
    int delay = argc == 5 && strcmp(argv[2], "--delay-cue") == 0 &&
                parse_ms(argv[3], &delay_cue) && delay_cue > 0 && parse_ms(argv[4], &delay_ms);
    if ((argc != 2 && !vtt && !delay) || (vtt && interactive)) {
        fprintf(stderr, "Usage: cue-clock sheet.txt [--vtt | [--delay-cue NUMBER MILLISECONDS] [--operator]]\n");
        return 2;
    }
    Cue cues[max_cues];
    size_t n = 0;
    if (!read_sheet(argv[1], cues, &n) || (size_t)delay_cue > n) return 2;
    if (vtt) return export_vtt(cues, n) ? 0 : 1;
#ifndef _WIN32
    if (interactive && !open_operator_input()) return 1;
#endif
#ifdef __QNXNTO__
    if (!open_deadline_timer()) return 1;
#endif
    int completed = perform(cues, n, delay_cue, delay_ms, interactive);
#ifdef __QNXNTO__
    close_deadline_timer();
#endif
#ifndef _WIN32
    if (choice_input != STDIN_FILENO) close(choice_input);
#endif
    return completed ? 0 : 1;
}

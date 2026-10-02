/* Native firmware/audio worker for frontend.py. stdin/stdout are a private line protocol.
 * Starts paused. Every unit operation runs on this thread; keyboard ACKs happen in the frontend.
 * No device services or keyboard drivers are changed. See README.md for the protocol.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include "audio_linux.h"
#include "audio_pace.h"
#include "emu_unit.h"
#include "tns_term.h"

static volatile sig_atomic_t stopping;
static emu_unit *unit;
static audio_out *audio;
static const char *save_path, *device, *firmware;
static int rate, paused = 1, failed;
static int buffer_mode = AP_LONG, idle_sound = 3, keep_open = 1, pop_click = 1, tick = 1, quick_keys;
static double next_tick, next_save;
static unsigned char bars_queue[64];
static int bars_count, bars_last;
static double bars_next;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void stop(int sig) { (void)sig; stopping = 1; }

static int save(void)
{
    char tmp[PATH_MAX];
    int fd, ok;
    if (snprintf(tmp, sizeof tmp, "%s.new", save_path) >= (int)sizeof tmp)
        return 0;
    ok = emu_save(unit, tmp);
    if (ok) {
        fd = open(tmp, O_RDONLY);
        ok = fd >= 0 && fsync(fd) == 0;
        if (fd >= 0) close(fd);
    }
    if (ok) ok = rename(tmp, save_path) == 0;
    if (!ok) {
        unlink(tmp);
        puts("ERROR Could not save the unit's memory.");
    } else {
        next_save = now() + 60;
        puts("SAVED");
    }
    return ok;
}

static void pause_unit(void)
{
    emu_keys_down(unit, 0);
    emu_braille_bars(unit, 0);
    bars_count = bars_last = 0;
    bars_next = emu_time(unit) + 0.12;  /* Let the firmware observe release after resuming. */
    audio_close(audio);
    audio = NULL;
    paused = 1;
}

static int valid_rate(int value)
{
    return value == 11025 || value == 16000 || value == 22050 || value == 32000 || value == 44100 || value == 48000;
}

static void configure_audio(int new_rate, const char *buffer, int sound, int keep, int pop, int ticks)
{
    char err[300];
    int mode = ap_mode_of(buffer);
    emu_unit *replacement = NULL;
    if (!paused || !valid_rate(new_rate) || mode < 0 || !strcmp(buffer, "short")
        || sound < 0 || sound > 3 || keep < 0 || keep > 2 || pop < 0 || pop > 1 || ticks < 0 || ticks > 1) {
        puts("ERROR Invalid audio settings, or unit is not paused.");
        return;
    }
    if (new_rate != rate) {
        /* Check device support before changing anything. No samples are sent during this probe. */
        if (strcmp(device, "-")) {
            audio_out *probe = audio_open(device, new_rate, 10, mode, err, sizeof err);
            if (!probe) { printf("ERROR %s\n", err); return; }
            audio_close(probe);
        }
        if (!save()) return;
        replacement = emu_create(emu_kind(unit), firmware, save_path, new_rate, 0, err, sizeof err);
        if (!replacement) { printf("ERROR %s\n", err); return; }
        emu_set_quick(replacement, quick_keys);
    }
    if (!emu_set_idle(replacement ? replacement : unit, sound, keep, pop, ticks)) {
        if (replacement) emu_destroy(replacement);
        puts("ERROR Could not configure idle audio.");
        return;
    }
    if (replacement) {
        emu_destroy(unit);
        unit = replacement;
        bars_count = bars_last = 0;
        bars_next = emu_time(unit) + 0.12;
    }
    rate = new_rate;
    buffer_mode = mode;
    idle_sound = sound;
    keep_open = keep;
    pop_click = pop;
    tick = ticks;
    puts("OK");
}

static void command(const char *line)
{
    int held, chord, quick, bars, new_rate, sound, keep, pop, ticks;
    char extra, err[300], buffer[16];
    if (!strcmp(line, "PAUSE")) {
        pause_unit();
        puts("PAUSED");
    } else if (!strcmp(line, "RESUME")) {
        if (paused && strcmp(device, "-")) {
            audio = audio_open(device, rate, 10, buffer_mode, err, sizeof err);
            if (!audio) {
                printf("ERROR %s\n", err);
                return;
            }
        }
        paused = 0;
        next_tick = now();
        puts("RUNNING");
    } else if (!strcmp(line, "SAVE")) {
        save();
    } else if (!strcmp(line, "AUDIO")) {
        printf("AUDIO %d %s %d %d %d %d\n", rate, ap_mode_name(buffer_mode), idle_sound, keep_open, pop_click, tick);
    } else if (sscanf(line, "AUDIO %d %15s %d %d %d %d %c", &new_rate, buffer, &sound, &keep, &pop, &ticks, &extra) == 6) {
        configure_audio(new_rate, buffer, sound, keep, pop, ticks);
    } else if (!strcmp(line, "BRAILLE")) {
        unsigned char cells[40];
        int i, n = emu_braille(unit, cells, sizeof cells);
        fputs("BRAILLE ", stdout);
        for (i = 0; i < n; i++) printf("%02x", cells[i]);
        putchar('\n');
    } else if (!strcmp(line, "QUIT")) {
        stopping = 1;
    } else if (sscanf(line, "BARS %d %c", &bars, &extra) == 1 && bars >= 0 && bars <= 3) {
        if (!paused && bars != bars_last) {
            if (bars_count == (int)sizeof bars_queue) {
                puts("ERROR Braille bar queue full.");
            } else {
                bars_queue[bars_count++] = (unsigned char)bars;
                bars_last = bars;
            }
        }
    } else if (sscanf(line, "QUICK %d %c", &quick, &extra) == 1 && (quick == 0 || quick == 1)) {
        emu_set_quick(unit, quick);
        quick_keys = quick;
        puts("OK");
    } else if (!strncmp(line, "TNS ", 4) && emu_kind(unit) == EMU_TYPE_N_SPEAK) {
        key_event e = {0};
        unsigned char codes[8];
        int n, i;
        e.key = key_parse(line + 4, &e.mods);
        n = tns_press_codes(&e, codes, sizeof codes);
        if (!n) {
            puts("ERROR Unknown Type 'n Speak key.");
        } else {
            for (i = 0; i < n; i++)
                if (!emu_key(unit, codes[i])) {
                    puts("ERROR Type 'n Speak keyboard queue full.");
                    return;
                }
            puts("OK");
        }
    } else if (sscanf(line, "KEY %d %d %c", &held, &chord, &extra) == 2
               && held >= 0 && held <= 255 && chord >= 0 && chord <= 255) {
        if (!paused) {
            emu_keys_down(unit, held);
            if (chord && !emu_key(unit, chord))
                puts("ERROR Keyboard queue full; the last chord was not entered.");
        }
    } else {
        puts("ERROR Invalid worker command.");
    }
}

int main(int argc, char **argv)
{
    char err[300], lock_path[PATH_MAX], line[128], bytes[256], *end;
    const char *state;
    short pcm[480];
    int lock_fd, used = 0, flags, kind = EMU_BRAILLE_LITE;
    struct sigaction sa = {0};
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "usage: blazie_bt FIRMWARE FACTORY_STATE|- SAVED_STATE AUDIO_DEVICE RATE [bl|tns]\n");
        return 2;
    }
    if (argc == 7) {
        if (!strcmp(argv[6], "tns")) kind = EMU_TYPE_N_SPEAK;
        else if (strcmp(argv[6], "bl")) return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    rate = (int)strtol(argv[5], &end, 10);
    if (*end || !valid_rate(rate)) return 2;
    firmware = argv[1];
    save_path = argv[3];
    device = argv[4];
    if (snprintf(lock_path, sizeof lock_path, "%s.lock", save_path) >= (int)sizeof lock_path) return 2;
    lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        puts("ERROR The saved unit is busy or its directory is not writable.");
        if (lock_fd >= 0) close(lock_fd);
        return 1;
    }
    sa.sa_handler = stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    state = access(save_path, F_OK) == 0 || errno != ENOENT ? save_path : argv[2];
    if (kind == EMU_TYPE_N_SPEAK && !strcmp(state, "-")) state = NULL;
    unit = emu_create(kind, argv[1], state, rate, 0, err, sizeof err);
    if (!unit) {
        printf("ERROR %s\n", err);
        close(lock_fd);
        return 1;
    }
    emu_set_idle(unit, 3, 1, 1, 1);
    flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        puts("ERROR Cannot read frontend commands.");
        failed = stopping = 1;
    }
    next_save = now() + 60;
    puts("READY");
    while (!stopping) {
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        int ready = poll(&pfd, 1, paused ? 100 : 0);
        if (ready < 0 && errno != EINTR) { failed = stopping = 1; break; }
        if (ready > 0 && pfd.revents) {
            ssize_t n = read(STDIN_FILENO, bytes, sizeof bytes), i;
            if (n == 0) { stopping = 1; break; }
            if (n < 0 && errno != EAGAIN && errno != EINTR) { failed = stopping = 1; break; }
            for (i = 0; i < n && !stopping; i++) {
                if (bytes[i] == '\n') {
                    line[used] = 0;
                    command(line);
                    used = 0;
                } else if (used < (int)sizeof line - 1) {
                    line[used++] = bytes[i];
                } else {
                    puts("ERROR Worker command too long.");
                    failed = stopping = 1;
                }
            }
        }
        if (paused || stopping) continue;
        if (audio) {
            int turn = audio_turn(audio, buffer_mode, now() >= next_save);
            if (turn == AUDIO_WAIT) continue;
            if (turn == AUDIO_SAVE) {
                double started = now();
                if (!save()) next_save = now() + 60;
                audio_saved(audio, (now() - started) * 1000);
                continue;
            }
        }
        /* Preserve short press/release pairs that arrive in one pipe read. Give the firmware's
           background polling a stable contact and a release interval, in emulated time. */
        if (bars_count && emu_time(unit) >= bars_next) {
            emu_braille_bars(unit, bars_queue[0]);
            memmove(bars_queue, bars_queue + 1, (size_t)--bars_count);
            bars_next = emu_time(unit) + 0.12;
        }
        emu_render(unit, pcm, rate / 100);
        if (audio && !audio_write(audio, pcm, rate / 100)) {
            pause_unit();
            puts("ERROR Sound output failed; the unit is paused.");
        }
        if (!audio && !paused) {
            double delay;
            struct timespec t;
            next_tick += (double)(rate / 100) / rate;
            delay = next_tick - now();
            if (delay > 0) {
                t.tv_sec = (time_t)delay;
                t.tv_nsec = (long)((delay - t.tv_sec) * 1e9);
                nanosleep(&t, NULL);
            } else if (delay < -0.1) next_tick = now();
            if (now() >= next_save && !save()) next_save = now() + 60;
        }
    }
    pause_unit();
    if (!save()) failed = 1;
    emu_destroy(unit);
    close(lock_fd);
    puts("BYE");
    return failed ? 1 : 0;
}

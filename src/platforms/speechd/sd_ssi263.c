/*
 * sd_ssi263 -- a speech-dispatcher output module: the Blazie Braille Lite 2000 speaking through the emulated
 * SSI-263 (bl_voice.h: the unit's own firmware, the C host, the chip), one process, no Python.
 *
 * The module protocol and its audio (705 blocks, HDLC-escaped, played and flushed by the server) follow
 * speech-dispatcher's module_process.c; the reply lines are TGSpeechBox's sd_tgsb's, proven on speech-dispatcher 0.11.  Synthesis is synchronous: stdin is polled
 * for STOP between blocks, and a STOP cancels the unit (its unspoken text dropped) so the next message starts clean.
 *
 * Config (argv[1], speech-dispatcher's module config; then this user's own file, whose keys win:
 * $XDG_CONFIG_HOME/ssi263-speech/sd_ssi263.conf, else ~/.config/ssi263-speech/sd_ssi263.conf; every key optional):
 *   SSI263DataDir "/usr/local/share/ssi263-speech"   BL2ENG.BNS + bl2_2003_warm.state [+ BL2SPA.BNS + bl2spa_fresh.state]
 *   SSI263SampleRate 22050     11025 | 22050 | 44100, as the add-ons (any other value: 22050)
 *   SSI263Inflection 1         the unit's voice inflection (status menu)
 *   SSI263Whine "off"          off | hiss | whine: the unit's idle sound
 *   SSI263Tone 7               0-26, the unit's tone (factory 7)
 *   SSI263ShortPauses 1        sentences packed onto one line from the second on (the NVDA driver's default)
 *   SSI263RunAhead 0           1: the NVDA driver's "Run the unit ahead" (EXPERIMENTAL, off by default; with short
 *                              pauses on only, as there): blv_set_run_ahead
 * SSI263_DATADIR overrides the data folder.  The firmware is not part of this program: it is loaded from there.
 *
 * Test hooks: SD_SSI263_TEST_NO_CANCEL=1 leaves the unit uncancelled on STOP, SD_SSI263_TEST_IGNORE_RUN_AHEAD=1 drops
 * SSI263RunAhead on its way to the voice -- the harness's controls must fail.
 */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "../../csrc/blazie/bl_voice.h"

/* ---- stdin: one raw buffer for lines and the STOP poll (never stdio on stdin) ----------------------------------- */
static char *rbuf;
static size_t rlen, rcap;
static int r_eof;

static void rappend(const char *p, size_t n)
{
    if (rlen + n + 1 > rcap) {
        size_t cap = rcap ? rcap : 8192;
        while (cap < rlen + n + 1) cap *= 2;
        rbuf = (char *)realloc(rbuf, cap);
        if (!rbuf) exit(1);
        rcap = cap;
    }
    memcpy(rbuf + rlen, p, n);
    rlen += n;
    rbuf[rlen] = 0;
}

/* the next line (without \r\n) into a malloc'd string; NULL at EOF */
static char *readline_sd(void)
{
    for (;;) {
        char *nl = rbuf ? (char *)memchr(rbuf, '\n', rlen) : NULL;
        if (nl) {
            size_t n = (size_t)(nl - rbuf);
            char *line = (char *)malloc(n + 1);
            memcpy(line, rbuf, n);
            line[n] = 0;
            if (n && line[n - 1] == '\r') line[n - 1] = 0;
            memmove(rbuf, nl + 1, rlen - n - 1);
            rlen -= n + 1;
            rbuf[rlen] = 0;
            return line;
        }
        if (r_eof) return NULL;
        {
            char buf[4096];
            ssize_t k = read(STDIN_FILENO, buf, sizeof buf);
            if (k <= 0) { r_eof = 1; continue; }
            rappend(buf, (size_t)k);
        }
    }
}

/* STOP (or CANCEL / PAUSE) waiting on stdin?  Reads what is there into the buffer, leaves it for readline_sd. */
static int poll_stop(void)
{
    fd_set f;
    struct timeval tv = {0, 0};
    FD_ZERO(&f);
    FD_SET(STDIN_FILENO, &f);
    if (select(STDIN_FILENO + 1, &f, NULL, NULL, &tv) > 0) {
        char buf[4096];
        ssize_t k = read(STDIN_FILENO, buf, sizeof buf);
        if (k <= 0) { r_eof = 1; return 1; }
        rappend(buf, (size_t)k);
    }
    return rbuf && (strstr(rbuf, "STOP") || strstr(rbuf, "CANCEL") || strstr(rbuf, "PAUSE"));
}

static void send_line(const char *s)
{
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

/* 705: 16-bit mono little-endian, the payload HDLC-escaped (0x7D and '\n' -> 0x7D, byte ^ 0x20) */
static void send_audio(const short *pcm, int n, int rate)
{
    const unsigned char *p = (const unsigned char *)pcm, *end = p + (size_t)n * 2;
    printf("705-bits=16\n705-num_channels=1\n705-sample_rate=%d\n705-num_samples=%d\n705-big_endian=0\n705-AUDIO",
           rate, n);
    fputc(0, stdout);
    for (; p < end; p++) {
        if (*p == 0x7D || *p == '\n') { fputc(0x7D, stdout); fputc(*p ^ 0x20, stdout); }
        else fputc(*p, stdout);
    }
    fputs("\n705 AUDIO\n", stdout);
    fflush(stdout);
}

/* ---- text: SSML tags dropped, XML's five entities decoded -------------------------------------------------------- */
static void strip_ssml(char *s)
{
    static const char *ent[][2] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    char *r = s, *w = s;
    int tag = 0, k;
    while (*r) {
        if (*r == '<') { tag = 1; r++; continue; }
        if (*r == '>' && tag) { tag = 0; r++; continue; }
        if (tag) { r++; continue; }
        if (*r == '&') {
            for (k = 0; k < 5; k++) {
                size_t n = strlen(ent[k][0]);
                if (!strncmp(r, ent[k][0], n)) { *w++ = ent[k][1][0]; r += n; break; }
            }
            if (k < 5) continue;
        }
        *w++ = *r++;
    }
    *w = 0;
}

/* ---- config ------------------------------------------------------------------------------------------------------ */
static char datadir[1024] = "";
static int sample_rate = 22050, inflection = 1, whine = 0, tone = 7, short_pauses = 1, run_ahead = 0;

static void read_config(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1200], key[128], val[1024];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (sscanf(line, "%127s \"%1023[^\"]\"", key, val) != 2 && sscanf(line, "%127s %1023s", key, val) != 2)
            continue;
        if (!strcmp(key, "SSI263DataDir")) snprintf(datadir, sizeof datadir, "%s", val);
        else if (!strcmp(key, "SSI263SampleRate")) sample_rate = atoi(val);
        else if (!strcmp(key, "SSI263Inflection")) inflection = atoi(val) != 0;
        else if (!strcmp(key, "SSI263Tone")) tone = atoi(val);
        else if (!strcmp(key, "SSI263ShortPauses")) short_pauses = atoi(val) != 0;
        else if (!strcmp(key, "SSI263RunAhead")) run_ahead = atoi(val) != 0;
        else if (!strcmp(key, "SSI263Whine")) whine = !strcmp(val, "hiss") ? 1 : !strcmp(val, "whine") ? 2 : 0;
    }
    fclose(f);
}

/* This user's own settings, read after the module config so their keys win (as TGSpeechBox's sd_tgsb): editable
 * without root, and kept when the voice is reinstalled. */
static void read_user_config(void)
{
    char p[1200];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(p, sizeof p, "%s/ssi263-speech/sd_ssi263.conf", xdg);
    else if (home && *home)
        snprintf(p, sizeof p, "%s/.config/ssi263-speech/sd_ssi263.conf", home);
    else
        return;
    read_config(p);
}

static void check_config(void)
{
    if (sample_rate != 11025 && sample_rate != 22050 && sample_rate != 44100) {
        fprintf(stderr, "sd_ssi263: SSI263SampleRate %d is not one of 11025, 22050, 44100: using 22050\n", sample_rate);
        sample_rate = 22050;
    }
}

static int have(const char *name)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", datadir, name);
    return access(p, R_OK) == 0;
}

static void find_datadir(void)
{
    const char *env = getenv("SSI263_DATADIR");
    if (env && *env) { snprintf(datadir, sizeof datadir, "%s", env); return; }
    if (*datadir) return;
    snprintf(datadir, sizeof datadir, "/usr/local/share/ssi263-speech");
    if (!have("BL2ENG.BNS") && access("/usr/share/ssi263-speech/BL2ENG.BNS", R_OK) == 0)
        snprintf(datadir, sizeof datadir, "/usr/share/ssi263-speech");
}

/* ---- the voices: English from the start, Spanish on first use ---------------------------------------------------- */
#define N_VOICES 2
static const char *voice_name[N_VOICES] = {"Braille Lite 2000", "Braille Lite 2000 (espa\xc3\xb1ol)"};
static const char *voice_lang[N_VOICES] = {"en-US", "es-ES"};
static const char *voice_fw[N_VOICES][2] = {{"BL2ENG.BNS", "bl2_2003_warm.state"}, {"BL2SPA.BNS", "bl2spa_fresh.state"}};
static bl_voice *voices[N_VOICES];
static int cur_voice = 0;
static int ssip_rate = 0, ssip_pitch = 0, ssip_volume = 100;

static bl_voice *voice(int i, char *err, int errlen)
{
    if (!voices[i]) {
        char fw[1200], st[1200];
        snprintf(fw, sizeof fw, "%s/%s", datadir, voice_fw[i][0]);
        snprintf(st, sizeof st, "%s/%s", datadir, voice_fw[i][1]);
        voices[i] = blv_create(fw, st, i == 1 ? BLV_CP850 : BLV_LATIN1, sample_rate, inflection, whine, err, errlen);
    }
    return voices[i];
}

static int to100(int ssip)                           /* SSIP -100..100 -> NVDA's 0..100 (0 -> 50) */
{
    int v = (ssip + 100) / 2;
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

/* ---- SPEAK ------------------------------------------------------------------------------------------------------- */
static void speak(char *text)
{
    char err[256];
    bl_voice *v = voice(cur_voice, err, sizeof err);
    const short *pcm;
    int done = 0, stopped = 0;
    strip_ssml(text);
    if (!v) { send_line("301 ERROR CANT SPEAK"); return; }
    if (poll_stop()) {                               /* already stopped (key repeat): nothing to say */
        send_line("200 OK SPEAKING");
        send_line("701 BEGIN");
        send_line("703 STOP");
        return;
    }
    send_line("200 OK SPEAKING");
    send_line("701 BEGIN");
    blv_set(v, to100(ssip_rate), to100(ssip_pitch), tone, to100(ssip_volume), short_pauses);
    blv_set_run_ahead(v, run_ahead && !getenv("SD_SSI263_TEST_IGNORE_RUN_AHEAD"));
    blv_speak(v, text);
    while (!done) {
        int n = blv_render(v, &pcm, &done);
        if (n > 0)
            send_audio(pcm, n, sample_rate);
        if (!done && poll_stop()) { stopped = 1; break; }
    }
    if (stopped) {
        if (!getenv("SD_SSI263_TEST_NO_CANCEL"))
            blv_cancel(v);
        send_line("703 STOP");
    } else {
        send_line("702 END");
    }
}

static char *read_text(void)
{
    char *text = (char *)calloc(1, 1), *line;
    size_t n = 0;
    while ((line = readline_sd()) != NULL) {
        const char *s = line;
        size_t k;
        if (!strcmp(line, ".")) { free(line); break; }
        if (s[0] == '.') s++;                        /* a leading dot is escaped as two */
        k = strlen(s);
        text = (char *)realloc(text, n + k + 2);
        if (n) text[n++] = ' ';
        memcpy(text + n, s, k);
        n += k;
        text[n] = 0;
        free(line);
    }
    return text;
}

static void set_param(char *key, const char *val)
{
    char *k;
    for (k = key; *k; k++) *k = (char)(*k >= 'A' && *k <= 'Z' ? *k + 32 : *k);
    if (!strcmp(key, "rate")) ssip_rate = atoi(val);
    else if (!strcmp(key, "pitch")) ssip_pitch = atoi(val);
    else if (!strcmp(key, "volume")) ssip_volume = atoi(val);
    else if (!strcmp(key, "synthesis_voice")) {
        int i;
        for (i = 0; i < N_VOICES; i++)
            if (!strcmp(val, voice_name[i])) cur_voice = i;
    } else if (!strcmp(key, "language")) {
        if (!strncmp(val, "es", 2) && have(voice_fw[1][0]) && have(voice_fw[1][1])) cur_voice = 1;
        else if (!strncmp(val, "en", 2)) cur_voice = 0;
    }
}

int main(int argc, char **argv)
{
    char *cmd, err[256];
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1) read_config(argv[1]);
    read_user_config();
    check_config();
    find_datadir();
    cmd = readline_sd();
    if (!cmd || strcmp(cmd, "INIT")) return 1;
    free(cmd);
    if (!voice(0, err, sizeof err)) {
        char msg[400];
        snprintf(msg, sizeof msg, "399-%s (data folder %s)", err, datadir);
        send_line(msg);
        send_line("399 ERR CANT INIT MODULE");
        return 1;
    }
    send_line("299-Braille Lite 2000 through an emulated SSI-263");
    send_line("299 OK LOADED SUCCESSFULLY");
    while ((cmd = readline_sd()) != NULL) {
        if (!strcmp(cmd, "SPEAK") || !strcmp(cmd, "CHAR") || !strcmp(cmd, "KEY") || !strcmp(cmd, "SOUND_ICON")) {
            int is_key = !strcmp(cmd, "KEY"), icon = !strcmp(cmd, "SOUND_ICON");
            char *text;
            send_line("202 OK RECEIVING MESSAGE");
            text = read_text();
            if (is_key) {
                char *c;
                for (c = text; *c; c++) if (*c == '_') *c = ' ';
            }
            if (icon || !*text) {                    /* no sound icons: an empty message still completes */
                send_line("200 OK SPEAKING");
                send_line("701 BEGIN");
                send_line("702 END");
            } else {
                speak(text);
            }
            free(text);
        } else if (!strcmp(cmd, "STOP") || !strcmp(cmd, "CANCEL") || !strcmp(cmd, "PAUSE")) {
            /* only between messages here (during one, speak() catches it): nothing is playing */
        } else if (!strcmp(cmd, "SET")) {
            char *line;
            send_line("202 OK RECEIVING MESSAGE");
            while ((line = readline_sd()) != NULL) {
                char *eq;
                if (!strcmp(line, ".")) { free(line); break; }
                eq = strchr(line, '=');
                if (eq) { *eq = 0; set_param(line, eq + 1); }
                free(line);
            }
            send_line("203 OK SETTINGS RECEIVED");
        } else if (!strcmp(cmd, "AUDIO") || !strcmp(cmd, "LOGLEVEL") || !strcmp(cmd, "DEBUG")) {
            char *line;
            send_line("202 OK RECEIVING MESSAGE");
            while ((line = readline_sd()) != NULL) {
                int end = !strcmp(line, ".");
                free(line);
                if (end) break;
            }
            send_line("203 OK SETTINGS RECEIVED");
        } else if (!strcmp(cmd, "LIST VOICES")) {
            int i;
            for (i = 0; i < N_VOICES; i++)
                if (have(voice_fw[i][0]) && have(voice_fw[i][1]))
                    printf("200-%s\t%s\tMALE1\n", voice_name[i], voice_lang[i]);
            send_line("249 OK VOICES LISTED");
        } else if (!strcmp(cmd, "QUIT")) {
            free(cmd);
            break;
        } else {
            send_line("300 ERR UNKNOWN COMMAND");
        }
        free(cmd);
    }
    {
        int i;
        for (i = 0; i < N_VOICES; i++) blv_destroy(voices[i]);
    }
    return 0;
}

/* test_android_native.c -- the app's native part without a phone: the front end the JNI bridge calls (ssa_engine.c,
 * ssa_map.c), linked with the same chip, board, host and voice, driven the way SsiTtsService drives it -- an
 * Android request's rate and pitch percentages on top of the app's sliders, the audio pulled in chunks, a stop
 * between chunks and a cancel after it -- and each case's PCM hashed.
 *
 *     test_android_native <data folder> [<aicom folder>]
 *                                           one line per case: "<name> <samples> <fnv-1a 64 of the PCM bytes>";
 *                                           with the Aicom folder (u2/u3/u4.BIN), the Accent SA's cases first
 *     test_android_native --texts           stdin's lines as the Accent SA's front end sends them
 *
 * test_android_native.py compares the hashes with bl_voice driven directly, the way the speech-dispatcher module
 * maps SSIP (the reference), and the Accent SA's with the NVDA Accent driver itself (accent_reference.py), on the
 * desktop and over adb.  SSI263_ANDROID_TEST_BREAK in the environment puts a bug back, the controls: 1 breaks the rate
 * mapping (ssa_map.h), so the "fast" cases must differ; accent-pitch, accent-glide and accent-reuse are
 * ssa_engine.h's ssa_accent_break 1, 2 and 3.
 *
 * Built by the desktop compiler (test_android_native.py) and by build_android.sh --test (static, for a device).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssa_engine.h"
#include "ssa_map.h"
#include "accentsa/as_voice.h"

static const char *HELLO = "Hello there. This is the Braille Lite, speaking on a phone.";
static const char *LONG = "This is a long message for the stop test, with a comma or two, that keeps going well "
                          "past the moment the harness says stop. It has a second sentence as well.";

typedef struct {
    unsigned long long h;
    long samples;
} digest;

static void add(digest *d, const short *pcm, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned v = (unsigned short)pcm[i];
        d->h = (d->h ^ (v & 0xFF)) * 1099511628211ULL;        /* little-endian bytes, as the app hands them over */
        d->h = (d->h ^ (v >> 8)) * 1099511628211ULL;
    }
    d->samples += n;
}

static void report(const char *name, const digest *d)
{
    printf("%s %ld %016llx\n", name, d->samples, d->h);
}

/* One utterance through the app's path, pulled `chunk` samples at a time.  stop_after > 0: stop once the unit has
   rendered that many non-empty blocks, check the stop lands before another block, and cancel.  -1 on a failure. */
static int speak(ssa_engine *e, int voice, const char *text, const ssa_settings *s, int req_rate, int req_pitch,
                 int chunk, int stop_after, digest *d)
{
    short buf[8192];
    int r = ssa_start(e, voice, text, s, req_rate, req_pitch), n;
    d->h = 1469598103934665603ULL;
    d->samples = 0;
    if (r < 0) { fprintf(stderr, "ssa_start failed on %s\n", text); return -1; }
    if (r == 1) return 0;
    while ((n = ssa_pull(e, buf, chunk)) > 0) {
        add(d, buf, n);
        if (stop_after > 0 && ssa_blocks(e) == stop_after) {
            int blocks = ssa_blocks(e);
            ssa_stop(e);
            /* what is left of the current block may still come; no new block may be rendered */
            while ((n = ssa_pull(e, buf, chunk)) > 0) add(d, buf, n);
            if (n != -2 || ssa_blocks(e) != blocks) {
                fprintf(stderr, "the stop did not land: pull %d, blocks %d -> %d\n", n, blocks, ssa_blocks(e));
                return -1;
            }
            ssa_cancel(e);
            return 0;
        }
    }
    if (n < 0) { fprintf(stderr, "pull returned %d\n", n); return -1; }
    return 0;
}

/* ---- the Accent SA (the built-in voice): the ROMs in memory, as the app hands them over ------------------------- */

static unsigned char *slurp(const char *dir, const char *name, size_t *n)
{
    char p[1200];
    FILE *f;
    unsigned char *d;
    long len;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (!(f = fopen(p, "rb"))) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (unsigned char *)malloc(len > 0 ? (size_t)len : 1);
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) { free(d); d = NULL; }
    fclose(f);
    *n = (size_t)len;
    return d;
}

static const char *HELLO_A = "Hello there. This is the Accent SA, speaking on a phone.";
static const char *LONG_A = "This is a long message for the stop test, with a comma or two, that keeps going well past "
                            "the moment the harness says stop. It has a second sentence as well.";
static const char *NUMBERS_A = "You owe $1234.50 for 3 items: 100 percent, the 21st of 1,000,000 and -2.5 degrees.";
static const char *TEXT_A = "It\xe2\x80\x99s \xe2\x80\x9cquoted\xe2\x80\x9d \xe2\x80\x93 see ~/code\xe2\x80\xa6 "
                            "\xc2\xa3" "2.63, 5 \xe2\x82\xac and caf\xc3\xa9\tend \xf0\x9f\x8e\x89";
static const char *CAP_A = "B";

/* test_android_native.py's ACCENT_CASES, in the same order: name, text, the app's rate and pitch sliders, the
   request's rate and pitch percentages, the engine volume, inflection, sample rate, pull size, blocks before a stop */
typedef struct {
    const char *name, *text;
    int rate, pitch, req_rate, req_pitch, volume, inflection, sample_rate, chunk, stop;
} accent_case;

static const accent_case *accent_cases(int *n)
{
    static accent_case c[16];
    int k = 0;
#define A(nm, tx, r, p, rr, rp, v, inf, sr, ch, st) \
    do { accent_case x = {nm, tx, r, p, rr, rp, v, inf, sr, ch, st}; c[k++] = x; } while (0)
    A("a-default", HELLO_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 0);
    A("a-default-97", HELLO_A, 50, 50, 100, 100, 100, 1, 22050, 97, 0);
    A("a-fast", HELLO_A, 50, 50, 200, 100, 100, 1, 22050, 4096, 0);
    A("a-slow-low", HELLO_A, 30, 50, 100, 50, 100, 1, 22050, 4096, 0);
    A("a-sliders", HELLO_A, 70, 80, 100, 100, 100, 1, 22050, 4096, 0);
    A("a-numbers", NUMBERS_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 0);
    A("a-text", TEXT_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 0);
    A("a-pitch-100", CAP_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 0);
    A("a-pitch-150", CAP_A, 50, 50, 100, 150, 100, 1, 22050, 4096, 0);
    A("a-pitch-75", CAP_A, 50, 50, 100, 75, 100, 1, 22050, 4096, 0);
    A("a-pitch-100-again", CAP_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 0);
    A("a-stopped", LONG_A, 50, 50, 100, 100, 100, 1, 22050, 4096, 5);
    A("a-after-stop", "Next message.", 50, 50, 100, 100, 100, 1, 22050, 4096, 0);
    A("a-volume-150", HELLO_A, 50, 50, 100, 100, 150, 1, 22050, 4096, 0);
    A("a-monotone", HELLO_A, 50, 50, 100, 100, 100, 0, 22050, 4096, 0);
    A("a-11k", HELLO_A, 50, 50, 100, 100, 100, 1, 11025, 4096, 0);
#undef A
    *n = k;
    return c;
}

static int accent(const char *aicom)
{
    size_t n2, n3, n4;
    unsigned char *u2 = slurp(aicom, "u2.BIN", &n2), *u3 = slurp(aicom, "u3.BIN", &n3), *u4 = slurp(aicom, "u4.BIN", &n4);
    ssa_engine *e = ssa_new(".");
    const accent_case *c;
    char err[256];
    digest d;
    int n, i, rc = 0;
    if (!u2 || !u3 || !u4 || !e || !ssa_set_accent_roms(e, u2, n2, u3, n3, u4, n4)) {
        fprintf(stderr, "the Accent SA's ROMs in %s: missing or the wrong size\n", aicom);
        return 1;
    }
    free(u2); free(u3); free(u4);                      /* the engine keeps its own copy */
    if (!ssa_has_voice(e, SSA_ACCENT_SA)) { fprintf(stderr, "no Accent SA\n"); return 1; }
    c = accent_cases(&n);
    for (i = 0; i < n && !rc; i++) {
        ssa_settings s = {c[i].rate, c[i].pitch, 7, c[i].volume, 1};
        ssa_configure(e, c[i].sample_rate, c[i].inflection, 0);
        if (ssa_load(e, SSA_ACCENT_SA, err, sizeof err) != 0) { fprintf(stderr, "boot: %s\n", err); rc = 1; break; }
        if (speak(e, SSA_ACCENT_SA, c[i].text, &s, c[i].req_rate, c[i].req_pitch, c[i].chunk, c[i].stop, &d))
            rc = 1;
        else
            report(c[i].name, &d);
    }
    ssa_free(e);
    return rc;
}

/* --texts: each line of stdin (a text's UTF-8, in hex) as the Accent SA's front end would send it, hex: "text <line>
   <hex>" */
static int texts(void)
{
    char line[8192], text[4096], out[65536];
    int k = 0;
    while (fgets(line, sizeof line, stdin)) {
        int n, i;
        unsigned b;
        line[strcspn(line, "\r\n")] = 0;
        for (n = 0; line[2 * n] && line[2 * n + 1] && n < (int)sizeof text - 1; n++) {
            sscanf(line + 2 * n, "%2x", &b);
            text[n] = (char)b;
        }
        text[n] = 0;
        n = asv_say_bytes(text, 1, out, (int)sizeof out);
        if (n < 0 || n >= (int)sizeof out) { fprintf(stderr, "say_bytes failed on line %d\n", k); return 1; }
        printf("text %d ", k++);
        for (i = 0; i < n; i++) printf("%02x", (unsigned char)out[i]);
        printf("\n");
    }
    return 0;
}

/* --level <data folder> <aicom folder> <voice> <engine volume>: each line of stdin (hex UTF-8) spoken through the app's
   path at the app's default settings and that volume: "level <line> <peak> <clipped> <sum of squares> <samples over
   -40 dBFS>" (test_volume_headroom.py) */
static int level(const char *data, const char *aicom, int voice, int volume)
{
    size_t n2, n3, n4;
    unsigned char *u2 = slurp(aicom, "u2.BIN", &n2), *u3 = slurp(aicom, "u3.BIN", &n3), *u4 = slurp(aicom, "u4.BIN", &n4);
    ssa_engine *e = ssa_new(data);
    ssa_settings s = {50, 50, 7, volume, 1};
    char line[8192], text[4096];
    short buf[4096];
    int k = 0;
    if (!e || !u2 || !u3 || !u4 || !ssa_set_accent_roms(e, u2, n2, u3, n3, u4, n4) || !ssa_has_voice(e, voice)) {
        fprintf(stderr, "voice %d cannot speak\n", voice);
        return 1;
    }
    while (fgets(line, sizeof line, stdin)) {
        int n, i, peak = 0;
        long clipped = 0, loud = 0;
        double sq = 0;
        unsigned b;
        line[strcspn(line, "\r\n")] = 0;
        for (n = 0; line[2 * n] && line[2 * n + 1] && n < (int)sizeof text - 1; n++) {
            sscanf(line + 2 * n, "%2x", &b);
            text[n] = (char)b;
        }
        text[n] = 0;
        if (ssa_start(e, voice, text, &s, 100, 100) < 0) { fprintf(stderr, "start failed\n"); return 1; }
        while ((n = ssa_pull(e, buf, 4096)) > 0)
            for (i = 0; i < n; i++) {
                int v = buf[i] < 0 ? -buf[i] : buf[i];
                if (v > peak) peak = v;
                clipped += v >= 32767;
                if (v > 328) { sq += (double)v * v; loud++; }       /* over -40 dBFS: speech, not the pauses */
            }
        printf("level %d %d %ld %.0f %ld\n", k++, peak, clipped, sq, loud);
    }
    ssa_free(e);
    free(u2); free(u3); free(u4);
    return 0;
}

int main(int argc, char **argv)
{
    ssa_engine *e;
    ssa_settings s = {50, 50, 7, 100, 1}, slow = {30, 50, 7, 100, 1};
    digest d;
    char err[256];
    const char *brk = getenv("SSI263_ANDROID_TEST_BREAK");
    if (argc >= 2 && !strcmp(argv[1], "--texts")) return texts();
    if (argc >= 6 && !strcmp(argv[1], "--level")) return level(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
    if (argc < 2) { fprintf(stderr, "usage: test_android_native <data folder> [<aicom folder>] | --texts\n"); return 2; }
    ssa_map_break = brk && !strcmp(brk, "1");
    ssa_accent_break = !brk ? 0 : !strcmp(brk, "accent-pitch") ? 1 : !strcmp(brk, "accent-glide") ? 2
                     : !strcmp(brk, "accent-reuse") ? 3 : 0;
    if (argc >= 3 && accent(argv[2])) return 1;
    e = ssa_new(argv[1]);
    if (!e || !ssa_has_voice(e, SSA_ENGLISH)) { fprintf(stderr, "no English unit in %s\n", argv[1]); return 2; }
    ssa_configure(e, 22050, 1, 0);
    if (ssa_load(e, SSA_ENGLISH, err, sizeof err) != 0) { fprintf(stderr, "boot: %s\n", err); return 2; }

    /* the mapping itself: Android 100% is the slider; a doubling is SSIP 50 */
    if (ssa_rate(50, 100) != 50 || ssa_pitch(50, 50) != 25 || ssa_pitch(50, 400) != 100
            || ssa_ssip_from_percent(200) != 50 || ssa_to100(-100) != 0) {
        fprintf(stderr, "the mapping is off\n");
        return 1;
    }

    if (speak(e, SSA_ENGLISH, HELLO, &s, 100, 100, 4096, 0, &d)) return 1;
    report("default", &d);
    if (speak(e, SSA_ENGLISH, HELLO, &s, 100, 100, 97, 0, &d)) return 1;
    report("default-97", &d);
    if (speak(e, SSA_ENGLISH, HELLO, &s, 200, 100, 4096, 0, &d)) return 1;
    report("fast", &d);
    if (speak(e, SSA_ENGLISH, HELLO, &slow, 100, 50, 4096, 0, &d)) return 1;
    report("slow-low", &d);
    if (speak(e, SSA_ENGLISH, LONG, &s, 100, 100, 4096, 5, &d)) return 1;
    report("stopped", &d);
    if (speak(e, SSA_ENGLISH, "Next message.", &s, 100, 100, 4096, 0, &d)) return 1;
    report("after-stop", &d);
    if (ssa_has_voice(e, SSA_SPANISH)) {
        if (speak(e, SSA_SPANISH, "Ma\xc3\xb1" "ana, \xc2\xbfqu\xc3\xa9 tal? \xc3\x89l est\xc3\xa1 aqu\xc3\xad.", &s,
                  100, 100, 4096, 0, &d))
            return 1;
        report("spanish", &d);
    }
    ssa_free(e);

    /* the import's check (ssa_probe): a unit of its own speaks the first case again, as the first case did */
    {
        unsigned long long h;
        long n = ssa_probe(argv[1], SSA_ENGLISH, HELLO, &h, err, sizeof err);
        if (n < 0) { fprintf(stderr, "probe: %s\n", err); return 1; }
        printf("probe %ld %016llx\n", n, h);
    }
    return 0;
}

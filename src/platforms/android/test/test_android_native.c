/* test_android_native.c -- the app's native part without a phone: the front end the JNI bridge calls (ssa_engine.c,
 * ssa_map.c), linked with the same chip, board, host and voice, driven the way SsiTtsService drives it -- an
 * Android request's rate and pitch percentages on top of the app's sliders, the audio pulled in chunks, a stop
 * between chunks and a cancel after it -- and each case's PCM hashed.
 *
 *     test_android_native <data folder>     one line per case: "<name> <samples> <fnv-1a 64 of the PCM bytes>"
 *
 * test_android_native.py compares the hashes with bl_voice driven directly, the way the speech-dispatcher module
 * maps SSIP (the reference), on the desktop and over adb.  SSI263_ANDROID_TEST_BREAK=1 in the environment breaks the
 * rate mapping (ssa_map.h): the "fast" case must then differ -- the control.
 *
 * Built by the desktop compiler (test_android_native.py) and by build_android.sh --test (static, for a device).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssa_engine.h"
#include "ssa_map.h"

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

int main(int argc, char **argv)
{
    ssa_engine *e;
    ssa_settings s = {50, 50, 7, 100, 1}, slow = {30, 50, 7, 100, 1};
    digest d;
    char err[256];
    const char *brk = getenv("SSI263_ANDROID_TEST_BREAK");
    if (argc < 2) { fprintf(stderr, "usage: test_android_native <data folder>\n"); return 2; }
    ssa_map_break = brk && !strcmp(brk, "1");
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
    return 0;
}

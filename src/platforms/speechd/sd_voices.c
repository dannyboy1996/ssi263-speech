/* sd_voices.c -- see sd_voices.h.  One table: each voice's name, language, files and engine; each engine's adapter
 * onto the module's calls (create, set, speak, render, cancel, destroy).  The settings each engine takes are its
 * NVDA add-on's: the Braille Lite's (bl_voice.h's blv_set), the Accents' (as_voice.h, am_voice.h), the Speak-Out's
 * (so_voice.h).
 *
 * Test hooks, each a must-fail control of test_sd_ssi263.py: SD_SSI263_TEST_IGNORE_RUN_AHEAD=1 drops SSI263RunAhead
 * on its way to the Braille Lite, SD_SSI263_TEST_IGNORE_ACCENT_INFLECTION=1 drops SSI263AccentInflection on its way
 * to the Accents (they get the default, full intonation). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../csrc/blazie/bl_voice.h"
#include "../../csrc/accentsa/as_voice.h"
#ifdef SD_ACCENT_MINI
#include "../../csrc/accentmini/am_voice.h"
#endif
#ifdef SD_SPEAKOUT
#include "../../csrc/speakout/so_voice.h"
#endif
#include "sd_voices.h"

void sdv_defaults(sd_settings *s)
{
    memset(s, 0, sizeof *s);
    s->sample_rate = 22050;
    s->inflection = 1;
    s->tone = 7;
    s->short_pauses = 1;
    s->accent_inflection = 100;
    s->accent_numbers = 1;
    s->accent_voice = 5;
    s->speakout_tone = 8;
    s->speakout_join = 1;
    s->speakout_short_pauses = 1;
}

static void path(const sd_settings *s, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", s->datadir, name);
}

/* ---- the Braille Lite 2000 (bl_voice.h): arg 0 English, 1 Spanish ------------------------------------------------ */
static void *ad_bl_create(int arg, const char *const *files, const sd_settings *s, char *err, int errlen)
{
    char fw[1200], st[1200];
    path(s, files[0], fw, sizeof fw);
    path(s, files[1], st, sizeof st);
    return blv_create(fw, st, arg == 1 ? BLV_CP850 : BLV_LATIN1, s->sample_rate, s->inflection, s->whine, err, errlen);
}

static void ad_bl_set(void *u, const sd_settings *s, int rate, int pitch, int volume)
{
    blv_set((bl_voice *)u, rate, pitch, s->tone, volume, s->short_pauses);
    blv_set_run_ahead((bl_voice *)u, s->run_ahead && !getenv("SD_SSI263_TEST_IGNORE_RUN_AHEAD"));
}

static int ad_bl_speak(void *u, const char *utf8)
{
    blv_speak((bl_voice *)u, utf8);
    return 1;                          /* rendered until done whatever it took (the module before 0.7.1) */
}

static int ad_bl_render(void *u, const short **pcm, int *done) { return blv_render((bl_voice *)u, pcm, done); }
static void ad_bl_cancel(void *u) { blv_cancel((bl_voice *)u); }
static void ad_bl_destroy(void *u) { blv_destroy((bl_voice *)u); }

/* ---- the Accents' shared setting ----------------------------------------------------------------------------- */
static int accent_inflection(const sd_settings *s)
{
    return getenv("SD_SSI263_TEST_IGNORE_ACCENT_INFLECTION") ? 100 : s->accent_inflection;
}

/* ---- the Accent SA (as_voice.h): Aicom's u2, u3, u4 ----------------------------------------------------------- */
static unsigned char *read_file(const char *p, size_t want, char *err, int errlen)
{
    FILE *f = fopen(p, "rb");
    unsigned char *b;
    size_t n;
    if (!f) { snprintf(err, (size_t)errlen, "cannot open %s", p); return NULL; }
    b = (unsigned char *)malloc(want + 1);
    n = b ? fread(b, 1, want + 1, f) : 0;
    fclose(f);
    if (!b || n != want) {
        snprintf(err, (size_t)errlen, "%s: not %lu bytes", p, (unsigned long)want);
        free(b);
        return NULL;
    }
    return b;
}

static void *ad_as_create(int arg, const char *const *files, const sd_settings *s, char *err, int errlen)
{
    static const size_t size[3] = {0x10000, 0x8000, 0x8000};
    unsigned char *rom[3] = {NULL, NULL, NULL};
    as_voice *v = NULL;
    int i;
    (void)arg;
    for (i = 0; i < 3; i++) {
        char p[1200];
        path(s, files[i], p, sizeof p);
        if (!(rom[i] = read_file(p, size[i], err, errlen))) break;
    }
    if (i == 3)
        v = asv_create(rom[0], size[0], rom[1], size[1], rom[2], size[2], (double)s->sample_rate, err, errlen);
    for (i = 0; i < 3; i++) free(rom[i]);
    return v;
}

static void ad_as_set(void *u, const sd_settings *s, int rate, int pitch, int volume)
{
    asv_set((as_voice *)u, rate, pitch, accent_inflection(s), volume, s->accent_numbers);
}

static int ad_as_speak(void *u, const char *utf8) { return asv_speak((as_voice *)u, utf8, 0); }
static int ad_as_render(void *u, const short **pcm, int *done) { return asv_render((as_voice *)u, pcm, done); }
static void ad_as_cancel(void *u) { asv_cancel((as_voice *)u); }
static void ad_as_destroy(void *u) { asv_destroy((as_voice *)u); }

#ifdef SD_ACCENT_MINI
/* ---- the Accent-mini (am_voice.h): Aicom's SPKEMS.DVC on an emulated PC ---------------------------------------- */
static void *ad_am_create(int arg, const char *const *files, const sd_settings *s, char *err, int errlen)
{
    char p[1200];
    (void)arg;
    path(s, files[0], p, sizeof p);
    return amv_create(p, (double)s->sample_rate, err, errlen);
}

static void ad_am_set(void *u, const sd_settings *s, int rate, int pitch, int volume)
{
    amv_set((am_voice *)u, rate, pitch, accent_inflection(s), volume, s->accent_numbers, s->accent_voice);
}

static int ad_am_speak(void *u, const char *utf8) { return amv_speak((am_voice *)u, utf8, 0); }
static int ad_am_render(void *u, const short **pcm, int *done) { return amv_render((am_voice *)u, pcm, done); }
static void ad_am_cancel(void *u) { amv_cancel((am_voice *)u); }
static void ad_am_destroy(void *u) { amv_destroy((am_voice *)u); }
#endif

#ifdef SD_SPEAKOUT
/* ---- the Speak-Out (so_voice.h): GW Micro's SPEAKOUT.HEX on an emulated V40 ----------------------------------- */
static void *ad_so_create(int arg, const char *const *files, const sd_settings *s, char *err, int errlen)
{
    char p[1200];
    (void)arg;
    path(s, files[0], p, sizeof p);
    return sov_create(p, (double)s->sample_rate, err, errlen);
}

static void ad_so_set(void *u, const sd_settings *s, int rate, int pitch, int volume)
{
    sov_set((so_voice *)u, rate, pitch, s->speakout_tone, volume, s->speakout_join, s->speakout_short_pauses);
}

static int ad_so_speak(void *u, const char *utf8) { return sov_speak((so_voice *)u, utf8, 0); }
static int ad_so_render(void *u, const short **pcm, int *done) { return sov_render((so_voice *)u, pcm, done); }
static void ad_so_cancel(void *u) { sov_cancel((so_voice *)u); }
static void ad_so_destroy(void *u) { sov_destroy((so_voice *)u); }
#endif

/* ---- the table --------------------------------------------------------------------------------------------------- */
typedef struct {
    const char *name, *language, *engine;
    const char *files[4];              /* in the data folder, NULL-terminated */
    int arg;
    void *(*create)(int arg, const char *const *files, const sd_settings *s, char *err, int errlen);
    void (*set)(void *u, const sd_settings *s, int rate, int pitch, int volume);
    int (*speak)(void *u, const char *utf8);
    int (*render)(void *u, const short **pcm, int *done);
    void (*cancel)(void *u);
    void (*destroy)(void *u);
} voice_def;

static const voice_def VOICES[] = {
    {"Braille Lite 2000", "en-US", "braille-lite", {"BL2ENG.BNS", "bl2_2003_warm.state", NULL}, 0,
     ad_bl_create, ad_bl_set, ad_bl_speak, ad_bl_render, ad_bl_cancel, ad_bl_destroy},
    {"Braille Lite 2000 (espa\xc3\xb1ol)", "es-ES", "braille-lite", {"BL2SPA.BNS", "bl2spa_fresh.state", NULL}, 1,
     ad_bl_create, ad_bl_set, ad_bl_speak, ad_bl_render, ad_bl_cancel, ad_bl_destroy},
    {"Accent SA", "en-US", "accent-sa",
     {"aicom-accent-sa/u2.BIN", "aicom-accent-sa/u3.BIN", "aicom-accent-sa/u4.BIN", NULL}, 0,
     ad_as_create, ad_as_set, ad_as_speak, ad_as_render, ad_as_cancel, ad_as_destroy},
#ifdef SD_ACCENT_MINI
    {"Accent-mini", "en-US", "accent-mini", {"aicom-accent-mini/SPKEMS.DVC", NULL}, 0,
     ad_am_create, ad_am_set, ad_am_speak, ad_am_render, ad_am_cancel, ad_am_destroy},
#endif
#ifdef SD_SPEAKOUT
    {"Speak-Out", "en-US", "speakout", {"gw-micro-speakout/SPEAKOUT.HEX", NULL}, 0,
     ad_so_create, ad_so_set, ad_so_speak, ad_so_render, ad_so_cancel, ad_so_destroy},
#endif
};
#define N_VOICES ((int)(sizeof VOICES / sizeof VOICES[0]))
static void *units[N_VOICES];

int sdv_count(void) { return N_VOICES; }
const char *sdv_name(int i) { return VOICES[i].name; }
const char *sdv_language(int i) { return VOICES[i].language; }
const char *sdv_engine(int i) { return VOICES[i].engine; }

int sdv_find(const char *name)
{
    int i;
    for (i = 0; i < N_VOICES; i++)
        if (!strcmp(name, VOICES[i].name)) return i;
    return -1;
}

int sdv_available(int i, const sd_settings *s)
{
    const char *const *f;
    for (f = VOICES[i].files; *f; f++) {
        char p[1200];
        path(s, *f, p, sizeof p);
        if (access(p, R_OK) != 0) return 0;
    }
    return 1;
}

int sdv_load(int i, const sd_settings *s, char *err, int errlen)
{
    if (!units[i])
        units[i] = VOICES[i].create(VOICES[i].arg, VOICES[i].files, s, err, errlen);
    return units[i] ? 0 : -1;
}

int sdv_speak(int i, const sd_settings *s, int rate, int pitch, int volume, const char *utf8, char *err, int errlen)
{
    if (sdv_load(i, s, err, errlen) != 0) return -1;
    VOICES[i].set(units[i], s, rate, pitch, volume);
    return VOICES[i].speak(units[i], utf8) > 0;
}

int sdv_render(int i, const short **pcm, int *done)
{
    if (!units[i]) { *done = 1; return 0; }
    return VOICES[i].render(units[i], pcm, done);
}

void sdv_cancel(int i)
{
    if (units[i]) VOICES[i].cancel(units[i]);
}

void sdv_destroy_all(void)
{
    int i;
    for (i = 0; i < N_VOICES; i++) {
        if (units[i]) VOICES[i].destroy(units[i]);
        units[i] = NULL;
    }
}

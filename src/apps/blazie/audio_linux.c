/* audio_linux.c -- see audio_linux.h. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "audio_linux.h"
#include "audio_pace.h"

#ifndef BLAZIE_AUDIO_PULSE
#include <alsa/asoundlib.h>
#else
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

struct audio_out {
#ifndef BLAZIE_AUDIO_PULSE
    snd_pcm_t *pcm;
#else
    pa_simple *s;
#endif
    int rate, block, block_ms;
    long ring;                       /* the device's buffer as opened (frames) */
    audio_pace pace;
    unsigned long long written;      /* frames the device has taken since it opened */
    /* BLAZIE_EMU_AUDIO_LOG and _STALL (audio_linux.h) */
    FILE *log;
    int stall, underruns, saves;
    long ahead_n;
    double ahead_sum, ahead_max, t_open, save_max;
    double lag0, lag;                /* the wall clock ahead of the played position: at the start, now */
    int lag_set;
};

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static void sleep_ms(double ms)
{
    struct timespec t;
    if (ms <= 0)
        return;
    t.tv_sec = (time_t)(ms / 1000.0);
    t.tv_nsec = (long)((ms - (double)t.tv_sec * 1000.0) * 1e6);
    nanosleep(&t, NULL);
}

static audio_out *new_out(int rate, int block_ms, int mode)
{
    audio_out *a = (audio_out *)calloc(1, sizeof(audio_out));
    if (!a)
        return NULL;
    a->rate = rate;
    a->block_ms = block_ms;
    a->block = rate * block_ms / 1000;
    a->ring = ap_ring_frames(a->block, block_ms);
    ap_init(&a->pace, mode, block_ms);
    return a;
}

/* the test's log and hold-ups, once the device is open */
static void test_hooks(audio_out *a, const char *device)
{
    const char *env;
    if ((env = getenv("BLAZIE_EMU_AUDIO_STALL")) != NULL)
        a->stall = atoi(env);
    if ((env = getenv("BLAZIE_EMU_AUDIO_LOG")) != NULL && *env && (a->log = fopen(env, "a")) != NULL)
        fprintf(a->log, "open: %s %s, %d Hz, blocks of %d ms, the device's buffer %ld frames (%.0f ms); sound buffer "
                "%s (%d ms)%s\n", audio_backend(), device ? device : "default", a->rate, a->block_ms, a->ring,
                (double)a->ring * 1000.0 / a->rate, ap_mode_name(a->pace.mode), ap_target(&a->pace) * a->block_ms,
                a->stall > 0 ? "; held up before 5% of the writes" : "");
    a->t_open = now_ms();
}

#ifndef BLAZIE_AUDIO_PULSE
/* ---- ALSA ---------------------------------------------------------------------------------------------------- */
const char *audio_backend(void)
{
    return "ALSA";
}

audio_out *audio_open(const char *device, int rate, int block_ms, int mode, char *err, int errlen)
{
    audio_out *a = new_out(rate, block_ms, mode);
    snd_pcm_hw_params_t *hw = NULL;
    snd_pcm_sw_params_t *sw = NULL;
    snd_pcm_uframes_t period, buffer;
    unsigned int r = (unsigned int)rate;
    long start;
    int e;
    const char *step = "open";
    if (!a) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    period = (snd_pcm_uframes_t)a->block;
    buffer = (snd_pcm_uframes_t)a->ring;
    if (!device || !*device)
        device = "default";
    if ((e = snd_pcm_open(&a->pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
        goto fail;
    step = "set up";
    if ((e = snd_pcm_hw_params_malloc(&hw)) < 0 || (e = snd_pcm_hw_params_any(a->pcm, hw)) < 0
            || (e = snd_pcm_hw_params_set_rate_resample(a->pcm, hw, 1)) < 0
            || (e = snd_pcm_hw_params_set_access(a->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0
            || (e = snd_pcm_hw_params_set_format(a->pcm, hw, SND_PCM_FORMAT_S16)) < 0
            || (e = snd_pcm_hw_params_set_channels(a->pcm, hw, 1)) < 0
            || (e = snd_pcm_hw_params_set_rate_near(a->pcm, hw, &r, NULL)) < 0)
        goto fail;
    if (r != (unsigned int)rate) {
        snprintf(err, errlen, "the sound device %s cannot play at %d Hz (nearest %u)", device, rate, r);
        snd_pcm_hw_params_free(hw);
        snd_pcm_close(a->pcm);
        free(a);
        return NULL;
    }
    /* periods of one block; a buffer for the longest queue, of which the sound thread keeps only the chosen queue
       filled (audio_turn) */
    snd_pcm_hw_params_set_period_size_near(a->pcm, hw, &period, NULL);
    snd_pcm_hw_params_set_buffer_size_near(a->pcm, hw, &buffer);
    if ((e = snd_pcm_hw_params(a->pcm, hw)) < 0)
        goto fail;
    if (snd_pcm_hw_params_get_buffer_size(hw, &buffer) == 0 && buffer > 0)
        a->ring = (long)buffer;      /* as the device gave it: a shorter one caps the queue (the write waits) */
    snd_pcm_hw_params_free(hw);
    hw = NULL;
    step = "start";
    if ((e = snd_pcm_sw_params_malloc(&sw)) < 0 || (e = snd_pcm_sw_params_current(a->pcm, sw)) < 0)
        goto fail;
    /* playing once the shortest queue is in (so no choice of queue leaves it waiting to start), at the start and
       after an underrun */
    start = ap_start_frames(a->block, block_ms);
    snd_pcm_sw_params_set_start_threshold(a->pcm, sw, (snd_pcm_uframes_t)(start < a->ring ? start : a->ring));
    snd_pcm_sw_params_set_avail_min(a->pcm, sw, (snd_pcm_uframes_t)a->block);
    if ((e = snd_pcm_sw_params(a->pcm, sw)) < 0)
        goto fail;
    snd_pcm_sw_params_free(sw);
    test_hooks(a, device);
    return a;
fail:
    snprintf(err, errlen, "could not %s the sound device %s: %s", step, device, snd_strerror(e));
    if (hw)
        snd_pcm_hw_params_free(hw);
    if (sw)
        snd_pcm_sw_params_free(sw);
    if (a->pcm)
        snd_pcm_close(a->pcm);
    free(a);
    return NULL;
}

/* frames in ALSA's buffer still to go out, and the delay until a frame written now is heard; 0: the device cannot
   say (an underrun: it has stopped, nothing is queued) */
static int card_queue(audio_out *a, long *queued, long *delay)
{
    snd_pcm_sframes_t avail = 0, d = 0;
    if (snd_pcm_avail_delay(a->pcm, &avail, &d) < 0)
        return 0;
    *queued = a->ring - (long)avail;
    if (*queued < 0)
        *queued = 0;
    *delay = (long)d;
    return 1;
}

static int card_write(audio_out *a, const short *pcm, int n)
{
    int tries = 0;
    while (n > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(a->pcm, pcm, (snd_pcm_uframes_t)n);
        if (w < 0) {
            if (w == -EPIPE)
                a->underruns++;
            if (++tries > 10 || snd_pcm_recover(a->pcm, (int)w, 1) < 0)   /* an underrun, a suspend: start again */
                return 0;
            continue;
        }
        pcm += w;
        n -= (int)w;
        a->written += (unsigned long long)w;
    }
    return 1;
}

static void card_close(audio_out *a)
{
    snd_pcm_drop(a->pcm);
    snd_pcm_close(a->pcm);
}

#else
/* ---- PulseAudio (or PipeWire's), its simple API --------------------------------------------------------------- */
const char *audio_backend(void)
{
    return "PulseAudio";
}

audio_out *audio_open(const char *device, int rate, int block_ms, int mode, char *err, int errlen)
{
    audio_out *a = new_out(rate, block_ms, mode);
    pa_sample_spec ss;
    pa_buffer_attr at;
    int e = 0;
    if (!a) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    ss.format = PA_SAMPLE_S16NE;
    ss.rate = (uint32_t)rate;
    ss.channels = 1;
    at.maxlength = (uint32_t)-1;
    at.tlength = (uint32_t)(a->ring * 2);        /* room for the longest queue; the sound thread keeps the chosen one */
    at.prebuf = (uint32_t)(ap_start_frames(a->block, block_ms) * 2);
    at.minreq = (uint32_t)(a->block * 2);
    at.fragsize = (uint32_t)-1;
    if (device && (!*device || !strcmp(device, "default")))
        device = NULL;                          /* the server's default sink (ALSA's name for it is not a sink's) */
    a->s = pa_simple_new(NULL, "Blazie emulator", PA_STREAM_PLAYBACK, device, "unit", &ss, NULL, &at, &e);
    if (!a->s) {
        snprintf(err, errlen, "could not open the sound server: %s", pa_strerror(e));
        free(a);
        return NULL;
    }
    test_hooks(a, device);
    return a;
}

/* the simple API tells only the stream's whole latency: the queue is that, the server's own latency included */
static int card_queue(audio_out *a, long *queued, long *delay)
{
    int e = 0;
    pa_usec_t us = pa_simple_get_latency(a->s, &e);
    if (us == (pa_usec_t)-1)
        return 0;
    *delay = *queued = (long)((double)us * a->rate / 1e6);
    return 1;
}

static int card_write(audio_out *a, const short *pcm, int n)
{
    int e = 0;
    if (pa_simple_write(a->s, pcm, (size_t)n * 2, &e) < 0)
        return 0;
    a->written += (unsigned long long)n;
    return 1;
}

static void card_close(audio_out *a)
{
    pa_simple_flush(a->s, NULL);
    pa_simple_free(a->s);
}
#endif

/* ---- the queue (audio_pace.h), either backend ---------------------------------------------------------------- */
int audio_turn(audio_out *a, int mode, int save_pending)
{
    long queued = 0, delay = 0;
    int in_flight = 0;
    double t = now_ms();
    if (mode != a->pace.mode && mode >= 0 && mode < AP_N_MODES) {   /* Settings > Sound buffer changed */
        ap_set_mode(&a->pace, mode);
        if (a->log)
            fprintf(a->log, "%.1f sound buffer %s (%d ms)\n", t - a->t_open, ap_mode_name(mode),
                    ap_target(&a->pace) * a->block_ms);
    }
    if (card_queue(a, &queued, &delay)) {
        double played = ap_played_ms(a->written, delay, a->rate), gap = ap_observe(&a->pace, t, played);
        if (played > 0) {            /* the sound lost in all, gaps too short to be found included (the log) */
            a->lag = t - played;
            if (!a->lag_set) {
                a->lag0 = a->lag;
                a->lag_set = 1;
            }
        }
        in_flight = ap_blocks_queued(queued, a->block);
        if (gap > 0 && a->log)
            fprintf(a->log, "%.1f GAP %.1f ms: sound buffer now %d ms\n", t - a->t_open, gap,
                    ap_target(&a->pace) * a->block_ms);
    }                                /* else an underrun: the device stopped, nothing queued (the write recovers it) */
    if (ap_want(&a->pace, in_flight, save_pending) > 0)
        return AUDIO_RENDER;
    if (ap_save_now(&a->pace, in_flight, save_pending))
        return AUDIO_SAVE;
    sleep_ms(ap_wait_ms(&a->pace, queued, a->block, a->rate, save_pending));
    return AUDIO_WAIT;
}

int audio_buffer_ms(audio_out *a)
{
    return ap_target(&a->pace) * a->block_ms;
}

int audio_write(audio_out *a, const short *pcm, int n)
{
    if (a->stall > 0 && rand() % 20 == 0)         /* the test: the thread held up (a busy machine, a remote session) */
        sleep_ms((double)(rand() % a->stall));
    if (!card_write(a, pcm, n))
        return 0;
    if (a->log) {                                 /* how far ahead the sound is: a key's answer waits about this */
        long queued, delay;
        if (card_queue(a, &queued, &delay)) {
            double ms = (double)delay * 1000.0 / a->rate;
            a->ahead_sum += ms;
            a->ahead_n++;
            if (ms > a->ahead_max)
                a->ahead_max = ms;
        }
    }
    return 1;
}

void audio_saved(audio_out *a, double took_ms)
{
    a->saves++;
    if (took_ms > a->save_max)
        a->save_max = took_ms;
    if (a->log)
        fprintf(a->log, "%.1f saved in %.2f ms\n", now_ms() - a->t_open, took_ms);
}

void audio_close(audio_out *a)
{
    if (!a)
        return;
    if (a->log) {
        fprintf(a->log, "close: %.1f s, %d gaps, %.0f ms lost; sound buffer %d ms; %d underruns, the played position "
                "%.0f ms behind the clock in all; ahead %.1f ms on average, %.1f at most; %d saves (%.2f ms at most)\n",
                (now_ms() - a->t_open) / 1000.0, a->pace.gaps, a->pace.gap_ms, ap_target(&a->pace) * a->block_ms,
                a->underruns, a->lag > a->lag0 ? a->lag - a->lag0 : 0.0,
                a->ahead_n ? a->ahead_sum / (double)a->ahead_n : 0.0, a->ahead_max, a->saves, a->save_max);
        fclose(a->log);
    }
    card_close(a);
    free(a);
}

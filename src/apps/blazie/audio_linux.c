/* audio_linux.c -- see audio_linux.h. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_linux.h"

#ifndef BLAZIE_AUDIO_PULSE
/* ---- ALSA ---------------------------------------------------------------------------------------------------- */
#include <alsa/asoundlib.h>

struct audio_out {
    snd_pcm_t *pcm;
};

const char *audio_backend(void)
{
    return "ALSA";
}

audio_out *audio_open(const char *device, int rate, int block, int blocks, char *err, int errlen)
{
    audio_out *a = (audio_out *)calloc(1, sizeof(audio_out));
    snd_pcm_hw_params_t *hw = NULL;
    snd_pcm_sw_params_t *sw = NULL;
    snd_pcm_uframes_t period = (snd_pcm_uframes_t)block, buffer = (snd_pcm_uframes_t)block * (snd_pcm_uframes_t)blocks;
    unsigned int r = (unsigned int)rate;
    int e;
    const char *step = "open";
    if (!a) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
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
    /* small periods, a short buffer: a key reaches the unit at the next block, heard behind the blocks queued */
    snd_pcm_hw_params_set_period_size_near(a->pcm, hw, &period, NULL);
    snd_pcm_hw_params_set_buffer_size_near(a->pcm, hw, &buffer);
    if ((e = snd_pcm_hw_params(a->pcm, hw)) < 0)
        goto fail;
    snd_pcm_hw_params_free(hw);
    hw = NULL;
    step = "start";
    if ((e = snd_pcm_sw_params_malloc(&sw)) < 0 || (e = snd_pcm_sw_params_current(a->pcm, sw)) < 0)
        goto fail;
    snd_pcm_sw_params_set_start_threshold(a->pcm, sw, (snd_pcm_uframes_t)block);   /* play from the first block */
    snd_pcm_sw_params_set_avail_min(a->pcm, sw, (snd_pcm_uframes_t)block);
    if ((e = snd_pcm_sw_params(a->pcm, sw)) < 0)
        goto fail;
    snd_pcm_sw_params_free(sw);
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

int audio_write(audio_out *a, const short *pcm, int n)
{
    int tries = 0;
    while (n > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(a->pcm, pcm, (snd_pcm_uframes_t)n);
        if (w < 0) {
            if (++tries > 10 || snd_pcm_recover(a->pcm, (int)w, 1) < 0)   /* an underrun, a suspend: start again */
                return 0;
            continue;
        }
        pcm += w;
        n -= (int)w;
    }
    return 1;
}

void audio_close(audio_out *a)
{
    if (!a)
        return;
    snd_pcm_drop(a->pcm);
    snd_pcm_close(a->pcm);
    free(a);
}

#else
/* ---- PulseAudio (or PipeWire's), its simple API --------------------------------------------------------------- */
#include <pulse/error.h>
#include <pulse/simple.h>

struct audio_out {
    pa_simple *s;
};

const char *audio_backend(void)
{
    return "PulseAudio";
}

audio_out *audio_open(const char *device, int rate, int block, int blocks, char *err, int errlen)
{
    audio_out *a = (audio_out *)calloc(1, sizeof(audio_out));
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
    at.tlength = (uint32_t)(block * blocks * 2);
    at.prebuf = (uint32_t)-1;
    at.minreq = (uint32_t)(block * 2);
    at.fragsize = (uint32_t)-1;
    if (device && (!*device || !strcmp(device, "default")))
        device = NULL;                          /* the server's default sink (ALSA's name for it is not a sink's) */
    a->s = pa_simple_new(NULL, "Blazie emulator", PA_STREAM_PLAYBACK, device, "unit", &ss, NULL, &at, &e);
    if (!a->s) {
        snprintf(err, errlen, "could not open the sound server: %s", pa_strerror(e));
        free(a);
        return NULL;
    }
    return a;
}

int audio_write(audio_out *a, const short *pcm, int n)
{
    int e = 0;
    return pa_simple_write(a->s, pcm, (size_t)n * 2, &e) >= 0;
}

void audio_close(audio_out *a)
{
    if (!a)
        return;
    pa_simple_flush(a->s, NULL);
    pa_simple_free(a->s);
    free(a);
}
#endif

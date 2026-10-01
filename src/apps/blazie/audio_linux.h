/* audio_linux.h -- the sound card, for the Linux shells (main_linux.c, main_gtk.c): 16-bit mono at the unit's rate,
 * in blocks, with the sound buffer of the Windows app (audio_pace.h; Settings > Sound buffer, [sound] buffer=).
 *
 * The device's buffer is opened long enough for the longest queue (ap_ring_frames: 300 ms of 10 ms blocks) and the
 * sound thread keeps only the chosen queue in it: on each turn (audio_turn) it asks the device how much is still
 * queued and how far it has played, renders a block when the queue wants one, and otherwise sleeps until it will.
 * The card's clock still paces the unit (a block is rendered only as one drains), and the automatic queue grows in
 * place, without reopening the device, each time the card is found to have run dry -- told, as on Windows, from its
 * played position against the wall clock (ap_observe).  A write that finds the buffer full waits, as before.
 *
 * ALSA (libasound) when built with it, which needs nothing else on the machine (no desktop, no sound server: a
 * BTSpeak's or a Raspberry Pi's console); on a desktop ALSA's "default" device usually reaches PulseAudio or
 * PipeWire through their ALSA plugin.  The queue is what is in ALSA's buffer (snd_pcm_avail: the sound server's
 * own latency comes on top), the played position what was written less ALSA's delay (snd_pcm_delay: all of it).
 * Built without ALSA's headers, PulseAudio's simple API (BLAZIE_AUDIO_PULSE): it tells only the stream's whole
 * latency (pa_simple_get_latency), so there the queue counts the server's latency too.
 *
 * For a test of the queue on a real device: BLAZIE_EMU_AUDIO_STALL=ms holds the sound thread up to that long before
 * 5% of its writes (as main_win.c's), BLAZIE_EMU_AUDIO_LOG=file logs the gaps found, the saves and, at the end, the
 * gaps, the sound lost, the device's underruns and how far ahead the written sound was (a key's wait).
 */
#ifndef BLAZIE_AUDIO_LINUX_H
#define BLAZIE_AUDIO_LINUX_H

typedef struct audio_out audio_out;

/* device: ALSA's device name (NULL or "": "default"); block_ms: the block (5-20 ms); mode: the sound buffer
   (audio_pace.h AP_AUTO .. AP_LONG).  NULL on failure, the reason in err. */
audio_out *audio_open(const char *device, int rate, int block_ms, int mode, char *err, int errlen);

/* the sound thread's turn, before each block: */
enum {
    AUDIO_RENDER,        /* the queue wants a block: render it and audio_write it */
    AUDIO_SAVE,          /* a save is pending and the queue holds its render-ahead: save now */
    AUDIO_WAIT           /* slept (at most one block) until a block will be wanted: check for stopping, ask again */
};
/* mode: the sound buffer chosen now (a change takes effect here); save_pending: the minute's save is due */
int audio_turn(audio_out *a, int mode, int save_pending);
/* the queue now, in ms (the automatic one as grown) */
int audio_buffer_ms(audio_out *a);
/* the block (frames): returns when the card has room for it (an underrun is recovered); 0 on an error that lasts */
int audio_write(audio_out *a, const short *pcm, int n);
/* a save done on the sound thread (logged with how long it took) */
void audio_saved(audio_out *a, double took_ms);
void audio_close(audio_out *a);
/* "ALSA" or "PulseAudio" */
const char *audio_backend(void);

#endif

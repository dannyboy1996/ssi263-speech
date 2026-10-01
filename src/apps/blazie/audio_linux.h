/* audio_linux.h -- the sound card, for the Linux shell: 16-bit mono at the unit's rate, in small blocks, the writes
 * blocking while the card's buffer is full -- so the card's clock paces the unit, as waveOut's does on Windows.
 *
 * ALSA (libasound) when built with it, which needs nothing else on the machine (no desktop, no sound server: a
 * BTSpeak's or a Raspberry Pi's console); on a desktop ALSA's "default" device usually reaches PulseAudio or
 * PipeWire through their ALSA plugin.  Built without ALSA's headers, PulseAudio's simple API (BLAZIE_AUDIO_PULSE).
 */
#ifndef BLAZIE_AUDIO_LINUX_H
#define BLAZIE_AUDIO_LINUX_H

typedef struct audio_out audio_out;

/* device: ALSA's device name (NULL or "": "default"); block: frames per write; blocks: how many may wait in the
   card's buffer.  NULL on failure, the reason in err. */
audio_out *audio_open(const char *device, int rate, int block, int blocks, char *err, int errlen);
/* n frames: returns when the card has room for them (an underrun is recovered); 0 on an error that lasts */
int audio_write(audio_out *a, const short *pcm, int n);
void audio_close(audio_out *a);
/* "ALSA" or "PulseAudio" */
const char *audio_backend(void);

#endif

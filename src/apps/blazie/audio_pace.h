/* audio_pace.h -- how far ahead the shell's sound thread renders the unit, and when it saves the unit's memory
 * (Tomi: the emulator's speech stutters, the add-on's doesn't).  Portable C99, no platform calls: main_win.c's
 * waveOut thread runs it, the Linux shells' sound thread (audio_linux.c, ALSA or PulseAudio) too, and test_audio.c
 * runs both against a simulated sound card.
 *
 * The thread keeps ap_target blocks queued at the sound card, rendering one each time the card hands one back.  The
 * 0.7.0 draft kept four blocks of 10 ms: 30-40 ms of sound in hand.  On this desktop's own card that never ran dry,
 * even with every core loaded; but a writer woken late (Remote Desktop's audio, a laptop saving power, a busy
 * machine) or a card that takes its sound in bigger or later passes empties 40 ms, and every time it does the
 * speech stops for the rest of the delay: the stutter.  Measured with injected wake-up delays (5% of the writes held
 * up to 60 ms): four blocks lost 1.6-2.0 s of 15 s, twelve lost 30-50 ms.
 *
 * Sound buffer (the Settings menu, [sound] buffer=):
 *   automatic  60 ms; each time the card is found to have run dry the queue grows (60, 100, 150, 220, 250 ms) and
 *              stays grown for the session -- a key's answer is heard that much later, the speech no longer chops
 *   short      40 ms (the 0.7.0 draft's), medium 100 ms, long 250 ms (Remote Desktop): fixed
 *
 * Running dry is told from the card's played position against the wall clock (ap_observe): while the card plays,
 * the two advance together; every gap puts the clock ahead by its length, for good.  (Counting the blocks handed
 * back does not see it: waveOut keeps the last block until the next one comes, and its position stops one block
 * short, so the queue never reads empty.)  A position that moves in steps (a card reporting per pass) only goes
 * ahead for a moment, so a lead must last AP_GAP_PERSIST_MS to count; a card's clock drifting from the wall
 * clock's is followed at up to AP_DRIFT_PER_S.
 *
 * Saving: the minute's save runs on the sound thread, after it has rendered AP_SAVE_AHEAD_MS beyond its target
 * (ap_want with a save pending, then ap_save_now): the card plays the queue while the memory is written, and no key
 * waits on the save.  A save slower than the queue in hand still leaves a gap (then the automatic queue grows).
 */
#ifndef BLAZIE_AUDIO_PACE_H
#define BLAZIE_AUDIO_PACE_H

enum { AP_AUTO, AP_SHORT, AP_MEDIUM, AP_LONG, AP_N_MODES };

#define AP_AUTO_START_MS 60
#define AP_SHORT_MS 40
#define AP_MEDIUM_MS 100
#define AP_LONG_MS 250
#define AP_MAX_MS 250                /* the automatic queue grows no further */
#define AP_GROW_MIN_MS 40            /* each growth: at least this, or half the queue */
#define AP_GROW_HOLD_MS 300.0        /* one growth per stall: detections closer than this grow once */
#define AP_SAVE_AHEAD_MS 50          /* rendered beyond the target before a save */
#define AP_GAP_MIN_MS 3.0            /* a lead of the wall clock over the played position this long ... */
#define AP_GAP_PERSIST_MS 150.0      /* ... held this long is a gap */
#define AP_DRIFT_PER_S 2.0           /* the card's clock may drift from the wall clock's this much (ms per s) */
#define AP_MIN_BLOCK_MS 5
/* the blocks a shell needs at most (any block_ms from AP_MIN_BLOCK_MS) */
#define AP_MAX_BLOCKS ((AP_MAX_MS + AP_SAVE_AHEAD_MS) / AP_MIN_BLOCK_MS)

typedef struct {
    int mode, block_ms;
    int target;                      /* blocks kept queued */
    int primed;                      /* the position has started moving */
    double base, last_t, over_since, last_grow;
    int gaps;                        /* gaps seen, and their length */
    double gap_ms;
} audio_pace;

void ap_init(audio_pace *p, int mode, int block_ms);
/* a new setting: its own queue (the automatic one starts again at AP_AUTO_START_MS) */
void ap_set_mode(audio_pace *p, int mode);
int ap_target(const audio_pace *p);
/* the blocks the shell must have for this block length (the longest queue and a save's render-ahead) */
int ap_capacity(int block_ms);
/* now_ms: the wall clock; played_ms: the card's played position (ms of sound since it opened).  The gap's length in
   ms when one is found now (the automatic queue has grown), else 0. */
double ap_observe(audio_pace *p, double now_ms, double played_ms);
/* blocks to render and queue now, with in_flight queued; save_pending: a save waits (render ahead for it) */
int ap_want(const audio_pace *p, int in_flight, int save_pending);
/* 1: the waiting save may run now (the queue holds its render-ahead) */
int ap_save_now(const audio_pace *p, int in_flight, int save_pending);
/* "auto", "short", "medium", "long"; and back (-1: none of them) */
const char *ap_mode_name(int mode);
int ap_mode_of(const char *name);

/* A card that is written into a ring and says how much of it is still to play (audio_linux.c: ALSA's
 * snd_pcm_avail_delay, PulseAudio's latency) rather than handing blocks back as waveOut does.  The Linux shells'
 * sound thread asks it on each turn: the blocks in flight (ap_blocks_queued: frames still in the ring, rounded up
 * to whole blocks, so a block partly played still counts), the played position (ap_played_ms: what was written less
 * what is still to be heard, the device's and sound server's own latency included), and, when ap_want says
 * nothing, how long to sleep before a block is wanted (ap_wait_ms: until the ring has drained to one block under
 * the queue, at most one block, at least AP_WAIT_MIN_MS). */
#define AP_WAIT_MIN_MS 1.0
int ap_blocks_queued(long queued_frames, int block_frames);
double ap_played_ms(unsigned long long written_frames, long delay_frames, int rate);
double ap_wait_ms(const audio_pace *p, long queued_frames, int block_frames, int rate, int save_pending);
/* the ring the Linux shells open: the longest queue and a save's render-ahead (ap_capacity), in frames; it starts
   playing once the shortest queue (AP_SHORT_MS) is in, so no choice can leave it waiting to start */
long ap_ring_frames(int block_frames, int block_ms);
long ap_start_frames(int block_frames, int block_ms);

#endif

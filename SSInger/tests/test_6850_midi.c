/* test_6850_midi.c -- offline checks for the Robovox emulator cores.
 *
 * 1. MC6850: reset/control programming, RDRF/TDRE status, Rx IRQ line,
 *    overrun + framing flags, transmit path.
 * 2. Translator vectors: patent embodiment-1 mapping (phoneme ch N,
 *    pitch ch N+1 at A=440, wheels, CC2/CC3/CC64, program-change
 *    embodiment 2, running status/sysex robustness).
 * 3. Bus: MIDI wire byte -> ACIA IRQ -> translator -> SC-02 latch, CPU
 *    socket map reads/writes, A/R sustain service.
 * 4. Engine smoke: real ssi263 chip speaks through the bus path; amplitude
 *    latches, inflection round-trips, mono priority.
 *
 * Build: gcc -std=c99 -I robovox -I src/csrc tests/test_6850_midi.c
 *        src/csrc/ssi263.c -lm
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "emu/mc6850.h"
#include "emu/robovox_firmware.h"
#include "emu/robovox_bus.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/* ---- 6850 ------------------------------------------------------------ */

static void test_acia_reset(void)
{
    mc6850_t m;
    mc6850_init(&m);
    CHECK(mc6850_read_status(&m) == M6850_SR_TDRE, "power-on status 0x%02X", mc6850_read_status(&m));
    CHECK(mc6850_irq_level(&m) == 0, "power-on IRQ quiet");
    mc6850_receive_byte(&m, 0x90, 0); /* in reset: ignored */
    CHECK(!(mc6850_read_status(&m) & M6850_SR_RDRF), "bytes ignored while in reset");
    mc6850_write_ctrl(&m, 0x03); /* master reset, explicit */
    CHECK(mc6850_read_status(&m) == M6850_SR_TDRE, "post-reset status");
    mc6850_write_ctrl(&m, 0x95); /* /16, 8N1, no Tx IRQ, Rx IRQ on */
    CHECK(mc6850_read_status(&m) == M6850_SR_TDRE, "programmed status 0x%02X", mc6850_read_status(&m));
}

static int irq_edges;
static void irq_probe(int level, void *ctx)
{
    (void)ctx;
    irq_edges += level ? 1000 : 1; /* thousands = asserts, ones = clears */
}

static void test_acia_rx_irq(void)
{
    mc6850_t m;
    mc6850_init(&m);
    m.irq_cb = irq_probe;
    m.irq_ctx = 0;
    irq_edges = 0;
    mc6850_write_ctrl(&m, 0x03);
    mc6850_write_ctrl(&m, 0x95);
    mc6850_receive_byte(&m, 0x90, 0);
    CHECK(mc6850_read_status(&m) == (M6850_SR_IRQ | M6850_SR_TDRE | M6850_SR_RDRF),
          "rx status 0x%02X", mc6850_read_status(&m));
    CHECK(irq_edges == 1000, "IRQ asserted once (%d)", irq_edges);
    CHECK(mc6850_read_data(&m) == 0x90, "data read back");
    CHECK(!(mc6850_read_status(&m) & M6850_SR_RDRF), "RDRF cleared by read");
    CHECK(!(mc6850_read_status(&m) & M6850_SR_IRQ), "IRQ cleared by read");
    CHECK(irq_edges == 1001, "IRQ cleared once (%d)", irq_edges);
}

static void test_acia_overrun_framing(void)
{
    mc6850_t m;
    mc6850_init(&m);
    mc6850_write_ctrl(&m, 0x03);
    mc6850_write_ctrl(&m, 0x95);
    mc6850_receive_byte(&m, 0x3C, 0);
    mc6850_receive_byte(&m, 0x40, 0); /* lost: RDRF still held */
    CHECK(mc6850_read_status(&m) & M6850_SR_OVRN, "overrun flagged");
    CHECK(mc6850_read_data(&m) == 0x3C, "older byte kept");
    CHECK(!(mc6850_read_status(&m) & M6850_SR_OVRN), "overrun cleared by read");
    mc6850_receive_byte(&m, 0x40, 1); /* bad stop bit */
    CHECK(mc6850_read_status(&m) & M6850_SR_FE, "framing error flagged");
    CHECK(mc6850_read_data(&m) == 0x40, "framed byte still delivered");
}

static void test_acia_tx(void)
{
    mc6850_t m;
    mc6850_init(&m);
    mc6850_write_ctrl(&m, 0x03);
    mc6850_write_ctrl(&m, 0x35); /* 8N1, Tx IRQ enabled, Rx IRQ off */
    mc6850_write_data(&m, 0xFE);
    CHECK(!(mc6850_read_status(&m) & M6850_SR_TDRE), "TDRE clears on write");
    CHECK(!(mc6850_read_status(&m) & M6850_SR_IRQ), "no IRQ while busy");
    mc6850_tx_done(&m);
    CHECK(mc6850_read_status(&m) & M6850_SR_TDRE, "TDRE back on done");
    CHECK(mc6850_read_status(&m) & M6850_SR_IRQ, "Tx-empty IRQ when enabled");
}

/* ---- translator -------------------------------------------------------- */

typedef struct {
    int n;
    int voice[64], addr[64], value[64];
    double xck;
} sc_log_t;

static void sc_probe(int voice, int addr, int value, void *ctx)
{
    sc_log_t *l = (sc_log_t *)ctx;
    if (l->n < 64) {
        l->voice[l->n] = voice;
        l->addr[l->n] = addr;
        l->value[l->n] = value;
    }
    l->n++;
}

static void xck_probe(double hz, void *ctx)
{
    ((sc_log_t *)ctx)->xck = hz;
}

static void fw_init(robovox_state_t *s, sc_log_t *l)
{
    memset(l, 0, sizeof(*l));
    robovox_default_cfg(&s->cfg);
    s->sc_write = sc_probe;
    s->xck_write = xck_probe;
    s->sink_ctx = l;
    robovox_default_table(s);
    robovox_reset(s);
}

static void feed(robovox_state_t *s, const uint8_t *bytes, int n)
{
    int i;
    for (i = 0; i < n; i++)
        robovox_midi_byte(s, bytes[i]);
}

static void test_embodiment1(void)
{
    robovox_state_t s;
    sc_log_t l;
    const uint8_t on[] = { 0x90, 60, 100 };
    const uint8_t off[] = { 0x80, 60, 0 };
    const uint8_t pitch[] = { 0x91, 69, 90 };
    const uint8_t pitchoff[] = { 0x81, 69, 0 };
    int r0 = -1, r3 = -1, i, r1 = -1, r2 = -1;
    fw_init(&s, &l);
    feed(&s, on, 3);
    for (i = 0; i < l.n; i++) {
        if (l.addr[i] == RV_SC_R0)
            r0 = l.value[i];
        if (l.addr[i] == RV_SC_R3)
            r3 = l.value[i];
    }
    /* Note 60 -> table code 25 (0x19 UH1); DUR=2 in high bits. Phoneme
     * velocity never touches volume (pitch owns it). */
    CHECK(r0 == ((2 << 6) | 25), "phoneme R0=0x%02X", r0);
    CHECK(r3 == -1, "phoneme writes no R3");
    l.n = 0;
    feed(&s, off, 3);
    r0 = -1;
    for (i = 0; i < l.n; i++)
        if (l.addr[i] == RV_SC_R0)
            r0 = l.value[i];
    CHECK(r0 == ((2 << 6) | RV_PHONEME_PAUSE), "note-off writes PA, R0=0x%02X", r0);
    /* Pitch: A4 440 Hz @1 MHz -> I=3812 -> R1=220, R2=rate|0x0C. */
    l.n = 0;
    feed(&s, pitch, 3);
    for (i = 0; i < l.n; i++) {
        if (l.addr[i] == RV_SC_R1)
            r1 = l.value[i];
        if (l.addr[i] == RV_SC_R2)
            r2 = l.value[i];
    }
    CHECK(r1 == (((3812 >> 3) & 0xF8) | 4), "inflection R1=%d", r1);
    CHECK(r2 == ((8 << 4) | 0x0C), "inflection R2=0x%02X", r2);
    l.n = 0;
    feed(&s, pitchoff, 3);
    CHECK(l.n == 0, "pitch-off latched: no writes");
}

static void test_wheels_and_cc(void)
{
    robovox_state_t s;
    sc_log_t l;
    const uint8_t bend[] = { 0xE0, 0x00, 0x40 }; /* center */
    const uint8_t bendUp[] = { 0xE0, 0x00, 0x50 }; /* +2048 -> FF +16 */
    const uint8_t mod[] = { 0xB0, 1, 127 };
    const uint8_t cc2[] = { 0xB0, 2, 96 };
    const uint8_t cc3[] = { 0xB0, 3, 120 };
    const uint8_t cc64[] = { 0xB0, 64, 100 };
    int i, r4 = -1;
    fw_init(&s, &l);
    /* Patent map: bend center leaves FF at default. */
    feed(&s, bend, 3);
    for (i = 0; i < l.n; i++)
        if (l.addr[i] == RV_SC_R4)
            r4 = l.value[i];
    CHECK(r4 == 0xE4, "bend center keeps FF=0xE4 (got 0x%02X)", r4);
    l.n = 0;
    feed(&s, bendUp, 3);
    r4 = -1;
    for (i = 0; i < l.n; i++)
        if (l.addr[i] == RV_SC_R4)
            r4 = l.value[i];
    CHECK(r4 == 0xE4 + 16, "bend up raises FF (got 0x%02X)", r4);
    /* Patent map: mod -> articulation 7. */
    l.n = 0;
    feed(&s, mod, 3);
    CHECK(s.cfg.articulation == 7, "mod max sets ART=7 (got %d)", s.cfg.articulation);
    /* CC2/CC3/CC64 are map-independent. */
    feed(&s, cc2, 3);
    CHECK(s.v[0].cc2 == 96, "CC2 stored (%d)", s.v[0].cc2);
    feed(&s, cc3, 3);
    CHECK(s.cfg.rate == 15, "CC3 sets rate (%d)", s.cfg.rate);
    feed(&s, cc64, 3);
    CHECK(s.v[0].carrier_ext == 1, "CC64 selects external carrier");
}

static void test_polaxis_map(void)
{
    robovox_state_t s;
    sc_log_t l;
    const uint8_t bend[] = { 0xE0, 0x00, 0x60 };
    const uint8_t mod[] = { 0xB0, 1, 64 };
    fw_init(&s, &l);
    s.cfg.ctlmap = RV_MAP_POLAXIS;
    feed(&s, bend, 3);
    CHECK(l.xck > 1000000.0, "polaxis bend raises XCK (%f)", l.xck);
    {
        /* Absolute from nominal: center returns exactly, repeats don't drift. */
        const uint8_t center[] = { 0xE0, 0x00, 0x40 };
        double once;
        l.xck = 0.0;
        feed(&s, bend, 3);
        once = l.xck;
        feed(&s, bend, 3);
        CHECK(l.xck == once, "repeat bend is stable (%f)", l.xck);
        feed(&s, center, 3);
        CHECK(l.xck == 1000000.0, "bend center returns to nominal (%f)", l.xck);
    }
    l.n = 0;
    feed(&s, mod, 3);
    CHECK(l.n == 1 && l.addr[0] == RV_SC_R4, "polaxis mod writes R4");
    CHECK(l.value[0] == 64 * 255 / 127, "polaxis mod scales FF (%d)", l.value[0]);
}

static void test_embodiment2_and_parser(void)
{
    robovox_state_t s;
    sc_log_t l;
    const uint8_t pc[] = { 0xC0, 40 };
    const uint8_t junk1[] = { 0xF0, 0x41, 0x10, 0xF7, /* sysex */
                              0x90, 60, 100, 62, 90, /* running status 2nd note */
                              0xF8 };                /* realtime ignored */
    const uint8_t junk2[] = { 0x80, 60, 0 };
    const uint8_t junk3[] = { 0x80, 62, 0 };
    int i, r0 = -1;
    fw_init(&s, &l);
    s.cfg.embodiment = RV_EMB_EXPANDER;
    feed(&s, pc, 2);
    for (i = 0; i < l.n; i++)
        if (l.addr[i] == RV_SC_R0)
            r0 = l.value[i];
    /* Program 40 -> table[40] = code 5 (AY). */
    CHECK(r0 == ((2 << 6) | 5), "program change selects phoneme, R0=0x%02X", r0);
    /* Parser robustness in embodiment 1. */
    fw_init(&s, &l);
    feed(&s, junk1, (int)sizeof(junk1));
    CHECK(s.v[0].note_held == 1, "running-status note held");
    CHECK(s.v[0].phoneme == s.note2phon[62], "2nd note phoneme %d", s.v[0].phoneme);
    feed(&s, junk2, (int)sizeof(junk2));
    CHECK(s.v[0].note_held == 1, "first off keeps running-status 2nd note");
    CHECK(s.v[0].phoneme == s.note2phon[62], "falls back to 2nd note %d", s.v[0].phoneme);
    feed(&s, junk3, (int)sizeof(junk3));
    CHECK(s.v[0].note_held == 0, "last note-off releases");
    CHECK(s.v[0].phoneme == RV_PHONEME_PAUSE, "last note-off writes PA");
    /* Editable table (e.g. Polaxis anchor: note 36 = U, code 0x16). */
    fw_init(&s, &l);
    robovox_set_note_phoneme(&s, 36, 0x16);
    {
        const uint8_t u[] = { 0x90, 36, 100 };
        feed(&s, u, 3);
        CHECK(s.v[0].phoneme == 0x16, "remapped note 36 -> U");
    }
    CHECK(strcmp(robovox_phoneme_names[0x16], "U") == 0, "phoneme name table");
}

static void test_map_file(void)
{
    robovox_state_t s;
    sc_log_t l;
    const char *path = "robovox_test_map.tmp";
    FILE *f = fopen(path, "w");
    int n;
    CHECK(f != 0, "map file created");
    fputs("# comment line\n\nh2o=EH\n", f);
    fputs("C4 = uh1\n", f);   /* spaces + lowercase */
    fputs("Db4=SCH\n", f);    /* flat */
    fputs("bogus line\n", f);
    fputs("C9=ZZZ\n", f);     /* unknown phoneme */
    fputs("H4=E\n", f);       /* unknown note */
    fclose(f);
    fw_init(&s, &l);
    n = robovox_load_map(&s, path, 0);
    CHECK(n == 2, "2 lines applied (%d)", n);
    CHECK(s.note2phon[60] == 0x19, "C4 -> UH1");
    CHECK(s.note2phon[61] == 0x32, "Db4 -> SCH");
    CHECK(s.note2phon[36] == -1, "replace mode clears the rest");
    n = robovox_load_map(&s, path, 1);
    CHECK(n == 2, "overlay re-applies (%d)", n);
    CHECK(robovox_note_name("A4") == 69, "A4=69");
    CHECK(robovox_note_name("C-1") == 0, "C-1=0");
    CHECK(robovox_note_name("G9") == 127, "G9=127");
    CHECK(robovox_note_name("C#-1") == 1, "C#-1=1");
    CHECK(robovox_note_name("X4") < 0, "bad letter rejected");
    CHECK(robovox_note_name("C") < 0, "missing octave rejected");
    CHECK(robovox_phoneme_code("uh1") == 0x19, "case-insensitive code");
    CHECK(robovox_phoneme_code(":OH") == 0x3B, "colon code");
    CHECK(robovox_phoneme_code("NOPE") < 0, "unknown code rejected");
    CHECK(robovox_load_map(&s, "no-such-file.tmp", 0) < 0, "missing file errors");
    remove(path);
}

static void test_quad_channels(void)
{
    robovox_state_t s;
    sc_log_t l;
    const uint8_t v3[] = { 0x94, 60, 100 }; /* ch5 phoneme (3rd pair) */
    const uint8_t p4[] = { 0x97, 69, 100 }; /* ch8 pitch (4th pair) */
    fw_init(&s, &l);
    s.cfg.nvoices = 4;
    feed(&s, v3, 3);
    CHECK(l.n > 0 && l.voice[0] == 2, "ch5 reaches voice 2 (got %d writes)", l.n);
    l.n = 0;
    feed(&s, p4, 3);
    CHECK(l.n > 0 && l.voice[0] == 3, "ch8 reaches voice 3");
}

/* ---- bus + engine ------------------------------------------------------ */

static void test_bus_path(void)
{
    robovox_bus_t b;
    ssi263_params p;
    const uint8_t seq[] = { 0x90, 48, 110, 0x91, 57, 100 };
    int i, ok;
    ssi263_default_params(&p);
    ok = robovox_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom());
    CHECK(ok, "bus init");
    CHECK(mc6850_read_status(&b.acia) == (M6850_SR_TDRE),
          "acia programmed, status 0x%02X", mc6850_read_status(&b.acia));
    for (i = 0; i < (int)sizeof(seq); i++)
        robovox_bus_midi_byte(&b, seq[i]);
    CHECK(b.regs_mirror[0][0] == ((2 << 6) | (48 - 36 + 1)),
          "bus latch R0=0x%02X", b.regs_mirror[0][0]);
    CHECK(b.irq_to_cpu == 0, "IRQ drains fully (level %d)", b.irq_to_cpu);
    /* CPU socket: RAM round-trip, ACIA status, mode switch, SC mirror. */
    robovox_bus_cpu_write(&b, 0x0200, 0xA5);
    CHECK(robovox_bus_cpu_read(&b, 0x0200) == 0xA5, "RAM round-trip");
    CHECK(robovox_bus_cpu_read(&b, 0x1000) == M6850_SR_TDRE, "ACIA status via bus");
    CHECK(robovox_bus_cpu_read(&b, 0x1800) == RV_MODE_SEQ, "mode switch reads SEQ");
    CHECK(robovox_bus_cpu_read(&b, 0x1400) == (uint8_t)b.regs_mirror[0][0],
          "SC-02 buffer mirrors R0");
    robovox_bus_cpu_write(&b, 0x1000, 0x03);
    CHECK(mc6850_read_status(&b.acia) == M6850_SR_TDRE, "CPU can reset ACIA");
    robovox_bus_free(&b);
}

static void test_engine_smoke(void)
{
    robovox_bus_t b;
    ssi263_params p;
    static double out[44100];
    double peak = 0.0;
    long i;
    const uint8_t seq[] = { 0x90, 44, 120, 0x91, 62, 110 };
    ssi263_default_params(&p);
    CHECK(robovox_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "smoke bus init");
    for (i = 0; i < (long)sizeof(seq); i++)
        robovox_bus_midi_byte(&b, seq[i]);
    robovox_bus_render(&b, 22050, out);
    robovox_bus_service_all(&b); /* A/R sustain like the firmware loop */
    robovox_bus_render(&b, 22050, out);
    for (i = 0; i < 44100; i++) {
        double a = out[i] < 0 ? -out[i] : out[i];
        if (a > peak)
            peak = a;
    }
    CHECK(peak > 1e-4, "chip speaks through bus path (peak %f)", peak);
    /* XCK retune is in-place and exact (no rebuild, registers kept). */
    robovox_bus_xck_write(2000000.0, &b);
    CHECK(b.xck_hz == 2000000.0, "XCK retuned");
    CHECK(b.regs_mirror[0][0] == ((2 << 6) | (44 - 36 + 1)), "registers survive retune");
    robovox_bus_free(&b);
}

/* Mirrors RobovoxProcessor::renderChunk order (event, render, service).
 * Amplitude latches like the hardware: velocity writes R3 at once, no
 * VST-side slew. */
static void test_vst_render_path(void)
{
    robovox_bus_t b;
    ssi263_params p;
    static double out[512];
    double e = 0.0, efirst = 0.0;
    int i, k;
    const uint8_t pon[] = { 0x91, 45, 100 };
    const uint8_t non[] = { 0x90, 45, 110 };
    ssi263_default_params(&p);
    CHECK(robovox_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "vstpath init");
    for (i = 0; i < 3; i++)
        robovox_bus_midi_byte(&b, pon[i]);
    for (i = 0; i < 3; i++)
        robovox_bus_midi_byte(&b, non[i]);
    for (k = 0; k < 86; k++) {
        robovox_bus_render(&b, 512, out);
        robovox_bus_service_all(&b);
        for (i = 0; i < 512; i++) {
            e += out[i] * out[i];
            if (k == 0)
                efirst += out[i] * out[i];
        }
    }
    CHECK(sqrt(e / (86.0 * 512)) > 0.02, "VST render path sings (rms %f)",
          sqrt(e / (86.0 * 512)));
    /* First 11.6 ms still ramps the formant latches from silence, but the
     * voice is present immediately (far above the ~0.0001 carrier bleed:
     * no amplitude gating). */
    CHECK(sqrt(efirst / 512) > 0.002, "first block already singing (rms %f)",
          sqrt(efirst / 512));
    robovox_bus_free(&b);
}

/* Pitch owns volume and latches it: pitch velocity lands at once and no
 * release or phoneme event moves it; phoneme events never write R3. */
static void test_amp_latch(void)
{
    robovox_bus_t b;
    ssi263_params p;
    const uint8_t phon[] = { 0x90, 48, 127 };
    const uint8_t phonoff[] = { 0x80, 48, 0 };
    const uint8_t pitch[] = { 0x91, 60, 100 };
    const uint8_t pitchoff[] = { 0x81, 60, 0 };
    int i, want = (1 + (100 * 14) / 127);
    ssi263_default_params(&p);
    CHECK(robovox_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "latch bus init");
    for (i = 0; i < 3; i++)
        robovox_bus_midi_byte(&b, phon[i]);
    CHECK((b.regs_mirror[0][3] & 0x0F) == 0, "phoneme moves no amp (stays %d)",
          b.regs_mirror[0][3] & 0x0F);
    for (i = 0; i < 3; i++)
        robovox_bus_midi_byte(&b, pitch[i]);
    CHECK((b.regs_mirror[0][3] & 0x0F) == want,
          "pitch vel latches amp at once (got %d)", b.regs_mirror[0][3] & 0x0F);
    for (i = 0; i < 3; i++)
        robovox_bus_midi_byte(&b, pitchoff[i]);
    CHECK((b.regs_mirror[0][3] & 0x0F) == want, "pitch-off keeps amp");
    for (i = 0; i < 3; i++)
        robovox_bus_midi_byte(&b, phonoff[i]);
    CHECK((b.regs_mirror[0][3] & 0x0F) == want, "phoneme-off keeps amp (PA gates)");
    robovox_bus_free(&b);
}

/* Pitch encoding round-trips through R1/R2 for every MIDI note (DUR != 3):
 * reassembling the emitted registers with the engine formula must give
 * back the exact 12-bit inflection, i.e. optimal half-LSB tuning. */
static void test_inflection_exact(void)
{
    robovox_state_t s;
    sc_log_t l;
    int note;
    fw_init(&s, &l);
    for (note = 0; note < 128; note++) {
        uint8_t on[] = { 0x91, (uint8_t)note, 100 };
        uint8_t off[] = { 0x81, (uint8_t)note, 0 };
        int i, r1 = -1, r2 = -1, want, got;
        l.n = 0;
        feed(&s, on, 3);
        for (i = 0; i < l.n; i++) {
            if (l.addr[i] == RV_SC_R1)
                r1 = l.value[i];
            if (l.addr[i] == RV_SC_R2)
                r2 = l.value[i];
        }
        CHECK(r1 >= 0 && r2 >= 0, "note %d emits R1+R2", note);
        want = robovox_freq_to_inflection(robovox_note_freq(note), s.cfg.xck_hz, 64);
        /* Engine formula, non-glide mode (ssi263.c update_inflection). */
        got = r1 * 8 + (((r2 >> 3) & 1) * 2048 + (r2 & 7));
        CHECK(got == want, "note %d round-trips I (want %d got %d)", note, want, got);
        l.n = 0;
        feed(&s, off, 3);
    }
}

/* Full-keyboard frequency sweep (notes 36..96 through the bus path):
 * the emitted R1/R2, reassembled per the engine formula, must land within
 * 12 cents of 12TET at 1 MHz. Fails by hundreds of cents if R1's low bits
 * carry glide instead of pitch (up to +1311 cents at note 96); fixed, the
 * worst note is +9.5 cents (note 90, the chip's own top-range step). */
static void test_pitch_sweep(void)
{
    robovox_state_t s;
    sc_log_t l;
    int note;
    fw_init(&s, &l);
    for (note = 36; note <= 96; note++) {
        uint8_t on[] = { 0x91, (uint8_t)note, 100 };
        uint8_t off[] = { 0x81, (uint8_t)note, 0 };
        int i, r1 = -1, r2 = -1, I;
        double f, fp, cents;
        l.n = 0;
        feed(&s, on, 3);
        for (i = 0; i < l.n; i++) {
            if (l.addr[i] == RV_SC_R1)
                r1 = l.value[i];
            if (l.addr[i] == RV_SC_R2)
                r2 = l.value[i];
        }
        CHECK(r1 >= 0 && r2 >= 0, "note %d emits R1+R2", note);
        I = r1 * 8 + (((r2 >> 3) & 1) * 2048 + (r2 & 7));
        CHECK(I > 0 && I < 4096, "note %d I in range (%d)", note, I);
        f = robovox_note_freq(note);
        fp = s.cfg.xck_hz / (8.0 * (4096.0 - I));
        cents = 1200.0 * log(fp / f) / log(2.0);
        if (cents < 0)
            cents = -cents;
        CHECK(cents <= 12.0, "note %d in tune (%d cents, I=%d)", note, (int)cents, I);
        l.n = 0;
        feed(&s, off, 3);
    }
}

/* Pitch channel latches inflection + amplitude (performance): latest
 * note-on wins, releases change nothing. */
static void test_pitch_latch(void)
{
    robovox_state_t s;
    sc_log_t l;
    const uint8_t c4[] = { 0x91, 60, 100 };
    const uint8_t e4[] = { 0x91, 64, 110 };
    const uint8_t offE[] = { 0x81, 64, 0 };
    const uint8_t offC[] = { 0x80, 60, 0 };
    int e4infl, e4amp;
    fw_init(&s, &l);
    feed(&s, c4, 3);
    CHECK(s.v[0].pitch_note == 60, "first pitch latched");
    feed(&s, e4, 3);
    CHECK(s.v[0].pitch_note == 64, "latest pitch wins");
    e4infl = s.v[0].inflection;
    e4amp = s.v[0].amplitude;
    CHECK(e4infl == robovox_freq_to_inflection(robovox_note_freq(64), s.cfg.xck_hz, 64),
          "latched note sets its inflection");
    CHECK(e4amp == robovox_vel_to_amp(&s, 110), "latched note sets its amp");
    l.n = 0;
    feed(&s, offE, 3);
    CHECK(l.n == 0, "pitch release writes nothing");
    CHECK(s.v[0].pitch_note == 64, "pitch stays latched");
    CHECK(s.v[0].inflection == e4infl && s.v[0].amplitude == e4amp,
          "inflection+amp stay latched");
    feed(&s, offC, 3);
    CHECK(s.v[0].pitch_note == 64, "older release changes nothing");
}

/* Nominal base + bend combine absolutely (studio clock discipline). */
static void test_clock_base_and_bend(void)
{
    robovox_state_t s;
    sc_log_t l;
    const uint8_t up[] = { 0xE0, 0x00, 0x60 };   /* +12 st at range 24 */
    const uint8_t center[] = { 0xE0, 0x00, 0x40 };
    const uint8_t pbend[] = { 0xE0, 0x00, 0x60 };
    fw_init(&s, &l);
    s.cfg.ctlmap = RV_MAP_POLAXIS;
    robovox_set_nominal_xck(&s, 2000000.0);
    CHECK(l.xck == 2000000.0, "centered base lands at once (%f)", l.xck);
    feed(&s, up, 3);
    CHECK(l.xck == 4000000.0, "bend scales from the base (%f)", l.xck);
    feed(&s, center, 3);
    CHECK(l.xck == 2000000.0, "center returns to the base (%f)", l.xck);
    /* Patent map: bend owns the filter, never the clock. */
    fw_init(&s, &l);
    feed(&s, pbend, 3);
    CHECK(l.xck == 0.0, "patent bend leaves XCK alone (%f)", l.xck);
}

/* ssi263_set_xck on a fresh chip == a fresh ssi263_new at that clock:
 * byte-identical output (the narrowed exactness claim in ssi263.h). */
static void test_xck_fresh_exact(void)
{
    ssi263_params p, q;
    const unsigned char *rom = ssi263_default_rom();
    ssi263 *a, *b;
    static double oa[8192], ob[8192];
    static const int prog[5][2] = {
        { 0, (2 << 6) | 25 }, { 1, 220 }, { 2, (8 << 4) | 0x0C },
        { 4, 0xE4 }, { 3, (5 << 4) | 15 }
    };
    long i, n;
    int k;
    ssi263_default_params(&p);
    q = p;
    q.xck_hz = 2000000.0;
    a = ssi263_new(&p, rom, 44100.0);
    b = ssi263_new(&q, rom, 44100.0);
    CHECK(a && b, "fresh chips");
    if (!a || !b) {
        if (a)
            ssi263_free(a);
        if (b)
            ssi263_free(b);
        return;
    }
    ssi263_set_xck(a, 2000000.0); /* before any writes: nothing started */
    for (k = 0; k < 5; k++) {
        ssi263_write(a, prog[k][0], prog[k][1]);
        ssi263_write(b, prog[k][0], prog[k][1]);
    }
    n = ssi263_run(a, 8192, oa);
    CHECK(n == 8192, "chip A renders");
    n = ssi263_run(b, 8192, ob);
    CHECK(n == 8192, "chip B renders");
    for (i = 0; i < 8192; i++)
        if (oa[i] != ob[i]) {
            CHECK(0, "retune byte-identical (first diff %ld: %a vs %a)", i, oa[i], ob[i]);
            break;
        }
    CHECK(i == 8192, "fresh retune == fresh chip at clock");
    ssi263_free(a);
    ssi263_free(b);
}

/* Phoneme channel likewise falls back across overlapping notes. */
static void test_phoneme_mono_priority(void)
{
    robovox_state_t s;
    sc_log_t l;
    const uint8_t a[] = { 0x90, 60, 100 };
    const uint8_t b[] = { 0x90, 62, 110 };
    const uint8_t offB[] = { 0x80, 62, 0 };
    const uint8_t offA[] = { 0x80, 60, 0 };
    fw_init(&s, &l);
    feed(&s, a, 3);
    CHECK(s.v[0].phoneme == s.note2phon[60], "first phoneme held");
    feed(&s, b, 3);
    CHECK(s.v[0].phoneme == s.note2phon[62], "second phoneme takes priority");
    feed(&s, offB, 3);
    CHECK(s.v[0].phoneme == s.note2phon[60], "release falls back to held phoneme");
    CHECK(s.v[0].note_held == 1, "still held");
    feed(&s, offA, 3);
    CHECK(s.v[0].phoneme == RV_PHONEME_PAUSE, "last release writes PA");
    CHECK(s.v[0].note_held == 0, "released");
}

int main(void)
{
    test_acia_reset();
    test_acia_rx_irq();
    test_acia_overrun_framing();
    test_acia_tx();
    test_embodiment1();
    test_wheels_and_cc();
    test_polaxis_map();
    test_embodiment2_and_parser();
    test_map_file();
    test_quad_channels();
    test_bus_path();
    test_engine_smoke();
    test_xck_fresh_exact();
    test_amp_latch();
    test_inflection_exact();
    test_pitch_sweep();
    test_pitch_latch();
    test_phoneme_mono_priority();
    test_clock_base_and_bend();
    test_vst_render_path();
    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

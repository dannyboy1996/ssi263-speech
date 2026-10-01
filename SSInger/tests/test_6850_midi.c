/* test_6850_midi.c -- offline checks for the SSInger emulator cores.
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
 * Build: gcc -std=c99 -I SSInger -I src/csrc tests/test_6850_midi.c
 *        src/csrc/ssi263.c -lm
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "emu/mc6850.h"
#include "emu/ssinger_firmware.h"
#include "emu/ssinger_bus.h"

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

static void fw_init(ssinger_state_t *s, sc_log_t *l)
{
    memset(l, 0, sizeof(*l));
    ssinger_default_cfg(&s->cfg);
    s->sc_write = sc_probe;
    s->xck_write = xck_probe;
    s->sink_ctx = l;
    ssinger_default_table(s);
    ssinger_reset(s);
}

static void feed(ssinger_state_t *s, const uint8_t *bytes, int n)
{
    int i;
    for (i = 0; i < n; i++)
        ssinger_midi_byte(s, bytes[i]);
}

static void test_embodiment1(void)
{
    ssinger_state_t s;
    sc_log_t l;
    const uint8_t on[] = { 0x90, 60, 100 };
    const uint8_t off[] = { 0x80, 60, 0 };
    const uint8_t pitch[] = { 0x91, 69, 90 };
    const uint8_t pitchoff[] = { 0x81, 69, 0 };
    int r0 = -1, r3 = -1, i, r1 = -1, r2 = -1;
    fw_init(&s, &l);
    feed(&s, on, 3);
    for (i = 0; i < l.n; i++) {
        if (l.addr[i] == SG_SC_R0)
            r0 = l.value[i];
        if (l.addr[i] == SG_SC_R3)
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
        if (l.addr[i] == SG_SC_R0)
            r0 = l.value[i];
    CHECK(r0 == ((2 << 6) | SG_PHONEME_PAUSE), "note-off writes PA, R0=0x%02X", r0);
    /* Pitch: A4 440 Hz @1 MHz -> I=3812 -> R1=220, R2=rate|0x0C. */
    l.n = 0;
    feed(&s, pitch, 3);
    for (i = 0; i < l.n; i++) {
        if (l.addr[i] == SG_SC_R1)
            r1 = l.value[i];
        if (l.addr[i] == SG_SC_R2)
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
    ssinger_state_t s;
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
        if (l.addr[i] == SG_SC_R4)
            r4 = l.value[i];
    CHECK(r4 == 0xE4, "bend center keeps FF=0xE4 (got 0x%02X)", r4);
    l.n = 0;
    feed(&s, bendUp, 3);
    r4 = -1;
    for (i = 0; i < l.n; i++)
        if (l.addr[i] == SG_SC_R4)
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
    ssinger_state_t s;
    sc_log_t l;
    const uint8_t bend[] = { 0xE0, 0x00, 0x60 };
    const uint8_t mod[] = { 0xB0, 1, 64 };
    fw_init(&s, &l);
    s.cfg.ctlmap = SG_MAP_POLAXIS;
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
    CHECK(l.n == 1 && l.addr[0] == SG_SC_R4, "polaxis mod writes R4");
    CHECK(l.value[0] == 64 * 255 / 127, "polaxis mod scales FF (%d)", l.value[0]);
}

static void test_embodiment2_and_parser(void)
{
    ssinger_state_t s;
    sc_log_t l;
    const uint8_t pc[] = { 0xC0, 40 };
    const uint8_t junk1[] = { 0xF0, 0x41, 0x10, 0xF7, /* sysex */
                              0x90, 60, 100, 62, 90, /* running status 2nd note */
                              0xF8 };                /* realtime ignored */
    const uint8_t junk2[] = { 0x80, 60, 0 };
    const uint8_t junk3[] = { 0x80, 62, 0 };
    int i, r0 = -1;
    fw_init(&s, &l);
    s.cfg.embodiment = SG_EMB_EXPANDER;
    feed(&s, pc, 2);
    for (i = 0; i < l.n; i++)
        if (l.addr[i] == SG_SC_R0)
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
    CHECK(s.v[0].phoneme == SG_PHONEME_PAUSE, "last note-off writes PA");
    /* Editable table (e.g. Polaxis anchor: note 36 = U, code 0x16). */
    fw_init(&s, &l);
    ssinger_set_note_phoneme(&s, 36, 0x16);
    {
        const uint8_t u[] = { 0x90, 36, 100 };
        feed(&s, u, 3);
        CHECK(s.v[0].phoneme == 0x16, "remapped note 36 -> U");
    }
    CHECK(strcmp(ssinger_phoneme_names[0x16], "U") == 0, "phoneme name table");
}

static void test_map_file(void)
{
    ssinger_state_t s;
    sc_log_t l;
    const char *path = "ssinger_test_map.tmp";
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
    n = ssinger_load_map(&s, path, 0);
    CHECK(n == 2, "2 lines applied (%d)", n);
    CHECK(s.note2phon[60] == 0x19, "C4 -> UH1");
    CHECK(s.note2phon[61] == 0x32, "Db4 -> SCH");
    CHECK(s.note2phon[36] == -1, "replace mode clears the rest");
    n = ssinger_load_map(&s, path, 1);
    CHECK(n == 2, "overlay re-applies (%d)", n);
    CHECK(ssinger_note_name("A4") == 69, "A4=69");
    CHECK(ssinger_note_name("C-1") == 0, "C-1=0");
    CHECK(ssinger_note_name("G9") == 127, "G9=127");
    CHECK(ssinger_note_name("C#-1") == 1, "C#-1=1");
    CHECK(ssinger_note_name("X4") < 0, "bad letter rejected");
    CHECK(ssinger_note_name("C") < 0, "missing octave rejected");
    CHECK(ssinger_phoneme_code("uh1") == 0x19, "case-insensitive code");
    CHECK(ssinger_phoneme_code(":OH") == 0x3B, "colon code");
    CHECK(ssinger_phoneme_code("NOPE") < 0, "unknown code rejected");
    CHECK(ssinger_load_map(&s, "no-such-file.tmp", 0) < 0, "missing file errors");
    remove(path);
}

static void test_quad_channels(void)
{
    ssinger_state_t s;
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
    ssinger_bus_t b;
    ssi263_params p;
    const uint8_t seq[] = { 0x90, 48, 110, 0x91, 57, 100 };
    int i, ok;
    ssi263_default_params(&p);
    ok = ssinger_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom());
    CHECK(ok, "bus init");
    CHECK(mc6850_read_status(&b.acia) == (M6850_SR_TDRE),
          "acia programmed, status 0x%02X", mc6850_read_status(&b.acia));
    for (i = 0; i < (int)sizeof(seq); i++)
        ssinger_bus_midi_byte(&b, seq[i]);
    CHECK(b.regs_mirror[0][0] == ((2 << 6) | (48 - 36 + 1)),
          "bus latch R0=0x%02X", b.regs_mirror[0][0]);
    CHECK(b.irq_to_cpu == 0, "IRQ drains fully (level %d)", b.irq_to_cpu);
    /* CPU socket: RAM round-trip, ACIA status, mode switch, SC mirror. */
    ssinger_bus_cpu_write(&b, 0x0200, 0xA5);
    CHECK(ssinger_bus_cpu_read(&b, 0x0200) == 0xA5, "RAM round-trip");
    CHECK(ssinger_bus_cpu_read(&b, 0x1000) == M6850_SR_TDRE, "ACIA status via bus");
    CHECK(ssinger_bus_cpu_read(&b, 0x1800) == SG_MODE_SEQ, "mode switch reads SEQ");
    CHECK(ssinger_bus_cpu_read(&b, 0x1400) == (uint8_t)b.regs_mirror[0][0],
          "SC-02 buffer mirrors R0");
    ssinger_bus_cpu_write(&b, 0x1000, 0x03);
    CHECK(mc6850_read_status(&b.acia) == M6850_SR_TDRE, "CPU can reset ACIA");
    ssinger_bus_free(&b);
}

static void test_engine_smoke(void)
{
    ssinger_bus_t b;
    ssi263_params p;
    static double out[44100];
    double peak = 0.0;
    long i;
    const uint8_t seq[] = { 0x90, 44, 120, 0x91, 62, 110 };
    ssi263_default_params(&p);
    CHECK(ssinger_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "smoke bus init");
    for (i = 0; i < (long)sizeof(seq); i++)
        ssinger_bus_midi_byte(&b, seq[i]);
    ssinger_bus_render(&b, 22050, out);
    ssinger_bus_service_all(&b); /* A/R sustain like the firmware loop */
    ssinger_bus_render(&b, 22050, out);
    for (i = 0; i < 44100; i++) {
        double a = out[i] < 0 ? -out[i] : out[i];
        if (a > peak)
            peak = a;
    }
    CHECK(peak > 1e-4, "chip speaks through bus path (peak %f)", peak);
    /* XCK retune is in-place and exact (no rebuild, registers kept). */
    ssinger_bus_xck_write(2000000.0, &b);
    CHECK(b.xck_hz == 2000000.0, "XCK retuned");
    CHECK(b.regs_mirror[0][0] == ((2 << 6) | (44 - 36 + 1)), "registers survive retune");
    ssinger_bus_free(&b);
}

/* Mirrors SSIngerProcessor's render loop (events, then ssinger_bus_run with
 * the service tick inside). Amplitude latches like the hardware: velocity
 * writes R3 at once, no VST-side slew. */
static void test_vst_render_path(void)
{
    ssinger_bus_t b;
    ssi263_params p;
    static double out[512];
    double e = 0.0, efirst = 0.0;
    int i, k;
    const uint8_t pon[] = { 0x91, 45, 100 };
    const uint8_t non[] = { 0x90, 45, 110 };
    ssi263_default_params(&p);
    CHECK(ssinger_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "vstpath init");
    for (i = 0; i < 3; i++)
        ssinger_bus_midi_byte(&b, pon[i]);
    for (i = 0; i < 3; i++)
        ssinger_bus_midi_byte(&b, non[i]);
    for (k = 0; k < 86; k++) {
        ssinger_bus_run(&b, 512, out);
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
    ssinger_bus_free(&b);
}

/* Pitch owns volume and latches it: pitch velocity lands at once and no
 * release or phoneme event moves it; phoneme events never write R3. */
static void test_amp_latch(void)
{
    ssinger_bus_t b;
    ssi263_params p;
    const uint8_t phon[] = { 0x90, 48, 127 };
    const uint8_t phonoff[] = { 0x80, 48, 0 };
    const uint8_t pitch[] = { 0x91, 60, 100 };
    const uint8_t pitchoff[] = { 0x81, 60, 0 };
    int i, want = (1 + (100 * 14) / 127);
    ssi263_default_params(&p);
    CHECK(ssinger_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "latch bus init");
    for (i = 0; i < 3; i++)
        ssinger_bus_midi_byte(&b, phon[i]);
    CHECK((b.regs_mirror[0][3] & 0x0F) == 0, "phoneme moves no amp (stays %d)",
          b.regs_mirror[0][3] & 0x0F);
    for (i = 0; i < 3; i++)
        ssinger_bus_midi_byte(&b, pitch[i]);
    CHECK((b.regs_mirror[0][3] & 0x0F) == want,
          "pitch vel latches amp at once (got %d)", b.regs_mirror[0][3] & 0x0F);
    for (i = 0; i < 3; i++)
        ssinger_bus_midi_byte(&b, pitchoff[i]);
    CHECK((b.regs_mirror[0][3] & 0x0F) == want, "pitch-off keeps amp");
    for (i = 0; i < 3; i++)
        ssinger_bus_midi_byte(&b, phonoff[i]);
    CHECK((b.regs_mirror[0][3] & 0x0F) == want, "phoneme-off keeps amp (PA gates)");
    ssinger_bus_free(&b);
}

/* Pitch encoding round-trips through R1/R2 for every MIDI note (DUR != 3):
 * reassembling the emitted registers with the engine formula must give
 * back the exact 12-bit inflection, i.e. optimal half-LSB tuning. */
static void test_inflection_exact(void)
{
    ssinger_state_t s;
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
            if (l.addr[i] == SG_SC_R1)
                r1 = l.value[i];
            if (l.addr[i] == SG_SC_R2)
                r2 = l.value[i];
        }
        CHECK(r1 >= 0 && r2 >= 0, "note %d emits R1+R2", note);
        want = ssinger_freq_to_inflection(ssinger_note_freq(note), s.cfg.xck_hz, 64);
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
    ssinger_state_t s;
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
            if (l.addr[i] == SG_SC_R1)
                r1 = l.value[i];
            if (l.addr[i] == SG_SC_R2)
                r2 = l.value[i];
        }
        CHECK(r1 >= 0 && r2 >= 0, "note %d emits R1+R2", note);
        I = r1 * 8 + (((r2 >> 3) & 1) * 2048 + (r2 & 7));
        CHECK(I > 0 && I < 4096, "note %d I in range (%d)", note, I);
        f = ssinger_note_freq(note);
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
    ssinger_state_t s;
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
    CHECK(e4infl == ssinger_freq_to_inflection(ssinger_note_freq(64), s.cfg.xck_hz, 64),
          "latched note sets its inflection");
    CHECK(e4amp == ssinger_vel_to_amp(&s, 110), "latched note sets its amp");
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
    ssinger_state_t s;
    sc_log_t l;
    const uint8_t up[] = { 0xE0, 0x00, 0x60 };   /* +12 st at range 24 */
    const uint8_t center[] = { 0xE0, 0x00, 0x40 };
    const uint8_t pbend[] = { 0xE0, 0x00, 0x60 };
    fw_init(&s, &l);
    s.cfg.ctlmap = SG_MAP_POLAXIS;
    ssinger_set_nominal_xck(&s, 2000000.0);
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

/* A note played while the clock is moved sounds transposed by the move, like
 * a note already held: the translator tunes against its fixed reference
 * clock and the chip's clock does the transposing (F0 = XCK/(8(4096-I))).
 * Regression: the bus copied the moved clock into that reference, so a note
 * played during a held +12 st bend came out un-bent (440 Hz, not 880) and
 * dropped an octave when the bend returned; the Master clock never reached
 * new notes at all. */
static double bus_f0(const ssinger_bus_t *b)
{
    int r1 = b->regs_mirror[0][1], r2 = b->regs_mirror[0][2];
    int I = ((r2 >> 3) & 1) * 2048 + (r2 & 7) + r1 * 8;   /* mode 2 (DUR 2) */
    return b->xck_hz / (8.0 * (4096 - I));
}

static void bus_midi3(ssinger_bus_t *b, int a, int c, int d)
{
    ssinger_bus_midi_byte(b, (uint8_t)a);
    ssinger_bus_midi_byte(b, (uint8_t)c);
    ssinger_bus_midi_byte(b, (uint8_t)d);
}

static double cents(double f, double want)
{
    return 1200.0 * log(f / want) / log(2.0);
}

static void test_new_note_follows_clock(void)
{
    ssinger_bus_t b;
    ssi263_params p;
    ssi263_default_params(&p);
    CHECK(ssinger_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "clock-follow bus init");
    b.fw.cfg.ctlmap = SG_MAP_POLAXIS;
    bus_midi3(&b, 0x90, 44, 100);                  /* a phoneme */
    bus_midi3(&b, 0x91, 69, 100);                  /* A4 */
    CHECK(fabs(cents(bus_f0(&b), 440.0)) < 12.0, "A4 at rest (%f Hz)", bus_f0(&b));
    bus_midi3(&b, 0xE0, 0x00, 0x60);               /* bend +12 st (range 24) */
    CHECK(fabs(cents(bus_f0(&b), 880.0)) < 12.0, "held A4 bent +12 (%f Hz)", bus_f0(&b));
    bus_midi3(&b, 0x81, 69, 0);
    bus_midi3(&b, 0x91, 69, 100);                  /* A4 played during the bend */
    CHECK(fabs(cents(bus_f0(&b), 880.0)) < 12.0, "A4 played during +12 bend (%f Hz)", bus_f0(&b));
    bus_midi3(&b, 0xE0, 0x00, 0x40);               /* bend back to centre */
    CHECK(fabs(cents(bus_f0(&b), 440.0)) < 12.0, "that A4 after the bend returns (%f Hz)", bus_f0(&b));
    ssinger_bus_master_write(2000000.0, &b);       /* Master clock +12 st */
    bus_midi3(&b, 0x81, 69, 0);
    bus_midi3(&b, 0x91, 69, 100);
    CHECK(fabs(cents(bus_f0(&b), 880.0)) < 12.0, "A4 played after Master +12 (%f Hz)", bus_f0(&b));
    CHECK(b.fw.cfg.xck_hz == 1000000.0, "reference clock unmoved (%f)", b.fw.cfg.xck_hz);
    ssinger_bus_free(&b);
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

/* Filter slew: R4 targets latch at once, the chip walks to them (~100 ms
 * full scale) so wheel/bend sweeps glide instead of zippering. */
static void test_filter_slew(void)
{
    ssinger_bus_t b;
    ssi263_params p;
    const uint8_t max[] = { 0xB0, 1, 127 };
    int i;
    ssi263_default_params(&p);
    CHECK(ssinger_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "ffslew init");
    b.fw.cfg.ctlmap = SG_MAP_POLAXIS; /* mod -> filter */
    for (i = 0; i < 3; i++)
        ssinger_bus_midi_byte(&b, max[i]);
    CHECK(b.ff_tgt[0] == 255, "target latches at once (%d)", b.ff_tgt[0]);
    CHECK(b.ff_out[0] == 0xE4, "chip still at start (0x%02X)", b.ff_out[0]);
    CHECK(b.regs_mirror[0][4] == 255, "mirror holds target");
    ssinger_bus_slew_filters(&b, 0.01); /* 10 ms: partway */
    CHECK(b.ff_out[0] > 0xE4 && b.ff_out[0] < 255, "mid-slew (0x%02X)", b.ff_out[0]);
    CHECK(ssi263_reg(b.chip[0], 4) == b.ff_out[0], "chip tracks slew");
    ssinger_bus_slew_filters(&b, 1.0);
    CHECK(b.ff_out[0] == 255, "converges");
    CHECK(ssi263_reg(b.chip[0], 4) == 255, "chip lands");
    ssinger_bus_free(&b);
}

/* ---- host conditions (any DAW, any OS) ------------------------------- */

/* A short sung phrase with a filter sweep (patent map: bend = R4), as
 * timestamped MIDI, the way a DAW hands it over. */
typedef struct {
    long pos; /* sample position at 44.1 kHz; scaled for other rates */
    uint8_t b[3];
} song_ev_t;

static const song_ev_t song[] = {
    { 1000, { 0x91, 57, 100 } },  /* pitch A3 */
    { 1000, { 0x90, 45, 110 } },  /* phoneme */
    { 20000, { 0xE0, 0, 0x10 } }, /* bend down: filter slews */
    { 30000, { 0x90, 50, 110 } },
    { 30500, { 0x80, 45, 0 } },
    { 40000, { 0xE0, 0, 0x70 } }, /* bend up */
    { 45000, { 0x91, 64, 90 } },
    { 60000, { 0x80, 50, 0 } },
    { 61000, { 0x81, 57, 0 } },
};
#define SONG_N ((int)(sizeof(song) / sizeof(song[0])))

/* SSIngerProcessor::processBlock, minus JUCE: per host block, render up to
 * each event, feed its bytes, render the rest -- through ssinger_bus_run. */
static void host_render(ssinger_bus_t *b, int block, double rate_scale,
                        long total, double *out)
{
    long t = 0;
    int e = 0, i;
    while (t < total) {
        long end = t + block, last = t;
        if (end > total)
            end = total;
        while (e < SONG_N && (long)(song[e].pos * rate_scale) < end) {
            long pos = (long)(song[e].pos * rate_scale);
            if (pos > last)
                ssinger_bus_run(b, pos - last, out + last);
            for (i = 0; i < 3; i++)
                ssinger_bus_midi_byte(b, song[e].b[i]);
            last = pos > last ? pos : last;
            e++;
        }
        if (end > last)
            ssinger_bus_run(b, end - last, out + last);
        t = end;
    }
}

/* The same song at any host block size gives the same samples. Regression:
 * the filter slew and A/R service ran at block edges, so a bend at 512
 * samples/block and at 4096 rendered differently (max diff 1.6, the R4
 * glide stepping every 93 ms at 4096). */
static void test_block_size_invariance(void)
{
    static double ref[44100 * 2], got[44100 * 2];
    const int blocks[] = { 1, 7, 64, 1000, 4096, 8192 };
    const long total = 44100 * 2;
    ssinger_bus_t b;
    ssi263_params p;
    int k;
    long i, first;
    double e = 0.0;
    ssi263_default_params(&p);
    CHECK(ssinger_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "blocksize init");
    host_render(&b, 512, 1.0, total, ref);
    CHECK(b.render_cap == 4096, "run never grows the scratch buffer (%ld)", b.render_cap);
    ssinger_bus_free(&b);
    for (i = 0; i < total; i++)
        e += ref[i] * ref[i];
    CHECK(sqrt(e / total) > 0.02, "song sings (rms %f)", sqrt(e / total));
    for (k = 0; k < (int)(sizeof(blocks) / sizeof(blocks[0])); k++) {
        CHECK(ssinger_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "blocksize init %d", blocks[k]);
        host_render(&b, blocks[k], 1.0, total, got);
        ssinger_bus_free(&b);
        first = -1;
        for (i = 0; i < total && first < 0; i++)
            if (got[i] != ref[i])
                first = i;
        CHECK(first < 0, "block %d renders the same as 512 (first diff at sample %ld)", blocks[k], first);
    }
}

/* 44.1 / 48 / 96 kHz: the chip keeps its own time, sings at a like level,
 * and every sample is finite and in range. */
static void test_sample_rates(void)
{
    static double out[96000 * 2];
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    double rms[3];
    int r;
    for (r = 0; r < 3; r++) {
        ssinger_bus_t b;
        ssi263_params p;
        long total = (long)(rates[r] * 2), i, bad = 0;
        double e = 0.0, pk = 0.0;
        ssi263_default_params(&p);
        CHECK(ssinger_bus_init(&b, rates[r], 1, &p, ssi263_default_rom()), "rate %.0f init", rates[r]);
        host_render(&b, 480, rates[r] / 44100.0, total, out);
        for (i = 0; i < total; i++) {
            if (!(out[i] == out[i]) || out[i] > 1e6 || out[i] < -1e6)
                bad++;
            e += out[i] * out[i];
            if (fabs(out[i]) > pk)
                pk = fabs(out[i]);
        }
        rms[r] = sqrt(e / total);
        CHECK(bad == 0, "rate %.0f: all samples finite (%ld bad)", rates[r], bad);
        CHECK(pk < 4.0, "rate %.0f: peak in range (%f)", rates[r], pk);
        CHECK(fabs(ssi263_time(b.chip[0]) - 2.0) < 0.01, "rate %.0f: chip time follows the host (%f s)",
              rates[r], ssi263_time(b.chip[0]));
        ssinger_bus_free(&b);
    }
    CHECK(rms[1] > rms[0] * 0.8 && rms[1] < rms[0] * 1.25, "48k level like 44.1k (%f vs %f)", rms[1], rms[0]);
    CHECK(rms[2] > rms[0] * 0.8 && rms[2] < rms[0] * 1.25, "96k level like 44.1k (%f vs %f)", rms[2], rms[0]);
}

/* No notes: the chip's own idle floor only (measured -79 dBFS peak at the
 * power-on registers, the carrier bleed of test_vst_render_path), never a
 * held tone; and after the last release it falls to that floor or below. */
static void test_idle_floor(void)
{
    static double out[96000];
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    int r;
    for (r = 0; r < 3; r++) {
        ssinger_bus_t b;
        ssi263_params p;
        long n = (long)rates[r], i;
        double pk = 0.0;
        ssi263_default_params(&p);
        CHECK(ssinger_bus_init(&b, rates[r], 1, &p, ssi263_default_rom()), "idle init");
        ssinger_bus_run(&b, n, out);
        for (i = 0; i < n; i++)
            if (fabs(out[i]) > pk)
                pk = fabs(out[i]);
        CHECK(pk < 3.2e-4, "rate %.0f: idle under -70 dBFS (peak %g)", rates[r], pk);
        ssinger_bus_free(&b);
    }
    {
        static double song_out[44100 * 3];
        ssinger_bus_t b;
        ssi263_params p;
        long total = 44100 * 3, i;
        double pk = 0.0;
        ssi263_default_params(&p);
        CHECK(ssinger_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "release init");
        host_render(&b, 512, 1.0, total, song_out);
        for (i = 44100 * 2; i < total; i++)
            if (fabs(song_out[i]) > pk)
                pk = fabs(song_out[i]);
        CHECK(pk < 3.2e-4, "a second after the last release: under -70 dBFS (peak %g)", pk);
        ssinger_bus_free(&b);
    }
}

/* Phoneme channel likewise falls back across overlapping notes. */
static void test_phoneme_mono_priority(void)
{
    ssinger_state_t s;
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
    CHECK(s.v[0].phoneme == SG_PHONEME_PAUSE, "last release writes PA");
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
    test_new_note_follows_clock();
    test_filter_slew();
    test_vst_render_path();
    test_block_size_invariance();
    test_sample_rates();
    test_idle_floor();
    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

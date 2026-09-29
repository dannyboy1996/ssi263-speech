/* test_6850_midi.c -- offline checks for the Robovox emulator cores.
 *
 * 1. MC6850: reset/control programming, RDRF/TDRE status, Rx IRQ line,
 *    overrun + framing flags, transmit path.
 * 2. Translator vectors: patent embodiment-1 mapping (phoneme ch N,
 *    pitch ch N+1 at A=440, wheels, CC2/CC3/CC64, program-change
 *    embodiment 2, running status/sysex robustness).
 * 3. Bus: MIDI wire byte -> ACIA IRQ -> translator -> SC-02 latch, CPU
 *    socket map reads/writes, A/R sustain service.
 * 4. Engine smoke: real ssi263 chip speaks through the bus path.
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
    /* Note 60 -> table code 25 (0x19 UH1); DUR=2 in high bits. */
    CHECK(r0 == ((2 << 6) | 25), "phoneme R0=0x%02X", r0);
    CHECK(r3 == ((5 << 4) | (1 + (100 * 14) / 127)), "amp R3=0x%02X", r3);
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
    r3 = -1;
    for (i = 0; i < l.n; i++)
        if (l.addr[i] == RV_SC_R3)
            r3 = l.value[i];
    CHECK((r3 & 0x0F) == 0, "pitch-off zeroes amplitude, R3=0x%02X", r3);
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
    CHECK(s.v[0].note_held == 0, "note-off releases");
    CHECK(s.v[0].phoneme == RV_PHONEME_PAUSE, "note-off writes PA");
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

/* Mirrors RobovoxProcessor::renderChunk order (event, render, slew,
 * service). Regression: with attack/release at 0 the slew must not gate
 * the voice (amp_tgt sync). */
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
        robovox_bus_slew(&b, 512 / 44100.0);
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
    /* Envelope shapes when engaged: attack ramps up, release falls. */
    CHECK(robovox_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "env init");
    b.attack_ms = 200.0;
    b.release_ms = 200.0;
    for (i = 0; i < 3; i++)
        robovox_bus_midi_byte(&b, pon[i]);
    {
        const uint8_t non127[] = { 0x90, 60, 127 };
        for (i = 0; i < 3; i++)
            robovox_bus_midi_byte(&b, non127[i]);
    }
    efirst = 0.0;
    robovox_bus_render(&b, 512, out);
    robovox_bus_slew(&b, 512 / 44100.0);
    robovox_bus_service_all(&b);
    for (i = 0; i < 512; i++)
        efirst += out[i] * out[i];
    CHECK((b.regs_mirror[0][3] & 0x0F) < 15, "attack starts low (amp %d)",
          b.regs_mirror[0][3] & 0x0F);
    e = 0.0;
    for (k = 0; k < 43; k++) {
        robovox_bus_render(&b, 512, out);
        robovox_bus_slew(&b, 512 / 44100.0);
        robovox_bus_service_all(&b);
        for (i = 0; i < 512; i++)
            e += out[i] * out[i];
    }
    CHECK((b.regs_mirror[0][3] & 0x0F) == 15, "attack converges (amp %d)",
          b.regs_mirror[0][3] & 0x0F);
    CHECK(sqrt(e / (43.0 * 512)) > sqrt(efirst / 512), "attack ramps up");
    {
        const uint8_t poff[] = { 0x81, 45, 0 };
        for (i = 0; i < 3; i++)
            robovox_bus_midi_byte(&b, poff[i]);
    }
    for (k = 0; k < 43; k++) {
        robovox_bus_render(&b, 512, out);
        robovox_bus_slew(&b, 512 / 44100.0);
        robovox_bus_service_all(&b);
    }
    CHECK((b.regs_mirror[0][3] & 0x0F) == 0, "release falls (amp %d)",
          b.regs_mirror[0][3] & 0x0F);
    robovox_bus_free(&b);
}

static void test_amp_slew(void)
{
    robovox_bus_t b;
    ssi263_params p;
    const uint8_t on[] = { 0x90, 48, 127 };
    int i;
    ssi263_default_params(&p);
    CHECK(robovox_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "slew bus init");
    b.attack_ms = 100.0;
    b.release_ms = 100.0;
    for (i = 0; i < 3; i++)
        robovox_bus_midi_byte(&b, on[i]);
    CHECK((b.regs_mirror[0][3] & 0x0F) == 0, "attack starts at zero");
    CHECK(b.amp_tgt[0] == 15, "target latched (%d)", b.amp_tgt[0]);
    robovox_bus_slew(&b, 0.05); /* half the attack */
    CHECK((b.regs_mirror[0][3] & 0x0F) >= 7 && (b.regs_mirror[0][3] & 0x0F) <= 8,
          "half attack ~7-8 (got %d)", b.regs_mirror[0][3] & 0x0F);
    robovox_bus_slew(&b, 0.10);
    CHECK((b.regs_mirror[0][3] & 0x0F) == 15, "attack completes");
    robovox_bus_free(&b);
    /* Zero attack/release = patent-exact direct write. */
    CHECK(robovox_bus_init(&b, 44100.0, 1, &p, ssi263_default_rom()), "direct bus init");
    for (i = 0; i < 3; i++)
        robovox_bus_midi_byte(&b, on[i]);
    CHECK((b.regs_mirror[0][3] & 0x0F) == 15, "direct write lands at once");
    robovox_bus_free(&b);
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
    test_amp_slew();
    test_vst_render_path();
    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

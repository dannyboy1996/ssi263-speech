/* bl_board.h -- the Blazie Braille Lite 2000 board around a Z180 core, as a library: one instance per unit.
 *
 * The live path of z180emu/bns.c (the investigation tool, which keeps its tracing), per instance, so a program can
 * run an English and a Spanish unit side by side.  The unit's SSI-263 writes and serial bytes come back as events,
 * in order, per call; the host (the SSI-263 model and the lockstep) applies them.  Behaviour is bns.c's --live mode
 * line for line: nvda/tools/golden/blazie_*.txt gates it.
 *
 * Not thread-safe per instance; different instances may be used from different threads.
 */
#ifndef BL_BOARD_H
#define BL_BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bl_unit bl_unit;

typedef struct {
    unsigned char type;          /* 'W': an SSI-263 write (a = register 0-4, b = value); 'T': a serial byte (a) */
    unsigned char a, b;
} bl_event;

/* firmware: a .BNS update file (the ROM image from file offset 3000h); state: battery-backed RAM (256 KB) + the file
   flash (512 KB), as make_states.sh saves them; phon_ms: the stand-in A/R timing before live mode (bns --phon-ms);
   keys: braille chords pressed at those instruction counts during the boot (bns --key INSTR=CHORD).
   NULL on failure, with a reason in err. */
bl_unit *bl_create(const char *firmware, const char *state, double phon_ms,
                   const unsigned long long *key_at, const unsigned char *key_val, int n_keys,
                   char *err, int errlen);
void bl_destroy(bl_unit *u);

void bl_boot(bl_unit *u, unsigned long long target_instr);   /* bns 'B': run until that instruction count */
void bl_live(bl_unit *u);                                    /* bns 'LIVE': the host drives A/R from now on */
void bl_run(bl_unit *u, unsigned long long cycles);          /* bns 'R': run that many CPU cycles */
void bl_set_ar(bl_unit *u, int requesting);                  /* bns 'A' */
void bl_queue(bl_unit *u, const unsigned char *bytes, int n);/* bns 'S': bytes for the unit's serial input */
void bl_urgent(bl_unit *u, int byte);                        /* bns 'U': one byte delivered even under XOFF (^X) */
int  bl_drop(bl_unit *u);                                    /* bns 'D': drop queued input; the ^F markers dropped */
unsigned long long bl_cycles(const bl_unit *u);              /* CPU cycles so far */
/* a braille key chord pressed live (port 40h: dot 1 = bit 0 .. dot 6 = bit 5, space = bit 6, bit 7 = an advance
   key): latched with /INT2 at the next boundary after the last chord was read; 0 if 16 are already waiting */
int  bl_key(bl_unit *u, int chord);
/* the battery-backed RAM + file flash, in bl_create's state format (what a real unit keeps while switched off);
   1 on success */
int  bl_save_state(const bl_unit *u, const char *path);

/* the events since the last bl_clear_events, in order */
int  bl_events(const bl_unit *u, const bl_event **events);
void bl_clear_events(bl_unit *u);

#ifdef __cplusplus
}
#endif
#endif

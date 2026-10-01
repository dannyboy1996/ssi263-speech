/* tns_rescue.h -- a saved Type 'n Speak that was never set up, told apart and set up anew (the Type 'n Speak's real
 * cold reset; Timothy, Jayson).
 *
 * In the 0.6 and 0.7 previews the emulator's first start of a Type 'n Speak missed the unit's cold reset
 * (tns_board.c): the unit came up with its flash set up but no RAM file system and no folders.  On such a unit a new file's first character
 * was never stored (the first file went into the program's last byte) and a file moved to flash was lost: an entry
 * in folder 0, the deleted mark, and its text never written (bl_files.h blf_lost_get).  A unit whose file system or
 * folder question was answered n (tns_setup.h) is the same.  The states saved by the 0.6 and 0.7 previews are such
 * units.
 *
 * Portable C99: the shells (main_win.c, main_linux.c) ask the person, then call tns_rescue.
 */
#ifndef BLAZIE_TNS_RESCUE_H
#define BLAZIE_TNS_RESCUE_H

typedef struct {
    int ram_files;                  /* the RAM files found (the unit's clipboard and datebook not counted) */
    int lost_flash;                 /* flash files the unit lost (bl_files.h blf_lost_get) */
    int carried;                    /* files carried into the unit set up anew */
    int first_lost;                 /* of them, files whose first byte the old unit never stored (dropped) */
    int unrecoverable;              /* lost flash files whose text the old unit never wrote: named in the log only */
    char log[2048];                 /* one line per file: carried, or why not */
} tns_rescue_report;

/* 1: the saved state at `path` is a Type 'n Speak that was never set up (no RAM file system, or no folders); its
   files counted into r (may be NULL).  0: set up, or not a Type 'n Speak.  -1: unreadable. */
int tns_needs_setup(const char *path, tns_rescue_report *r);

/* The unit set up anew -- its factory setup, its own questions answered (tns_setup.h) -- and, with keep_files, the
   old state's files carried in: RAM files into the RAM startup folder (a first byte the old unit never stored
   dropped), flash files into the flash startup folder (a lost one only if its text is there; else it is named in
   the log), each with its name (a digit added if two share one), time, date and protection.  The unit's settings
   are the factory's.  Saved over `path`, the old state
   kept as `path`.before-setup.  1, or 0 with the reason in err (`path` then as it was). */
int tns_rescue(const char *firmware, const char *path, int keep_files, tns_rescue_report *r, char *err, int errlen);

#endif

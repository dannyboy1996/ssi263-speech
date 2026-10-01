/* tns_setup.h -- the Type 'n Speak's first start: its cold reset and the questions it asks (the Type 'n Speak's real
 * cold reset; Timothy, Jayson).
 *
 * A Type 'n Speak started with no saved memory starts as Ctrl+Alt+Del held at power-on (tns_board.c): the unit's own
 * cold reset.  It asks, each answered y (the Spanish unit: s) -- measured on the running firmware, both languages:
 *
 *   1 "initialize file system?"            2 "are you sure?"       ("system initialized")
 *   3 "initialize flash system?"           4 "are you sure?"       ("please wait": the flash's 32 s erase, clicking)
 *   5 "initialize folder system?"                                   ("Type 'n Speak ready", the date, "help is open")
 *   6 "delete all data in file area."      7 "are you sure?"       (the wipe: ~35 s, silent, keys ignored; then
 *                                                                    "system initialized" and the unit starts again:
 *                                                                    "Type 'n Speak ready", "help is open")
 *
 * Any other key asks the same question again.  n to 1 or to 5 leaves the unit unable to keep its files (no RAM file
 * system, or no folders: a file moved to flash is then lost); n to 3 makes it ask about the flash again; n to 6 (the
 * memory is blank on a new unit anyway) leaves the file area as it is.  tns_rescue.h tells such a unit apart.
 *
 * Headless, for the tests and tns_rescue.c: the answers' times in the unit's own time, and a whole factory setup.
 * Portable C99.
 */
#ifndef BLAZIE_TNS_SETUP_H
#define BLAZIE_TNS_SETUP_H

#include "emu_unit.h"

#define TNS_SETUP_ANSWERS 7

/* what the shells tell the person before a first start, and in their key help */
#define TNS_FIRST_START \
    "The first time (and after Back to the factory state), the Type 'n Speak starts as a new unit: its own cold " \
    "reset asks how to set itself up. Press y for each question (the Spanish unit: s), seven times in all:\n" \
    "  initialize file system? y. Are you sure? y.\n" \
    "  initialize flash system? y. Are you sure? y. Then about 30 seconds of clicks while it erases the flash.\n" \
    "  initialize folder system? y. It says it is ready and opens its help.\n" \
    "  delete all data in file area? y. Are you sure? y. Then about 35 seconds of silence while it clears its " \
    "memory, and it starts again: ready.\n" \
    "Answered n, the file system or the folders are not made and the unit cannot keep files: the emulator then " \
    "offers to set it up the next time it starts."

/* answer k's time (0..6) in seconds of the unit's time from its start, each a little after the question is asked
   (a key while the unit asks is its answer); flash_timed: with the flash erase's own 32 s (emu_set_flash_timed),
   which comes after answer 4 */
double tns_setup_answer_at(int k, int flash_timed);
/* by then every answer is in, the unit set up and quiet in its main menu */
double tns_setup_ready_at(int flash_timed);
/* the unit's yes: 's' for the Spanish firmware (by its file name, TNSSPA), else 'y'; its key code (tns_keys.h) */
int tns_setup_yes(const char *firmware);
int tns_setup_yes_code(int yes);

/* A factory start, its questions answered yes, the flash erased at once, saved to out_state: the unit as it left
   the factory.  1, or 0 with the reason in err.  It does not check the result (tns_rescue.h does). */
int tns_factory_setup(const char *firmware, const char *out_state, char *err, int errlen);

#endif

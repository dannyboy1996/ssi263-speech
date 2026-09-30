/* bl_unity.c -- the Braille Lite board and its Z180 as one translation unit.
 *
 * The board (bl_board.c) drives the CPU only through ../cpu/cpu.h.  The Z180 behind it is z180emu's core on the
 * legacy path (../cpu/z180_legacy.c, GPL-2.0-or-later), which includes z180emu's sources itself.  The board goes
 * FIRST: z180.c's register shortcuts (_B, _C, _DE, ...) are macros that would rename identifiers in files after it,
 * and the adapter uses them on purpose after z180.c.
 *
 * Build with -I<z180emu> -I<z180emu>/z180; add bl_live.c for the pipe-protocol program.
 *
 * The core's diagnostics (logerror = printf in z80common.h, e.g. "TRDR rd") go nowhere: the adapter silences them,
 * since a library must never write to a stdout that may be a protocol pipe.
 *
 * The board's idle-channel sounds (bl_idle.c, which bl_host.c drives for the emulator) come along here, so every build
 * that links the board has them; before the core, like the board.
 */
#include "bl_board.c"
#include "flash29.c"
#include "bl_idle.c"
#include "../cpu/z180_legacy.c"

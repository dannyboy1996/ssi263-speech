/* bl_unity.c -- the Braille Lite board and the Z180 core as one translation unit (as z180emu's bns_unity.c): the
 * compiler inlines the per-instruction hook into the core's execute loop.  z180.c goes last: its register shortcuts
 * (_B, _C, _DE, ...) are macros that would rename identifiers in the files after it.
 *
 * Build with -I<z180emu> -I<z180emu>/z180; add bl_live.c for the pipe-protocol program.
 *
 * The core's diagnostics (logerror = printf in z80common.h, e.g. "TRDR rd") go nowhere here: a library must never
 * write to a stdout that may be a protocol pipe.  The header's include guard keeps the later includes from
 * redefining it.
 */
#include "z180/z80common.h"
#undef logerror
#define logerror(...) ((void)0)
#include "z180/z180dasm.c"
#include "z180/z80daisy.c"
#include "z180/z80scc.c"
#include "z180/z180asci.c"
#include "bl_board.c"
#include "z180/z180.c"

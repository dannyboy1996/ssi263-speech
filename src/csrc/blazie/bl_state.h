/* bl_state.h -- a Braille Lite's battery-backed state (bl_create's `state`: RAM + file flash), made from its firmware
 * alone, on the device that will speak with it.
 *
 * The state is what the unit wrote while it was powered on and keyed: it holds the firmware's own words and tables,
 * so it can ship only where the firmware ships.  An app that makes its users bring the firmware (Android) makes the
 * state here instead: the unit is powered on and keyed exactly as z180emu's bns.c was when the shipped states were
 * made (make_states.sh and the Spanish unit's reset), and the result is the shipped state byte for byte --
 * src/platforms/android/test/test_import_native.py checks the sha256 and the speech.
 */
#ifndef BL_STATE_H
#define BL_STATE_H

#include "bl_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLV_STATE_ENGLISH 0        /* the June 2003 BL2ENG.BNS -> bl2_2003_warm.state; also ONCE's September 2000
                                      English (bl_firmware.c's list holds each release's state hash); and the
                                      Braille 'n Speak 2000's English and Slovak (BS03ENG.BNS, BS2SLL.BNS: the
                                      emulator's factory states), whose prompts come at the same points and take
                                      the same keys (bl_state.c) */
#define BLV_STATE_SPANISH 1        /* ONCE's BL2SPA.BNS -> bl2spa_fresh.state */

/* Called between runs with the share done so far (0..1); a nonzero return abandons the state. */
typedef int (*blv_state_progress)(void *ctx, double done);

/* Powers the unit on with `firmware` (a .BNS) and no state, keys it through the language's recipe and writes the
   state to out_path (which also carries it between the recipe's power-ons).  1 on success; 0 with the reason in err
   (and out_path removed).  English takes about 150 million Z180 instructions, Spanish 1150 million. */
BL_API int blv_make_state(const char *firmware, int language, const char *out_path, blv_state_progress progress,
                          void *ctx, char *err, int errlen);

/* The test's control (test_import_native.py sets it; an app never does): nonzero holds the wrong chord (7Eh) at the
   English warm reset, so the state -- and the speech -- must differ. */
BL_API extern int blv_state_break;

#ifdef __cplusplus
}
#endif
#endif

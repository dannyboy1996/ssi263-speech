/* bl_numbers.h -- the Braille Lite driver's "Custom number processing" (nvda/blazie/synthDrivers/blazie.py _numbers and
 * _money, the driver's default: on) in C, for bl_voice's blv_set_numbers.
 *
 * English: numbers as words (numwords.c's nw_normalise), except dollar amounts, which go to the firmware as they are
 * up to $999,999,999,999 (it says "$25" itself, and drops a "$" in front of words); from a trillion up those become
 * words here.  Spanish: Spain's convention, "1.234.567" one number and "3,5" tres coma cinco (numwords_es.c).
 *
 * It lives apart from bl_voice.c so the voice still links without the number words (bl.dll, the add-on's library,
 * has none of them): a front end that wants them links this file, numwords.c and numwords_es.c and passes bl_numbers
 * to blv_set_numbers.  nvda/tools/voice_text_equiv.py --numbers holds it to the driver byte for byte.  MIT.
 */
#ifndef BL_NUMBERS_H
#define BL_NUMBERS_H

#include "bl_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* _numbers(text, lang): text is the voice's cleaned text (UTF-8; 7-bit for the English unit), encoding the voice's
   (bl_voice.h BLV_LATIN1 = English, BLV_CP850 = Spanish).  A malloc'd UTF-8 string, NULL when out of memory. */
BL_API char *bl_numbers(const char *utf8, int encoding);

#ifdef __cplusplus
}
#endif
#endif

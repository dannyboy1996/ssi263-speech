/* numwords_es.h -- nvda/shared/ssi263_numwords.py's normalise(text, lang="es", decimal_comma=True) in C: numbers as
 * Spanish words the way the Braille Lite's Spanish unit (ONCE's firmware) is fed them -- "1.234.567" one number,
 * "3,5" tres coma cinco, the long scale (mil millones, billón), apocope before a scale word (veintiún mil), an English
 * ordinal suffix as the bare cardinal.  On code points, since the Spanish text keeps its accented letters (Python's
 * \w is Unicode: "café3" is one word).  Each function names the Python it ports; change them together.
 *
 * The Braille Lite voice uses it (blazie/bl_numbers.c, the driver's _numbers for the Spanish unit);
 * nvda/tools/voice_text_equiv.py --numbers holds it to the driver byte for byte on random texts.  MIT.
 */
#ifndef SSI263_NUMWORDS_ES_H
#define SSI263_NUMWORDS_ES_H

#ifdef __cplusplus
extern "C" {
#endif

/* normalise(text, lang="es", decimal_comma=True) on n code points: a malloc'd UTF-8 string (the number words' accents
   included), NULL when out of memory. */
char *nw_normalise_es(const unsigned *t, int n);

#ifdef __cplusplus
}
#endif
#endif

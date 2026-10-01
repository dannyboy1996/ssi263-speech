/* tns_setup.c -- see tns_setup.h.  The times were measured on both languages' firmware (September 2000): each
 * question is asked within 2 s of the answer before it, except question 6, which comes ~4 s after answer 5 (the unit
 * starts, says it is ready and opens its help first), and question 5, which waits for the flash's erase.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tns_keys.h"
#include "tns_setup.h"

#define RATE 22050
#define BLOCK (RATE / 100)

static const double AT[TNS_SETUP_ANSWERS] = {4.0, 7.0, 10.0, 13.0, 18.0, 25.0, 28.0};
#define ERASE_S 32.0                /* the 29F016's chip erase (flash29.h), after answer 4 */

double tns_setup_answer_at(int k, int flash_timed)
{
    if (k < 0 || k >= TNS_SETUP_ANSWERS)
        return -1.0;
    return AT[k] + (flash_timed && k >= 4 ? ERASE_S : 0.0);
}

/* after the last answer the unit wipes its file area, keys ignored, for 34.6 s of its time (both languages), says
   "system initialized", starts again ("Type 'n Speak ready", the date, "help is open") and is quiet by 37 s */
#define WIPE_S 40.0

double tns_setup_ready_at(int flash_timed)
{
    return tns_setup_answer_at(TNS_SETUP_ANSWERS - 1, flash_timed) + WIPE_S;
}

/* the Spanish firmware asks "pulsar s, o n." where the English asks "enter y or n" */
int tns_setup_yes(const char *firmware)
{
    static const char spanish[] = "pulsar s, o n";
    FILE *f = fopen(firmware, "rb");
    unsigned char *d;
    long n, i;
    int yes = 'y';
    if (!f)
        return yes;
    d = (unsigned char *)malloc(0x100000);
    n = d ? (long)fread(d, 1, 0x100000, f) : 0;
    fclose(f);
    for (i = 0; i + (long)sizeof spanish - 1 <= n; i++)
        if (d[i] == 'p' && !memcmp(d + i, spanish, sizeof spanish - 1)) {
            yes = 's';
            break;
        }
    free(d);
    return yes;
}

int tns_setup_yes_code(int yes)
{
    char name[2] = {(char)yes, 0};
    return tns_code_named(name);
}

int tns_factory_setup(const char *firmware, const char *out_state, char *err, int errlen)
{
    short buf[BLOCK];
    int code = tns_setup_yes_code(tns_setup_yes(firmware)), k = 0, i, n = (int)(tns_setup_ready_at(0) * 100);
    emu_unit *u = emu_create(EMU_TYPE_N_SPEAK, firmware, NULL, RATE, 0, err, errlen);
    if (!u)
        return 0;
    emu_set_flash_timed(u, 0);
    for (i = 0; i < n; i++) {
        if (k < TNS_SETUP_ANSWERS && i >= (int)(tns_setup_answer_at(k, 0) * 100)) {
            emu_key(u, code);               /* down, then up */
            emu_key(u, code & 0x7F);
            k++;
        }
        emu_render(u, buf, BLOCK);
    }
    if (!emu_save(u, out_state)) {
        snprintf(err, (size_t)errlen, "could not save %s", out_state);
        emu_destroy(u);
        return 0;
    }
    emu_destroy(u);
    return 1;
}

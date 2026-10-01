/* make_state.c -- a unit's factory state from its firmware alone, as the emulator starts it the first time.
 *
 *   make_state FIRMWARE english|spanish OUT
 *
 * The unit is powered on and keyed through the recipe (../../csrc/blazie/bl_state.h: english is the three power-ons
 * that made bl2_2003_warm.state, and fits the Braille 'n Speak 2000's English and Slovak; spanish the Spanish
 * Braille Lite's reset), and its memory written to OUT.  Then it says which unit the firmware is for and the state's
 * sha256.  The Braille 'n Speak 2000's states, kept beside its firmware (firmware/blazie/bns2000/, never in git):
 *
 *   make_state firmware/blazie/bns2000/BS03ENG.BNS english firmware/blazie/bns2000/bs03eng_fresh.state
 *   make_state firmware/blazie/bns2000/BS2SLL.BNS english firmware/blazie/bns2000/bs2sll_fresh.state
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../csrc/blazie/bl_board.h"
#include "../../csrc/blazie/bl_firmware.h"
#include "../../csrc/blazie/bl_state.h"

int main(int argc, char **argv)
{
    char err[256];
    int language, model;
    bl_unit *u;
    if (argc != 4 || (strcmp(argv[2], "english") && strcmp(argv[2], "spanish"))) {
        fprintf(stderr, "usage: make_state FIRMWARE english|spanish OUT\n");
        return 2;
    }
    language = strcmp(argv[2], "spanish") ? BLV_STATE_ENGLISH : BLV_STATE_SPANISH;
    u = bl_create(argv[1], NULL, 20.0, NULL, NULL, 0, err, sizeof err);
    if (!u) {
        fprintf(stderr, "make_state: %s\n", err);
        return 1;
    }
    model = bl_model(u);
    bl_destroy(u);
    if (!blv_make_state(argv[1], language, argv[3], NULL, NULL, err, sizeof err)) {
        fprintf(stderr, "make_state: %s\n", err);
        return 1;
    }
    {
        FILE *f = fopen(argv[3], "rb");
        unsigned char *data, d[32];
        long n = -1;
        int k;
        if (f && fseek(f, 0, SEEK_END) == 0)
            n = ftell(f);
        data = n > 0 ? (unsigned char *)malloc((size_t)n) : NULL;
        if (!f || !data || fseek(f, 0, SEEK_SET) != 0 || fread(data, 1, (size_t)n, f) != (size_t)n) {
            fprintf(stderr, "make_state: cannot read back %s\n", argv[3]);
            return 1;
        }
        fclose(f);
        blv_sha256(data, n, d);
        free(data);
        printf("%s: a %s state, %ld bytes, sha256 ", argv[3], model == BL_MODEL_BNS2000 ? "Braille 'n Speak 2000"
               : "Braille Lite 2000", n);
        for (k = 0; k < 32; k++)
            printf("%02x", d[k]);
        printf("\n");
    }
    return 0;
}

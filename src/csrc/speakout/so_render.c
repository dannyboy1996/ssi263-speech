/* so_render.c -- the Speak-Out reads a text into a WAV file, with no Python: the firmware from its path, the chip
 * from the built-in defaults (so_voice.h).  A demonstration of the C API and a smoke test.
 *
 *   so_render <SPEAKOUT.HEX> "text" out.wav [sample rate, default 22050]
 *
 * As the NVDA driver: boot, the default settings, the text, then 30 ms blocks until the box is done.  MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "so_voice.h"

static void le(FILE *f, unsigned long v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        fputc((int)(v >> (8 * i)) & 0xFF, f);
}

int main(int argc, char **argv)
{
    char err[256];
    double rate = argc > 4 ? atof(argv[4]) : 22050.0;
    so_voice *v;
    short *all = NULL;
    const short *pcm;
    long n_all = 0, cap = 0, i;
    int n, done = 0, blocks = 0;
    FILE *f;
    if (argc < 4) {
        fprintf(stderr, "usage: so_render <SPEAKOUT.HEX> \"text\" out.wav [rate]\n");
        return 2;
    }
    v = sov_create(argv[1], rate, err, (int)sizeof err);
    if (!v) {
        fprintf(stderr, "so_render: %s\n", err);
        return 1;
    }
    sov_speak(v, argv[2], 0);
    while (!done && blocks < 20 * 120) {
        n = sov_render(v, &pcm, &done);
        if (n_all + n > cap) {
            short *q;
            cap = (n_all + n) * 2 + 4096;
            q = (short *)realloc(all, (size_t)cap * sizeof(short));
            if (!q)
                return 1;
            all = q;
        }
        memcpy(all + n_all, pcm, (size_t)n * sizeof(short));
        n_all += n;
        blocks++;
    }
    f = fopen(argv[3], "wb");
    if (!f) {
        fprintf(stderr, "so_render: cannot write %s\n", argv[3]);
        return 1;
    }
    fwrite("RIFF", 1, 4, f);
    le(f, 36 + 2 * (unsigned long)n_all, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    le(f, 16, 4);
    le(f, 1, 2);
    le(f, 1, 2);
    le(f, (unsigned long)rate, 4);
    le(f, 2 * (unsigned long)rate, 4);
    le(f, 2, 2);
    le(f, 16, 2);
    fwrite("data", 1, 4, f);
    le(f, 2 * (unsigned long)n_all, 4);
    for (i = 0; i < n_all; i++)
        le(f, (unsigned short)all[i], 2);
    fclose(f);
    printf("%.2f s of audio in %s%s\n", n_all / rate, argv[3], done ? "" : " (NOT done)");
    free(all);
    sov_destroy(v);
    return done ? 0 : 1;
}

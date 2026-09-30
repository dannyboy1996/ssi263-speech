/* chords.c -- see chords.h. */
#include "chords.h"

void chord_reset(chord_state *s)
{
    s->down = s->seen = 0;
}

void chord_down(chord_state *s, int bit)
{
    s->down |= bit;
    s->seen |= bit;
}

int chord_up(chord_state *s, int bit)
{
    int chord;
    if (!(s->down & bit))
        return 0;                  /* an up for a key we never saw go down (focus came back mid-press) */
    s->down &= ~bit;
    if (s->down)
        return 0;
    chord = s->seen;
    s->seen = 0;
    return chord;
}

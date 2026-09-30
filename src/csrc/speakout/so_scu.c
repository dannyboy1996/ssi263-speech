/* so_scu.c -- the V40's serial receiver, reduced as the approved host reduces it (so_scu.h).  MIT. */
#include "so_scu.h"

#include <stdlib.h>
#include <string.h>

void so_scu_init(so_scu *s)
{
    memset(s, 0, sizeof *s);
    s->loaded = -1;
}

void so_scu_free(so_scu *s)
{
    free(s->q);
    so_scu_init(s);
}

int so_scu_queue(so_scu *s, const uint8_t *bytes, int n)
{
    if (n <= 0)
        return 1;
    if (s->head > 0 && s->tail + n > s->cap) {     /* slide the unread bytes down first */
        memmove(s->q, s->q + s->head, (size_t)(s->tail - s->head));
        s->tail -= s->head;
        s->head = 0;
    }
    if (s->tail + n > s->cap) {
        int cap = s->cap ? s->cap : 256;
        uint8_t *q;
        while (cap < s->tail + n)
            cap *= 2;
        q = (uint8_t *)realloc(s->q, (size_t)cap);
        if (!q)
            return 0;
        s->q = q;
        s->cap = cap;
    }
    memcpy(s->q + s->tail, bytes, (size_t)n);
    s->tail += n;
    return 1;
}

void so_scu_drop(so_scu *s)
{
    s->head = s->tail = 0;
    s->loaded = -1;
}

int so_scu_pending(const so_scu *s)
{
    return s->loaded >= 0 || s->tail > s->head;
}

static int pop(so_scu *s)
{
    return s->tail > s->head ? s->q[s->head++] : -1;
}

int so_scu_load(so_scu *s)
{
    if (s->loaded < 0)
        s->loaded = pop(s);
    return s->loaded >= 0;
}

uint8_t so_scu_in(so_scu *s, uint16_t port)
{
    if (port == 1)
        return (uint8_t)(0x01 | (s->loaded >= 0 ? 0x02 : 0));
    {                                              /* port 0 */
        uint8_t v = (uint8_t)(s->loaded >= 0 ? s->loaded : 0);
        s->loaded = pop(s);
        return v;
    }
}

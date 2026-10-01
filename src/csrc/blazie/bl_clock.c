/* bl_clock.c -- the Blazie units' clock controller (see bl_clock.h). */
#include <string.h>
#include "bl_clock.h"

#define GRAIN 6144ULL               /* cycles between catch-ups at the step boundary (1 ms at 6.144 MHz) */
#define OFF_MAX_S (366.0 * 86400.0)

int blc_break;

static int leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in(int month, int year)
{
    static const unsigned char d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 31;
    return month == 2 && leap(year) ? 29 : d[month - 1];
}

static int jan1(int y)               /* the weekday of 1 January (0 = Sunday) */
{
    int p = y - 1;
    return (1 + 5 * (p % 4) + 4 * (p % 100) + 6 * (p % 400)) % 7;
}

int blc_year5(int year)
{
    int y;
    if (year >= BLC_YEAR0 && year < BLC_YEAR0 + BLC_YEARS)
        return year - BLC_YEAR0;
    for (y = BLC_YEAR0 + BLC_YEARS - 1; y >= BLC_YEAR0; y--)
        if (jan1(y) == jan1(year) && leap(y) == leap(year))
            return y - BLC_YEAR0;
    return ((year - BLC_YEAR0) % BLC_YEARS + BLC_YEARS) % BLC_YEARS;
}

static unsigned long long minute_cycles(const blc_clock *c)
{
    return (unsigned long long)(60.0 * c->hz);
}

void blc_init(blc_clock *c, double hz, unsigned long long now)
{
    memset(c, 0, sizeof *c);
    c->hz = hz;
    c->last = now;
    c->next_tx = now;
}

void blc_set(blc_clock *c, const blc_time *t)
{
    unsigned char *f = c->f[BLC_CLOCK];
    f[BLC_YEAR] = (unsigned char)blc_year5(t->year);
    f[BLC_MONTH] = (unsigned char)t->month;
    f[BLC_DAY] = (unsigned char)t->day;
    f[BLC_HOUR] = (unsigned char)t->hour;
    f[BLC_MINUTE] = (unsigned char)t->minute;
    c->sub = (unsigned long long)(t->second * c->hz);
}

void blc_get(const blc_clock *c, int which, blc_time *t)
{
    const unsigned char *f = c->f[which ? BLC_ALARM : BLC_CLOCK];
    t->year = BLC_YEAR0 + f[BLC_YEAR];
    t->month = f[BLC_MONTH];
    t->day = f[BLC_DAY];
    t->hour = f[BLC_HOUR];
    t->minute = f[BLC_MINUTE];
    t->second = which ? 0 : (int)(c->sub / (unsigned long long)c->hz);
}

static void send(blc_clock *c, int byte)
{
    if (c->n_out < (int)sizeof c->out)
        c->out[c->n_out++] = (unsigned char)byte;
}

static int alarm_due(const blc_clock *c)
{
    const unsigned char *a = c->f[BLC_ALARM], *k = c->f[BLC_CLOCK];
    if (a[BLC_YEAR] == 0)
        return 0;                                    /* "no alarm set" */
    return (a[BLC_MINUTE] == 63 || a[BLC_MINUTE] == k[BLC_MINUTE])
        && (a[BLC_HOUR] == 31 || a[BLC_HOUR] == k[BLC_HOUR])
        && (a[BLC_DAY] == 0 || a[BLC_DAY] == k[BLC_DAY])
        && (a[BLC_MONTH] == 0 || a[BLC_MONTH] == k[BLC_MONTH])
        && (a[BLC_YEAR] == 2000 - BLC_YEAR0 || a[BLC_YEAR] == k[BLC_YEAR]);
}

static void next_minute(blc_clock *c, int alarms)
{
    unsigned char *f = c->f[BLC_CLOCK];
    if (++f[BLC_MINUTE] >= 60) {
        f[BLC_MINUTE] = 0;
        if (++f[BLC_HOUR] >= 24) {
            f[BLC_HOUR] = 0;
            if (++f[BLC_DAY] > days_in(f[BLC_MONTH], BLC_YEAR0 + f[BLC_YEAR])) {
                f[BLC_DAY] = 1;
                if (++f[BLC_MONTH] > 12) {
                    f[BLC_MONTH] = 1;
                    f[BLC_YEAR] = (unsigned char)((f[BLC_YEAR] + 1) % BLC_YEARS);
                }
            }
        }
    }
    if (alarms && alarm_due(c))
        send(c, 0x0A);
}

static void advance(blc_clock *c, unsigned long long cycles, int alarms)
{
    unsigned long long m = minute_cycles(c);
    if (blc_break == 1 || !m)
        return;
    c->sub += cycles;
    while (c->sub >= m) {
        c->sub -= m;
        next_minute(c, alarms);
    }
}

void blc_advance_off(blc_clock *c, double seconds)
{
    if (seconds <= 0.0)
        return;
    if (seconds > OFF_MAX_S)
        seconds = OFF_MAX_S;
    advance(c, (unsigned long long)(seconds * c->hz), 0);
}

static void answer(blc_clock *c)              /* 04h: the selected set, the year last */
{
    const unsigned char *f = c->f[c->sel];
    send(c, 0x20 | (f[BLC_MINUTE] & 0x1F));
    if (f[BLC_MINUTE] & 0x20)
        send(c, 0x05);
    send(c, 0xA0 | (f[BLC_HOUR] & 0x1F));
    send(c, 0x40 | (f[BLC_MONTH] & 0x1F));
    send(c, 0x60 | (f[BLC_DAY] & 0x1F));
    send(c, 0x80 | (f[BLC_YEAR] & 0x1F));
}

void blc_put(blc_clock *c, int b)
{
    unsigned char *f = c->f[c->sel];
    int v = b & 0x1F;
    switch (b & 0xE0) {
    case 0x00:
        if (b == 0x02) c->sel = BLC_CLOCK;
        else if (b == 0x03) c->sel = BLC_ALARM;
        else if (b == 0x04) answer(c);
        else if (b == 0x05) f[BLC_MINUTE] |= 0x20;
        else if (b == 0x06) f[BLC_MINUTE] = 63;
        break;
    case 0x20:
        f[BLC_MINUTE] = (unsigned char)v;
        if (c->sel == BLC_CLOCK)
            c->sub = 0;                               /* the minute starts over */
        break;
    case 0x40: f[BLC_MONTH] = (unsigned char)v; break;
    case 0x60: f[BLC_DAY] = (unsigned char)v; break;
    case 0x80: if (blc_break != 2) f[BLC_YEAR] = (unsigned char)v; break;
    case 0xA0: f[BLC_HOUR] = (unsigned char)v; break;
    default: break;                                   /* C0h-FFh: no field */
    }
}

void blc_step(blc_clock *c, z180 *cpu, int selected)
{
    unsigned long long now = z180_cycles(cpu);
    uint8_t cntr;
    if (now - c->last >= GRAIN) {
        advance(c, now - c->last, 1);
        c->last = now;
    }
    cntr = z180_csio_cntr(cpu);
    if ((cntr & 0x07) != 0x07)
        return;                                       /* the Z180's own clock: not this controller's transfer */
    if (selected && (cntr & 0x10)) {                  /* the firmware calls with a byte loaded */
        uint8_t b = 0;
        z180_csio_clock(cpu, 0xFF, &b);
        blc_put(c, b);
        c->next_tx = now + (unsigned long long)(BLC_BYTE_S * c->hz);
    } else if (c->n_out && (cntr & 0x30) == 0x20 && now >= c->next_tx) {
        z180_csio_clock(cpu, (uint8_t)blc_take(c), NULL);
        c->next_tx = now + (unsigned long long)(BLC_BYTE_S * c->hz);
    }
}

int blc_take(blc_clock *c)
{
    int b;
    if (!c->n_out)
        return -1;
    b = c->out[0];
    memmove(c->out, c->out + 1, (size_t)--c->n_out);
    return b;
}

void blc_advance(blc_clock *c, double seconds)
{
    if (seconds > 0.0)
        advance(c, (unsigned long long)(seconds * c->hz), 1);
}

void blc_ppi_control(unsigned char *port_c, int value)
{
    if (value & 0x80)
        *port_c = 0;                                  /* a mode word: the outputs are reset */
    else if (value & 1)
        *port_c |= (unsigned char)(1 << ((value >> 1) & 7));
    else
        *port_c &= (unsigned char)~(1 << ((value >> 1) & 7));
}

/* ---- saving ---------------------------------------------------------------------------------------------------- */
static const char MAGIC[8] = {'B', 'L', 'C', 'L', 'O', 'C', 'K', '1'};

static void put64(unsigned char *p, unsigned long long v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static unsigned long long get64(const unsigned char *p)
{
    unsigned long long v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

void blc_save(const blc_clock *c, long long saved_at, unsigned char *buf)
{
    memset(buf, 0, BLC_SAVE_SIZE);
    memcpy(buf, MAGIC, 8);
    memcpy(buf + 8, c->f, sizeof c->f);
    buf[18] = (unsigned char)c->sel;
    put64(buf + 20, c->sub);
    put64(buf + 28, (unsigned long long)saved_at);
}

int blc_load(blc_clock *c, const unsigned char *buf, long long *saved_at)
{
    if (memcmp(buf, MAGIC, 8))
        return 0;
    memcpy(c->f, buf + 8, sizeof c->f);
    c->sel = buf[18] ? BLC_ALARM : BLC_CLOCK;
    c->sub = get64(buf + 20);
    if (c->sub >= minute_cycles(c))
        c->sub = 0;
    if (saved_at)
        *saved_at = (long long)get64(buf + 28);
    return 1;
}

int blc_write_tail(FILE *f, const blc_clock *c, long long saved_at)
{
    unsigned char buf[BLC_SAVE_SIZE];
    if (blc_break == 3)
        return 1;                                     /* the control: the controller left out of the state */
    blc_save(c, saved_at, buf);
    return fwrite(buf, 1, sizeof buf, f) == sizeof buf;
}

int blc_read_tail(FILE *f, unsigned char *buf)
{
    return fread(buf, 1, BLC_SAVE_SIZE, f) == BLC_SAVE_SIZE && !memcmp(buf, MAGIC, 8);
}

/* serial_linux.c -- see serial_linux.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "serial_linux.h"

#define OUT_CAP 1024

struct tty_link {
    int fd, pty;
    unsigned char in[BL_SERIAL_ROOM];       /* read from the line, not yet taken by the unit */
    int n_in;
    unsigned char out[OUT_CAP];             /* the unit's bytes not yet written, all of one status */
    int n_out, off;
    bl_serial_status applied;
    int have_applied;
};

static speed_t speed_of(long baud)
{
    static const struct { long baud; speed_t s; } SPEEDS[] = {
        {300, B300}, {600, B600}, {1200, B1200}, {2400, B2400}, {4800, B4800}, {9600, B9600}, {19200, B19200},
        {38400, B38400}, {57600, B57600}, {115200, B115200},
    };
    unsigned i;
    for (i = 0; i < sizeof SPEEDS / sizeof SPEEDS[0]; i++)
        if (SPEEDS[i].baud == baud)
            return SPEEDS[i].s;
    return 0;                               /* a rate a tty cannot be set to: left as it was */
}

static void raw_mode(int fd)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS);
    t.c_iflag &= ~(IXON | IXOFF | IXANY);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
}

static void modem_line(int fd, int bit, int on)
{
    ioctl(fd, on ? TIOCMBIS : TIOCMBIC, &bit);   /* a pseudo-terminal has none: the error is ignored */
}

static int same_format(const bl_serial_status *a, const bl_serial_status *b)
{
    return a->baud == b->baud && a->data_bits == b->data_bits && a->parity == b->parity && a->stop_bits == b->stop_bits;
}

/* the line set to the unit's status (the bytes of the old format gone first) */
static void apply(tty_link *t, const bl_serial_status *st)
{
    if (t->have_applied && bl_serial_same(&t->applied, st))
        return;
    if (!t->have_applied || !same_format(&t->applied, st)) {
        struct termios tio;
        if (!t->pty)
            tcdrain(t->fd);
        if (tcgetattr(t->fd, &tio) == 0) {
            speed_t s = st->baud > 0 ? speed_of(st->baud) : 0;
            if (s) {
                cfsetispeed(&tio, s);
                cfsetospeed(&tio, s);
            }
            tio.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
            tio.c_cflag |= st->data_bits == 7 ? CS7 : CS8;
            if (st->parity != 'N')
                tio.c_cflag |= PARENB | (st->parity == 'O' ? PARODD : 0);
            if (st->stop_bits == 2)
                tio.c_cflag |= CSTOPB;
            tcsetattr(t->fd, TCSANOW, &tio);
        }
    }
    if (!t->pty) {
        modem_line(t->fd, TIOCM_DTR, st->powered);
        modem_line(t->fd, TIOCM_RTS, st->powered && st->rts);
    }
    t->applied = *st;
    t->have_applied = 1;
}

tty_link *tty_open(const char *name, char *other, int other_cap, char *err, int errlen)
{
    tty_link *t = (tty_link *)calloc(1, sizeof(tty_link));
    if (other_cap > 0)
        other[0] = 0;
    if (!t) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (!strcmp(name, "pty")) {
        const char *slave;
        t->pty = 1;
        t->fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (t->fd < 0 || grantpt(t->fd) < 0 || unlockpt(t->fd) < 0 || !(slave = ptsname(t->fd))) {
            snprintf(err, errlen, "could not make a pseudo-terminal: %s", strerror(errno));
            if (t->fd >= 0)
                close(t->fd);
            free(t);
            return NULL;
        }
        snprintf(other, (size_t)other_cap, "%s", slave);
    } else {
        t->fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (t->fd < 0) {
            int e = errno;
            snprintf(err, errlen, "could not open %s: %s%s", name, strerror(e),
                     e == EACCES ? " (the dialout group may be needed: sudo usermod -aG dialout $USER)" : "");
            free(t);
            return NULL;
        }
        if (!isatty(t->fd)) {
            snprintf(err, errlen, "%s is not a serial port", name);
            close(t->fd);
            free(t);
            return NULL;
        }
    }
    raw_mode(t->fd);
    tcflush(t->fd, TCIOFLUSH);
    return t;
}

void tty_pump(tty_link *t, emu_unit *u)
{
    int room, k;
    if (!t || !u)
        return;
    /* the far end's bytes, no more than the unit has room for */
    room = emu_serial_space(u) - t->n_in;
    if (room > (int)sizeof t->in - t->n_in)
        room = (int)sizeof t->in - t->n_in;
    if (room > 0) {
        ssize_t r = read(t->fd, t->in + t->n_in, (size_t)room);
        if (r > 0)
            t->n_in += (int)r;
    }
    if (t->n_in) {
        k = emu_serial_write(u, t->in, t->n_in);
        memmove(t->in, t->in + k, (size_t)(t->n_in - k));
        t->n_in -= k;
    }
    /* the unit's bytes, each run under its own status */
    for (;;) {
        if (t->off == t->n_out) {
            bl_serial_status st;
            t->n_out = emu_serial_read(u, t->out, OUT_CAP, &st);
            t->off = 0;
            apply(t, &st);                  /* before the bytes that left under it (none: the status now) */
            if (!t->n_out)
                break;
        }
        {
            ssize_t w = write(t->fd, t->out + t->off, (size_t)(t->n_out - t->off));
            if (w > 0)
                t->off += (int)w;
            else if (w < 0 && errno == EAGAIN)
                break;                      /* the line is busy: the rest next round */
            else
                t->off = t->n_out;          /* nobody on the other end (EIO): lost, as on a dead line */
        }
    }
}

void tty_close(tty_link *t)
{
    if (!t)
        return;
    if (!t->pty && t->have_applied) {
        modem_line(t->fd, TIOCM_DTR, 0);
        modem_line(t->fd, TIOCM_RTS, 0);
    }
    close(t->fd);
    free(t);
}

/* serial_linux.h -- the unit's serial port on a Linux tty: a real port (/dev/ttyUSB0, /dev/ttyS0, /dev/ttyACM0) or a
 * pseudo-terminal whose other end a program on the same machine opens (a terminal program, or a disk tool run
 * under DOSBox with its serial port on that device) -- the Windows app's COM port setting (serial_win.c).
 *
 * The shell's sound thread calls tty_pump after each block the unit renders, under its lock: the bytes that
 * arrived go to the unit (emu_serial_write, no more than it has room for), the unit's go out, and the port is set
 * to the line status the firmware programmed -- the baud rate, data bits and parity, DTR for the unit's port being
 * on, RTS for /RTS0 -- before the bytes sent under it.  The tty's own flow control is off: the unit's XON/XOFF go to
 * the far end as bytes, and the far end's reach the unit.  Nothing blocks: a far end that is not there (a
 * pseudo-terminal nobody has opened) loses the unit's bytes, as a cable to nothing would.
 */
#ifndef BLAZIE_SERIAL_LINUX_H
#define BLAZIE_SERIAL_LINUX_H

#include "emu_unit.h"

typedef struct tty_link tty_link;

/* name: a device path, or "pty" for a new pseudo-terminal (its other end's path in other, else other is "").  NULL
   on failure, the reason in err. */
tty_link *tty_open(const char *name, char *other, int other_cap, char *err, int errlen);
/* one round of bytes both ways; the caller holds the lock the unit is used under */
void tty_pump(tty_link *t, emu_unit *u);
/* the unit gone from the line: DTR and RTS drop, the port closes */
void tty_close(tty_link *t);

#endif

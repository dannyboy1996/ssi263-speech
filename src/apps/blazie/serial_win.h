/* serial_win.h -- the unit's serial port on a Windows COM port: Settings > Serial port.
 *
 * One thread per open port, overlapped I/O, no polling: it wakes when bytes arrive, when a write completes, and when
 * the unit has run (com_kick, from the sound thread after each render).  It moves bytes between the port and the
 * unit (emu_serial_*, under the shell's lock) and sets the port to the line status the firmware programmed -- baud
 * rate, data bits and parity, DTR for the unit's port being on, RTS for /RTS0 -- in order with the bytes.  Windows'
 * own flow control is off: the unit's XON/XOFF go to the far end as they are, and the far end's reach the unit.
 * The portable half is ../../csrc/blazie/bl_serial.c; a Linux shell would put a tty here.
 */
#ifndef BLAZIE_SERIAL_WIN_H
#define BLAZIE_SERIAL_WIN_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "emu_unit.h"

#define COM_MAX 64

typedef struct {
    char name[16];                 /* "COM3" */
    char label[160];               /* "COM3, com0com - serial port emulator": for the menu */
} com_port_info;

/* the COM ports Windows has now (none is opened to find them), sorted by number; how many */
int com_list(com_port_info *ports, int max);

typedef struct com_link com_link;

/* opens the port and starts its thread, which reaches the unit as *unit under *lock (the shell's own); NULL on
   failure, with a reason fit for a message box in err */
com_link *com_open(const char *name, CRITICAL_SECTION *lock, emu_unit **unit, char *err, int errlen);
void com_kick(com_link *c);
void com_close(com_link *c);

#endif

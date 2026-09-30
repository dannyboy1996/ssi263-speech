/* bl_serial.h -- a Blazie unit's RS-232 port carried to a real port on the host (the emulator's COM port setting):
 * the part shared by the Braille Lite and Type 'n Speak boards.  Portable C: the platform shell (a Windows COM port,
 * a Linux tty) only moves bytes and applies the line status.
 *
 * Measured by running the firmware (both units, the same design): the port is the Z180's ASCI channel 0.  The
 * firmware programs it 9600 bit/s, 8 data bits, no parity (its Status menu changes these), and switches the port's
 * line drivers on and off with a latch bit (the Braille Lite's port A0h bit 0, the Type 'n Speak's B0h bit 0): on
 * while "serial port" is on in the Status menu, in speech-box mode, and around the storage commands, which talk at
 * 19200 bit/s whatever the menu says.  /RTS0 (CNTLA bit 4 low) says the unit takes input; it goes high under
 * hardware handshaking when the unit's buffer is full.  The unit's XON/XOFF are ordinary bytes here: the program on
 * the far end honours them, as a PC does.  ASCI channel 1 is the portable disk drive's port: not carried.
 *
 * Unit to host: the bytes the unit sent, each with the line status in force when it left (a status change is kept
 * in order with the bytes: the storage command switches to 19200 and sends XON ENQ within a millisecond, and the
 * host port must send them at 19200).  Host to unit: at most BL_SERIAL_ROOM bytes wait on the unit's side -- the
 * far end's overshoot after the unit says stop is no more than that, as with a PC's UART FIFO (the unit sends XOFF
 * with some 200 bytes of its buffer left).
 */
#ifndef BL_SERIAL_H
#define BL_SERIAL_H

#include "../cpu/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_SERIAL_ROOM 64

typedef struct {
    long baud;        /* bit/s from the ASCI's divisors and the CPU clock; 0: no clock (SS = 7, an external clock) */
    int data_bits;    /* 7 or 8 */
    char parity;      /* 'N', 'E' or 'O' */
    int stop_bits;    /* always 1: see bl_serial_decode */
    int powered;      /* the port's line drivers are on (the board's latch bit) */
    int rts;          /* /RTS0 asserted: the unit takes input */
    int rx_on, tx_on; /* the receiver and transmitter enabled (CNTLA RE, TE) */
} bl_serial_status;

/* The status from ASCI channel 0's registers.  Stop bits: 1 always.  The z180emu core's CNTLA0 readback sets MOD0
   (its /RTS0 readback lands in bit 0), so after the firmware's read-modify-writes the register says 2 stop bits
   when the Status menu says 1 (it writes 64h/6Ch for 1, 65h/6Dh for 2); a receiver takes either, and a sender of 1
   is read by a receiver set to 2.  The same readback clears RTS0: under hardware handshaking a read-modify-write
   can assert /RTS0 while the unit's buffer is still full (z180emu only; MAME's core keeps the bit). */
void bl_serial_decode(const z180_asci_regs *r, double clock_hz, int powered, bl_serial_status *s);
int  bl_serial_same(const bl_serial_status *a, const bl_serial_status *b);

typedef struct bl_serial_line bl_serial_line;

bl_serial_line *bl_serial_new(void);
void bl_serial_free(bl_serial_line *l);
/* the unit's side (the board): the status now (noted before every byte sent, and whenever the host reads); a byte
   the unit sent; the next byte on the line for the ASCI's receiver (-1: none) */
void bl_serial_note(bl_serial_line *l, const bl_serial_status *now);
void bl_serial_sent(bl_serial_line *l, unsigned char byte);
int  bl_serial_next(bl_serial_line *l);
/* the host's side: bytes for the unit (returns how many were taken: at most the room left); the unit's bytes in
   order, those of one status at a time -- *status is the status they left under (with 0 bytes: the status now);
   call until it returns 0, applying the status before writing the bytes */
int  bl_serial_room(const bl_serial_line *l);
int  bl_serial_put(bl_serial_line *l, const unsigned char *bytes, int n);
int  bl_serial_take(bl_serial_line *l, unsigned char *out, int cap, bl_serial_status *status);

#ifdef __cplusplus
}
#endif
#endif

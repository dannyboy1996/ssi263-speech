/* flash29.h -- an AMD 29Fxxx-style flash chip: its command cycles (program, sector and chip erase, autoselect), as
 * the Blazie boards' file flash answers them.  Used by bl_board.c and tns_board.c, each a 29F016 (2 MB, 32 sectors of
 * 64 KB) behind a window the board pages (the Type 'n Speak's window reaches 4 MB; its firmware uses the first 2).
 *
 * Timing (flash29.hz > 0): an erase or a program runs for the chip's typical time, the Am29F016 data sheet's (sector
 * erase 1 s, chip erase 32 s, a byte 7 us), counted in the board's CPU cycles.  An erase also takes its embedded
 * preprogramming: the chip first programs every byte it erases to 00h, which the data sheet's erase times exclude
 * ("Excludes 00H programming prior to erasure"), at its chip programming time (14.4 s typical for the 2 MB) per byte
 * not already 00h -- an initialisation's chip erase is ~46 s, not 32 (users: the flash initialization beeps).
 * Meanwhile a read anywhere returns the status, not the data: DQ7 the complement of the byte being programmed (0 while erasing), DQ6 toggling on every read,
 * DQ5 0 (no time-out), DQ3 1 while erasing; writes are ignored.  The firmware polls DQ7 and keeps its user informed
 * while it waits (the Blazie units chirp through the speech chip during an erase).  hz = 0: every operation is done
 * at once (the screen-reader drivers and their state recipes, which press keys at fixed instruction counts).
 */
#ifndef BLAZIE_FLASH29_H
#define BLAZIE_FLASH29_H

typedef struct {
    unsigned char *data;           /* the chip's contents (the caller owns them) */
    unsigned long size;
    unsigned char maker, device;   /* the autoselect answer: 01h (AMD), ADh (29F016) */
    int short_decode;              /* 1: the command addresses match on A0-A10 too (555h/2AAh), as a 29F016's do */
    int state, autoselect;
    double hz;                     /* the CPU clock the `now` arguments count; 0: no busy time (the default) */
    /* the operation in progress (hz > 0): busy until that cycle; its status's DQ7 and DQ3 */
    unsigned long long busy_until;
    unsigned char busy_status;
    int toggle;
    /* counters (tests): chip and sector erases begun, bytes programmed */
    unsigned long n_chip_erase, n_sector_erase, n_program;
} flash29;

#define FLASH29_SECTOR 0x10000UL
#define FLASH29_SECTOR_ERASE_S 1.0
#define FLASH29_CHIP_ERASE_S 32.0
#define FLASH29_PROGRAM_S 7e-6
#define FLASH29_CHIP_PROGRAM_S 14.4        /* the whole 2 MB, typical: the erase's preprogramming, pro rata per byte */
#define FLASH29_CHIP_BYTES 0x200000UL

/* `now`: the board's CPU cycle count at the access */
unsigned char flash29_read(flash29 *f, unsigned long off, unsigned long long now);
void flash29_write(flash29 *f, unsigned long off, unsigned char v, unsigned long long now);
/* 1 while an erase is in progress at `now`, 2 while a byte is being programmed, 0 idle */
int flash29_busy(const flash29 *f, unsigned long long now);

#endif

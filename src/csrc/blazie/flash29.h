/* flash29.h -- an AMD 29Fxxx-style flash chip: its command cycles (program, sector and chip erase, autoselect), as
 * the Blazie boards' file flash answers them.  Used by bl_board.c (a 29F040, 512 KB) and tns_board.c (a 29F016,
 * 4 MB).
 */
#ifndef BLAZIE_FLASH29_H
#define BLAZIE_FLASH29_H

typedef struct {
    unsigned char *data;           /* the chip's contents (the caller owns them) */
    unsigned long size;
    unsigned char maker, device;   /* the autoselect answer: 01h (AMD), A4h (29F040) or ADh (29F016) */
    int short_decode;              /* 1: the command addresses match on A0-A10 too (555h/2AAh), as a 29F016's do */
    int state, autoselect;
} flash29;

unsigned char flash29_read(const flash29 *f, unsigned long off);
void flash29_write(flash29 *f, unsigned long off, unsigned char v);

#endif

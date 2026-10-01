/* flash29.c -- see flash29.h. */
#include <string.h>
#include "flash29.h"

#define DQ7 0x80
#define DQ6 0x40
#define DQ3 0x08

int flash29_busy(const flash29 *f, unsigned long long now)
{
    if (f->hz <= 0 || now >= f->busy_until)
        return 0;
    return (f->busy_status & DQ3) ? 1 : 2;
}

static void begin(flash29 *f, unsigned long long now, double seconds, unsigned char status)
{
    if (f->hz <= 0)
        return;
    f->busy_until = now + (unsigned long long)(seconds * f->hz + 0.5);
    f->busy_status = status;
}

unsigned char flash29_read(flash29 *f, unsigned long off, unsigned long long now)
{
    if (flash29_busy(f, now)) {                  /* the embedded algorithm's status, at any address */
        f->toggle ^= DQ6;
        return (unsigned char)(f->busy_status | f->toggle);
    }
    if (f->autoselect)
        return (off & 3) == 0 ? f->maker : (off & 3) == 1 ? f->device : 0x00;
    return f->data[off];
}

void flash29_write(flash29 *f, unsigned long off, unsigned char v, unsigned long long now)
{
    unsigned a = (unsigned)(off & 0x7FFF);
    int is555 = a == 0x5555 || (f->short_decode && (off & 0x7FF) == 0x555);
    int is2aa = a == 0x2AAA || (f->short_decode && (off & 0x7FF) == 0x2AA);
    if (flash29_busy(f, now))
        return;                                  /* commands are ignored until the operation ends */
    switch (f->state) {
    case 0: if (v == 0xF0) { f->autoselect = 0; return; }
            f->state = (is555 && v == 0xAA) ? 1 : 0; break;
    case 1: f->state = (is2aa && v == 0x55) ? 2 : 0; break;
    case 2: f->state = 0;
            if (!is555) break;
            if (v == 0xA0) f->state = 3;
            else if (v == 0x80) f->state = 4;
            else if (v == 0x90) f->autoselect = 1;
            else if (v == 0xF0) f->autoselect = 0;
            break;
    case 3: f->data[off] &= v;
            f->state = 0;
            f->n_program++;
            begin(f, now, FLASH29_PROGRAM_S, (unsigned char)(~v & DQ7));
            break;
    case 4: f->state = (is555 && v == 0xAA) ? 5 : 0; break;
    case 5: f->state = (is2aa && v == 0x55) ? 6 : 0; break;
    case 6: f->state = 0;
            if (v == 0x10) {
                memset(f->data, 0xFF, f->size);
                f->n_chip_erase++;
                begin(f, now, FLASH29_CHIP_ERASE_S, DQ3);
            } else if (v == 0x30) {
                memset(f->data + (off & ~(FLASH29_SECTOR - 1) & (f->size - 1)), 0xFF, FLASH29_SECTOR);
                f->n_sector_erase++;
                begin(f, now, FLASH29_SECTOR_ERASE_S, DQ3);
            }
            break;
    }
}

/* flash29.c -- see flash29.h.  bl_board.c's file flash, moved here unchanged for the 29F040 (short_decode = 0). */
#include <string.h>
#include "flash29.h"

unsigned char flash29_read(const flash29 *f, unsigned long off)
{
    if (f->autoselect)
        return (off & 3) == 0 ? f->maker : (off & 3) == 1 ? f->device : 0x00;
    return f->data[off];
}

void flash29_write(flash29 *f, unsigned long off, unsigned char v)
{
    unsigned a = (unsigned)(off & 0x7FFF);
    int is555 = a == 0x5555 || (f->short_decode && (off & 0x7FF) == 0x555);
    int is2aa = a == 0x2AAA || (f->short_decode && (off & 0x7FF) == 0x2AA);
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
    case 3: f->data[off] &= v; f->state = 0; break;
    case 4: f->state = (is555 && v == 0xAA) ? 5 : 0; break;
    case 5: f->state = (is2aa && v == 0x55) ? 6 : 0; break;
    case 6: f->state = 0;
            if (v == 0x10)
                memset(f->data, 0xFF, f->size);
            else if (v == 0x30)
                memset(f->data + (off & ~0xFFFFUL & (f->size - 1)), 0xFF, 0x10000);
            break;
    }
}

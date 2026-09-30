/* so_hex.c -- Intel HEX into a 1 MB image (so_hex.h).  MIT. */
#include "so_hex.h"

static int nib(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

long so_hex_parse(const char *text, size_t len, uint8_t *mem)
{
    size_t i = 0;
    uint32_t seg = 0;
    long stored = 0;
    while (i < len) {
        size_t s = i, e;
        while (i < len && text[i] != '\n')
            i++;
        e = i++;                                   /* the line is text[s..e) */
        while (s < e && (text[s] == ' ' || text[s] == '\t' || text[s] == '\r'))
            s++;                                   /* Python's strip() */
        while (e > s && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r'))
            e--;
        if (s >= e || text[s] != ':')
            continue;
        {
            uint8_t b[260];
            size_t n = 0, k;
            unsigned cnt, addr, typ;
            for (k = s + 1; k + 1 < e && n < sizeof b; k += 2) {
                int hi = nib(text[k]), lo = nib(text[k + 1]);
                if (hi < 0 || lo < 0)
                    return -1;
                b[n++] = (uint8_t)(hi << 4 | lo);
            }
            if (n < 4)
                return -1;
            cnt = b[0];
            addr = (unsigned)b[1] << 8 | b[2];
            typ = b[3];
            if (n < 4 + cnt)
                return -1;
            if (typ == 0) {
                for (k = 0; k < cnt; k++) {
                    mem[(seg + addr + k) & 0xFFFFF] = b[4 + k];
                    stored++;
                }
            } else if (typ == 2) {
                if (cnt < 2)
                    return -1;
                seg = ((uint32_t)b[4] << 8 | b[5]) << 4;
            } else if (typ == 1) {
                break;
            }
        }
    }
    return stored;
}

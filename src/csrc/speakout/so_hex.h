/* so_hex.h -- Intel HEX into a 1 MB image, as src/hosts/speakout.py's load_intel_hex reads it: data records (type 0)
 * at the current extended segment (type 2) plus their address, wrapping at 1 MB; end of file (type 1) stops; every
 * other record and every line not starting with ':' is skipped.  No checksum is checked (the Python loader checks
 * none).  MIT. */
#ifndef SO_HEX_H
#define SO_HEX_H

#include <stddef.h>
#include <stdint.h>

/* Loads text[0..len) into mem (1 MB).  Returns the data bytes stored, or -1 on a malformed record (a bad hex digit,
   a record shorter than its count). */
long so_hex_parse(const char *text, size_t len, uint8_t *mem);

#endif

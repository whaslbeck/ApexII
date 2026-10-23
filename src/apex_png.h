#ifndef APEX_PNG_H
#define APEX_PNG_H

/* Reading images for import (DMD frames): PNG (grey, RGB, palette, with or
   without alpha; 1-16 bit; not interlaced) and the binary netpbm formats the
   toolchain writes (PBM P4, PGM P5).  The result is one 8-bit luminance byte
   per pixel, row by row; transparent pixels count as black.  A PBM 1-bit is a
   lit dot (255), as apexdmd writes DMD planes, so an exported frame comes back
   unchanged. */

#include <stddef.h>
#include <stdint.h>

/* Returns 1 and a malloc'd w*h luminance buffer, else 0 with a message. */
int apex_image_read_gray(const char *path, uint8_t **pixels, unsigned *w, unsigned *h, char *err,
                         size_t errsz);

/* zlib (RFC 1950/1951) decompression into a growing buffer; 1 on success. */
int apex_inflate_zlib(const uint8_t *src, size_t len, uint8_t **out, size_t *out_len);

#endif /* APEX_PNG_H */

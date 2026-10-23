#ifndef APEXDMD_H
#define APEXDMD_H

#include <stddef.h>
#include <stdint.h>

#define APEX_DMD_WIDTH 128u
#define APEX_DMD_HEIGHT 32u
#define APEX_DMD_ROW_BYTES (APEX_DMD_WIDTH / 8u)
#define APEX_DMD_PAGE_BYTES (APEX_DMD_ROW_BYTES * APEX_DMD_HEIGHT)

int apexdmd_decode_fullframe(const uint8_t *src, size_t src_size, uint8_t *dest, size_t *consumed,
                             uint8_t *type_out);
/* Encode one plane (APEX_DMD_PAGE_BYTES) as a fullframe of `type`: 0x00 raw,
   0x01 / 0x02 run-length (column / row order, one flag byte).  out needs
   APEX_DMD_ENCODE_MAX bytes.  Returns the encoded size. */
#define APEX_DMD_ENCODE_MAX (2u + 3u * APEX_DMD_PAGE_BYTES)
size_t apexdmd_encode_plane(const uint8_t *plane, uint8_t type, uint8_t *out);
/* The smallest of the three encodings; *type_out (may be NULL) gets its type. */
size_t apexdmd_encode_best(const uint8_t *plane, uint8_t *out, uint8_t *type_out);
/* 128x32 luminance -> planes (nplanes * APEX_DMD_PAGE_BYTES).  One plane:
   pixels >= 128 are on.  Two planes: four levels; `bright` (0 or 1) is the
   plane that lights the pixel for longer (level 2), the other gives level 1,
   both give level 3. */
void apexdmd_planes_from_gray(const uint8_t *gray, int nplanes, int bright, uint8_t *planes);

int apexdmd_write_plane_pbm(const char *path, const uint8_t *plane);
int apexdmd_write_pair_pgm(const char *path, const uint8_t *plane0, const uint8_t *plane1);

#endif

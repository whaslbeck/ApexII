#ifndef APEXDMD_H
#define APEXDMD_H

#include <stddef.h>
#include <stdint.h>

#define APEX_DMD_WIDTH 128u
#define APEX_DMD_HEIGHT 32u
#define APEX_DMD_ROW_BYTES (APEX_DMD_WIDTH / 8u)
#define APEX_DMD_PAGE_BYTES (APEX_DMD_ROW_BYTES * APEX_DMD_HEIGHT)

int apexdmd_decode_fullframe(const uint8_t *src, size_t src_size, uint8_t *dest,
                             size_t *consumed, uint8_t *type_out);
int apexdmd_write_plane_pbm(const char *path, const uint8_t *plane);
int apexdmd_write_pair_pgm(const char *path, const uint8_t *plane0, const uint8_t *plane1);

#endif

#ifndef APEXSPRITE_H
#define APEXSPRITE_H

#include <stddef.h>
#include <stdint.h>

#define APEX_SPRITE_MAX_WIDTH  128u
#define APEX_SPRITE_MAX_HEIGHT 128u
#define APEX_SPRITE_MAX_ROW_BYTES ((APEX_SPRITE_MAX_WIDTH + 7u) / 8u)
#define APEX_SPRITE_MAX_BYTES (APEX_SPRITE_MAX_ROW_BYTES * APEX_SPRITE_MAX_HEIGHT)

/* Returns 1 if src[0] is a VSI header byte (0x00, 0xFD, 0xFE, 0xFF). */
int apexsprite_is_header(const uint8_t *src, size_t src_size);

/* Returns 1 if src[0] is a valid VSI no-header width byte (1..128). */
int apexsprite_is_noheader(const uint8_t *src, size_t src_size);

/* Decode a header-format VariableSizedImage (first byte 0x00/0xFD/0xFE/0xFF).
   Format: [header][y_off][x_off][height_px][width_px][raw bitmap MSB-first row-major]
   dest must be at least APEX_SPRITE_MAX_BYTES bytes; zeroed on entry.
   enc_type_out is always set to 0 (no encoding — data is raw).
   All out-pointers are optional (may be NULL).
   Returns 1 on success. */
int apexsprite_decode(const uint8_t *src, size_t src_size,
                      uint8_t *dest,
                      uint8_t *header_type_out,
                      uint8_t *vert_offset_out,
                      uint8_t *horiz_offset_out,
                      uint8_t *width_out,
                      uint8_t *height_out,
                      uint8_t *enc_type_out,
                      size_t  *consumed_out);

/* Decode a no-header VariableSizedImage (first byte 1..128 = width in pixels).
   Format: [width_px][raw bitmap MSB-first row-major]
   Height must be supplied from the enclosing font/sprite table descriptor.
   dest must be at least APEX_SPRITE_MAX_BYTES bytes; zeroed on entry.
   All out-pointers are optional (may be NULL).
   Returns 1 on success. */
int apexsprite_decode_noheader(const uint8_t *src, size_t src_size,
                               uint8_t *dest,
                               uint8_t table_height,
                               uint8_t *width_out,
                               size_t  *consumed_out);

#endif

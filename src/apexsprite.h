#ifndef APEXSPRITE_H
#define APEXSPRITE_H

#include <stddef.h>
#include <stdint.h>

#define APEX_SPRITE_MAX_WIDTH  128u
#define APEX_SPRITE_MAX_HEIGHT 128u
#define APEX_SPRITE_MAX_ROW_BYTES ((APEX_SPRITE_MAX_WIDTH + 7u) / 8u)
#define APEX_SPRITE_MAX_BYTES (APEX_SPRITE_MAX_ROW_BYTES * APEX_SPRITE_MAX_HEIGHT)

/* VSI image encodings, distinguished by the header byte (see apexsprite_decode).
   enc_type_out is set to one of these. */
#define APEX_SPRITE_ENC_MONO             0  /* 0x00: single plane, raw            */
#define APEX_SPRITE_ENC_FD               1  /* 0xFD: single plane (drawn like 0x00) */
#define APEX_SPRITE_ENC_BICOLOR_INDIRECT 2  /* 0xFE: hdr + 2-byte ptr to plane 1 + plane 0 inline */
#define APEX_SPRITE_ENC_BICOLOR_DIRECT   3  /* 0xFF: hdr + plane 1 + plane 0, both inline */

/* Returns 1 if src[0] is a VSI header byte (0x00, 0xFD, 0xFE, 0xFF). */
int apexsprite_is_header(const uint8_t *src, size_t src_size);

/* Returns 1 if src[0] is a valid VSI no-header width byte (1..128). */
int apexsprite_is_noheader(const uint8_t *src, size_t src_size);

/* Decode a header-format VariableSizedImage (first byte 0x00/0xFD/0xFE/0xFF).
   Header: [header][y_off][x_off][height_px][width_px], then plane data whose
   layout depends on the header byte (page = ceil(width/8)*height bytes):
     0x00 mono / 0xFD       : [page]                    consumed = 5 + page
     0xFE bicolor-indirect  : [2-byte ptr to plane1][page(plane0)]
                                                        consumed = 5 + 2 + page
     0xFF bicolor-direct    : [page(plane1)][page(plane0)]
                                                        consumed = 5 + 2*page
   dest (>= APEX_SPRITE_MAX_BYTES, zeroed on entry) receives plane 0; for the
   bicolor formats plane 1 (inline before plane 0 for direct, via the 2-byte
   pointer for indirect) is left to the caller to fetch.
   enc_type_out gets one of APEX_SPRITE_ENC_*. consumed_out gets the in-place
   byte length (for bicolor-indirect, plane 1 lives elsewhere via the pointer
   and is not counted).  All out-pointers are optional (may be NULL).
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

#include "apexsprite.h"

#include <string.h>

int apexsprite_is_header(const uint8_t *src, size_t src_size)
{
    uint8_t b;
    if (!src || src_size == 0) return 0;
    b = src[0];
    return b == 0x00u || b == 0xFDu || b == 0xFEu || b == 0xFFu;
}

int apexsprite_is_noheader(const uint8_t *src, size_t src_size)
{
    if (!src || src_size == 0) return 0;
    return src[0] >= 1u && src[0] <= APEX_SPRITE_MAX_WIDTH;
}

/* Header-format VSI decode.
   Layout: [hdr][y_off][x_off][height_px][width_px][raw bitmap]
   Pixel data is stored MSB-first, row-major: bit 7 of each byte = leftmost pixel. */
int apexsprite_decode(const uint8_t *src, size_t src_size,
                      uint8_t *dest,
                      uint8_t *header_type_out,
                      uint8_t *vert_offset_out,
                      uint8_t *horiz_offset_out,
                      uint8_t *width_out,
                      uint8_t *height_out,
                      uint8_t *enc_type_out,
                      size_t  *consumed_out)
{
    uint8_t hdr, vert, horiz, height, width, row_bytes;
    size_t page_bytes;

    if (!src || !dest || src_size < 5u) return 0;

    hdr = src[0];
    if (hdr != 0x00u && hdr != 0xFDu && hdr != 0xFEu && hdr != 0xFFu) return 0;

    vert   = src[1];
    horiz  = src[2];
    height = src[3];
    width  = src[4];

    if (width == 0 || height == 0 ||
        width > APEX_SPRITE_MAX_WIDTH || height > APEX_SPRITE_MAX_HEIGHT) {
        return 0;
    }

    row_bytes  = (uint8_t)((width + 7u) / 8u);
    page_bytes = (size_t)row_bytes * (size_t)height;

    if (src_size < 5u + page_bytes) return 0;

    memset(dest, 0, APEX_SPRITE_MAX_BYTES);
    memcpy(dest, src + 5u, page_bytes);

    if (header_type_out)  *header_type_out  = hdr;
    if (vert_offset_out)  *vert_offset_out  = vert;
    if (horiz_offset_out) *horiz_offset_out = horiz;
    if (width_out)        *width_out        = width;
    if (height_out)       *height_out       = height;
    if (enc_type_out)     *enc_type_out     = 0;
    if (consumed_out)     *consumed_out     = 5u + page_bytes;
    return 1;
}

/* No-header VSI decode.
   Layout: [width_px][raw bitmap]
   Height is not stored in the data — it must be supplied from the table descriptor. */
int apexsprite_decode_noheader(const uint8_t *src, size_t src_size,
                               uint8_t *dest,
                               uint8_t table_height,
                               uint8_t *width_out,
                               size_t  *consumed_out)
{
    uint8_t width, row_bytes;
    size_t page_bytes;

    if (!src || !dest || src_size < 1u || table_height == 0) return 0;
    if (table_height > APEX_SPRITE_MAX_HEIGHT) return 0;

    width = src[0];
    if (width == 0 || width > APEX_SPRITE_MAX_WIDTH) return 0;

    row_bytes  = (uint8_t)((width + 7u) / 8u);
    page_bytes = (size_t)row_bytes * (size_t)table_height;

    if (src_size < 1u + page_bytes) return 0;

    memset(dest, 0, APEX_SPRITE_MAX_BYTES);
    memcpy(dest, src + 1u, page_bytes);

    if (width_out)    *width_out    = width;
    if (consumed_out) *consumed_out = 1u + page_bytes;
    return 1;
}

#include "apexdmd.h"

#include <stdio.h>
#include <string.h>

enum {
    WRITE_TYPE_COLUMNS = 1,
    WRITE_TYPE_ROWS = 2
};

static uint8_t reverse_bits(uint8_t value)
{
    value = (uint8_t)(((value & 0x55u) << 1) | ((value >> 1) & 0x55u));
    value = (uint8_t)(((value & 0x33u) << 2) | ((value >> 2) & 0x33u));
    value = (uint8_t)((value << 4) | (value >> 4));
    return value;
}

static void write_next_8bit_value(uint8_t *dest, size_t *write_counter, uint8_t value,
                                  uint8_t type)
{
    size_t count = *write_counter;
    size_t index;

    if (count >= APEX_DMD_PAGE_BYTES) {
        return;
    }

    if (type == WRITE_TYPE_ROWS) {
        index = count;
    } else {
        size_t row = count % APEX_DMD_HEIGHT;
        size_t col = count / APEX_DMD_HEIGHT;
        index = row * APEX_DMD_ROW_BYTES + col;
    }

    dest[index] = value;
    *write_counter = count + 1u;
}

static int decode_01or02(const uint8_t *src, size_t src_size, uint8_t *dest, size_t *consumed,
                         uint8_t type)
{
    size_t pos = 1u;
    size_t write_counter = 0u;
    uint8_t special_flag;

    if (src_size < 2u) {
        return 0;
    }

    special_flag = src[pos++];
    while (write_counter < APEX_DMD_PAGE_BYTES) {
        uint8_t ch;

        if (pos >= src_size) {
            return 0;
        }
        ch = src[pos++];
        if (ch == special_flag) {
            uint8_t repeat_count;
            uint8_t repeat_value;
            unsigned remaining;

            if (pos + 1u >= src_size) {
                return 0;
            }
            repeat_count = src[pos++];
            repeat_value = src[pos++];
            remaining = repeat_count == 0u ? 256u : (unsigned)repeat_count;
            while (remaining != 0u && write_counter < APEX_DMD_PAGE_BYTES) {
                write_next_8bit_value(dest, &write_counter, repeat_value, type);
                remaining--;
            }
        } else {
            write_next_8bit_value(dest, &write_counter, ch, type);
        }
    }

    if (consumed) {
        *consumed = pos;
    }
    return 1;
}

int apexdmd_decode_fullframe(const uint8_t *src, size_t src_size, uint8_t *dest,
                             size_t *consumed, uint8_t *type_out)
{
    uint8_t type;

    if (!src || !dest || src_size == 0u) {
        return 0;
    }

    memset(dest, 0, APEX_DMD_PAGE_BYTES);
    type = (uint8_t)(src[0] & 0x0fu);
    if (type_out) {
        *type_out = type;
    }

    switch (type) {
    case 0x01u:
        return decode_01or02(src, src_size, dest, consumed, WRITE_TYPE_COLUMNS);
    case 0x02u:
        return decode_01or02(src, src_size, dest, consumed, WRITE_TYPE_ROWS);
    default:
        return 0;
    }
}

int apexdmd_write_plane_pbm(const char *path, const uint8_t *plane)
{
    FILE *out;
    size_t row;

    out = fopen(path, "wb");
    if (!out) {
        return 0;
    }
    if (fprintf(out, "P4\n%u %u\n", (unsigned)APEX_DMD_WIDTH, (unsigned)APEX_DMD_HEIGHT) < 0) {
        fclose(out);
        return 0;
    }
    for (row = 0; row < APEX_DMD_HEIGHT; row++) {
        uint8_t packed[APEX_DMD_ROW_BYTES];
        size_t col;

        for (col = 0; col < APEX_DMD_ROW_BYTES; col++) {
            packed[col] = reverse_bits(plane[row * APEX_DMD_ROW_BYTES + col]);
        }
        if (fwrite(packed, 1, sizeof(packed), out) != sizeof(packed)) {
            fclose(out);
            return 0;
        }
    }
    fclose(out);
    return 1;
}

int apexdmd_write_pair_pgm(const char *path, const uint8_t *plane0, const uint8_t *plane1)
{
    FILE *out;
    size_t row;

    out = fopen(path, "wb");
    if (!out) {
        return 0;
    }
    if (fprintf(out, "P5\n%u %u\n255\n", (unsigned)APEX_DMD_WIDTH, (unsigned)APEX_DMD_HEIGHT) < 0) {
        fclose(out);
        return 0;
    }
    for (row = 0; row < APEX_DMD_HEIGHT; row++) {
        uint8_t pixels[APEX_DMD_WIDTH];
        size_t col_byte;

        for (col_byte = 0; col_byte < APEX_DMD_ROW_BYTES; col_byte++) {
            uint8_t a = plane0[row * APEX_DMD_ROW_BYTES + col_byte];
            uint8_t b = plane1[row * APEX_DMD_ROW_BYTES + col_byte];
            size_t bit;

            for (bit = 0; bit < 8u; bit++) {
                size_t x = col_byte * 8u + bit;
                unsigned p0 = (a >> bit) & 1u;
                unsigned p1 = (b >> bit) & 1u;
                unsigned level = p0 + p1;

                pixels[x] = (uint8_t)(level == 0u ? 0u : (level == 1u ? 127u : 255u));
            }
        }
        if (fwrite(pixels, 1, sizeof(pixels), out) != sizeof(pixels)) {
            fclose(out);
            return 0;
        }
    }
    fclose(out);
    return 1;
}

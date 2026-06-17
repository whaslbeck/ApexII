#include "apexdmd.h"

#include <stdio.h>
#include <string.h>

enum {
    WRITE_TYPE_COLUMNS = 1,
    WRITE_TYPE_ROWS = 2
};

/* Source reader shared by the byte-oriented and bit-stream decoders.  `pos` is
   the running offset from the start of the encoded frame (offset 0 is the type
   byte), so the final `pos` doubles as the consumed-byte count.  `mask` holds
   the current bit position for the bit-stream encodings (0x04/0x05/0x0a/0x0b);
   it is irrelevant to the byte encodings. */
typedef struct {
    const uint8_t *src;
    size_t size;
    size_t pos;
    uint8_t mask;
    int bad;
} DmdReader;

static uint8_t reverse_bits(uint8_t value)
{
    value = (uint8_t)(((value & 0x55u) << 1) | ((value >> 1) & 0x55u));
    value = (uint8_t)(((value & 0x33u) << 2) | ((value >> 2) & 0x33u));
    value = (uint8_t)((value << 4) | (value >> 4));
    return value;
}

static uint8_t rd_byte(DmdReader *r)
{
    if (r->pos >= r->size) {
        r->bad = 1;
        return 0u;
    }
    return r->src[r->pos++];
}

/* Read a single bit (MSB first), advancing to the next source byte once all 8
   bits are consumed.  Mirrors WPCEdit's readNextBit(). */
static unsigned rd_bit(DmdReader *r)
{
    unsigned bit;

    if (r->pos >= r->size) {
        r->bad = 1;
        return 0u;
    }
    bit = (unsigned)(r->src[r->pos] & r->mask);
    r->mask = (uint8_t)(r->mask >> 1);
    if (r->mask == 0u) {
        r->mask = 0x80u;
        r->pos++;
    }
    return bit;
}

/* Read one 8-bit value from the bit stream.  A leading 1 selects a dictionary
   entry (count of following 1-bits, max 7, indexes RepeatBytes[]); a leading 0
   is followed by 8 literal bits.  Mirrors WPCEdit's readNext8BitValue(). */
static uint8_t rd_stream_value(DmdReader *r, const uint8_t *repeat_bytes)
{
    if (rd_bit(r)) {
        int ones = 0;
        int i;

        for (i = 0; i < 7; i++) {
            if (rd_bit(r)) {
                ones++;
            } else {
                break;
            }
        }
        return repeat_bytes[ones];
    } else {
        uint8_t value = 0u;
        uint8_t mask = 0x80u;
        int i;

        for (i = 0; i < 8; i++) {
            if (rd_bit(r)) {
                value = (uint8_t)(value | mask);
            }
            mask = (uint8_t)(mask >> 1);
        }
        return value;
    }
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

/* Repeat `value` `count` times, with WPCEdit's "0 means fill the rest of the
   page" semantics (the JS `do { } while (--count && ...)` decrements 0 into a
   negative, so only the page-full check stops it). */
static void emit_repeat(uint8_t *dest, size_t *write_counter, uint8_t value, unsigned count,
                        uint8_t type)
{
    unsigned remaining = count ? count : 0xffffffffu;

    do {
        write_next_8bit_value(dest, write_counter, value, type);
    } while (--remaining && *write_counter < APEX_DMD_PAGE_BYTES);
}

/* 0x00: raw page copy, no encoding. */
static void decode_00(DmdReader *r, uint8_t *dest)
{
    size_t i;

    for (i = 0; i < APEX_DMD_PAGE_BYTES; i++) {
        dest[i] = rd_byte(r);
        if (r->bad) {
            return;
        }
    }
}

/* 0x01/0x02: simple single-flag-byte repeats (columns / rows). */
static void decode_01or02(DmdReader *r, uint8_t *dest, uint8_t type)
{
    uint8_t special = rd_byte(r);
    size_t write_counter = 0u;

    while (write_counter < APEX_DMD_PAGE_BYTES && !r->bad) {
        uint8_t ch = rd_byte(r);

        if (r->bad) {
            return;
        }
        if (ch == special) {
            uint8_t repeat_count = rd_byte(r);
            uint8_t repeat_value = rd_byte(r);
            unsigned remaining = repeat_count == 0u ? 256u : (unsigned)repeat_count;

            if (r->bad) {
                return;
            }
            while (remaining != 0u && write_counter < APEX_DMD_PAGE_BYTES) {
                write_next_8bit_value(dest, &write_counter, repeat_value, type);
                remaining--;
            }
        } else {
            write_next_8bit_value(dest, &write_counter, ch, type);
        }
    }
}

/* 0x04/0x05: complex repeats, 1 flag byte + 8-byte dictionary, bit stream. */
static void decode_04or05(DmdReader *r, uint8_t *dest, uint8_t type)
{
    uint8_t special = rd_byte(r);
    uint8_t repeat[8];
    size_t write_counter = 0u;
    int i;

    for (i = 0; i < 8; i++) {
        repeat[i] = rd_byte(r);
    }
    r->mask = 0x80u;

    while (write_counter < APEX_DMD_PAGE_BYTES && !r->bad) {
        uint8_t ch = rd_stream_value(r, repeat);

        if (ch == special) {
            uint8_t value1 = rd_stream_value(r, repeat);
            uint8_t value2 = rd_stream_value(r, repeat);

            emit_repeat(dest, &write_counter, value2, value1, type);
        } else {
            write_next_8bit_value(dest, &write_counter, ch, type);
        }
    }
}

/* 0x06/0x07: XOR-repeats.  Plane data gets 0x00 where the XOR run applies (the
   XOR overlay is an animation delta we do not reconstruct); the source-byte
   walk — and therefore the consumed length — matches the game. */
static void decode_06or07(DmdReader *r, uint8_t *dest, uint8_t type)
{
    uint8_t special = rd_byte(r);
    size_t write_counter = 0u;

    while (write_counter < APEX_DMD_PAGE_BYTES && !r->bad) {
        uint8_t ch = rd_byte(r);

        if (r->bad) {
            return;
        }
        if (ch == special) {
            uint8_t value1 = rd_byte(r);
            (void)rd_byte(r); /* XOR value: not applied to a standalone plane */

            if (r->bad) {
                return;
            }
            emit_repeat(dest, &write_counter, 0x00u, value1, type);
        } else {
            write_next_8bit_value(dest, &write_counter, ch, type);
        }
    }
}

/* 0x08/0x09: bulk data loads alternating with bulk skips, byte stream. */
static void decode_08or09(DmdReader *r, uint8_t *dest, uint8_t type)
{
    size_t write_counter = 0u;
    int looping = 1;
    uint8_t count;

    count = rd_byte(r); /* start flag: zero => begin with a skip phase */
    if (r->bad) {
        return;
    }
    if (count == 0u) {
        uint8_t skips = rd_byte(r);

        if (r->bad) {
            return;
        }
        if (skips) {
            emit_repeat(dest, &write_counter, 0x00u, skips, type);
        }
        if (write_counter >= APEX_DMD_PAGE_BYTES) {
            looping = 0;
        }
    }

    while (looping && !r->bad) {
        count = rd_byte(r);
        if (r->bad) {
            return;
        }
        if (count) {
            do {
                uint8_t pattern = rd_byte(r);

                if (r->bad) {
                    return;
                }
                write_next_8bit_value(dest, &write_counter, pattern, type);
            } while (--count && write_counter < APEX_DMD_PAGE_BYTES);
        }
        if (write_counter >= APEX_DMD_PAGE_BYTES) {
            looping = 0;
        }
        if (looping) {
            uint8_t skips = rd_byte(r);

            if (r->bad) {
                return;
            }
            if (skips) {
                emit_repeat(dest, &write_counter, 0x00u, skips, type);
            }
            if (write_counter >= APEX_DMD_PAGE_BYTES) {
                looping = 0;
            }
        }
    }
}

/* 0x0a/0x0b: bulk data loads alternating with bulk skips, 8-byte dictionary,
   bit stream. */
static void decode_0aor0b(DmdReader *r, uint8_t *dest, uint8_t type)
{
    uint8_t repeat[8];
    size_t write_counter = 0u;
    int looping = 1;
    uint8_t count;
    int i;

    for (i = 0; i < 8; i++) {
        repeat[i] = rd_byte(r);
    }
    r->mask = 0x80u;

    count = rd_stream_value(r, repeat); /* start flag */
    if (count == 0u) {
        uint8_t skips = rd_stream_value(r, repeat);

        if (skips) {
            emit_repeat(dest, &write_counter, 0x00u, skips, type);
        }
        if (write_counter >= APEX_DMD_PAGE_BYTES) {
            looping = 0;
        }
    }

    while (looping && !r->bad) {
        count = rd_stream_value(r, repeat);
        if (count) {
            do {
                uint8_t value = rd_stream_value(r, repeat);

                write_next_8bit_value(dest, &write_counter, value, type);
            } while (--count && write_counter < APEX_DMD_PAGE_BYTES);
        }
        if (write_counter >= APEX_DMD_PAGE_BYTES) {
            looping = 0;
        }
        if (looping) {
            uint8_t skips = rd_stream_value(r, repeat);

            if (skips) {
                emit_repeat(dest, &write_counter, 0x00u, skips, type);
            }
            if (write_counter >= APEX_DMD_PAGE_BYTES) {
                looping = 0;
            }
        }
    }
}

int apexdmd_decode_fullframe(const uint8_t *src, size_t src_size, uint8_t *dest,
                             size_t *consumed, uint8_t *type_out)
{
    DmdReader r;
    uint8_t type;
    int bitstream = 0;

    if (!src || !dest || src_size == 0u) {
        return 0;
    }

    memset(dest, 0, APEX_DMD_PAGE_BYTES);
    type = (uint8_t)(src[0] & 0x0fu);
    if (type_out) {
        *type_out = type;
    }

    r.src = src;
    r.size = src_size;
    r.pos = 1u; /* skip the type byte */
    r.mask = 0x80u;
    r.bad = 0;

    switch (type) {
    case 0x00u:
        decode_00(&r, dest);
        break;
    case 0x01u:
        decode_01or02(&r, dest, WRITE_TYPE_COLUMNS);
        break;
    case 0x02u:
        decode_01or02(&r, dest, WRITE_TYPE_ROWS);
        break;
    case 0x04u:
        decode_04or05(&r, dest, WRITE_TYPE_COLUMNS);
        bitstream = 1;
        break;
    case 0x05u:
        decode_04or05(&r, dest, WRITE_TYPE_ROWS);
        bitstream = 1;
        break;
    case 0x06u:
        decode_06or07(&r, dest, WRITE_TYPE_COLUMNS);
        break;
    case 0x07u:
        decode_06or07(&r, dest, WRITE_TYPE_ROWS);
        break;
    case 0x08u:
        decode_08or09(&r, dest, WRITE_TYPE_COLUMNS);
        break;
    case 0x09u:
        decode_08or09(&r, dest, WRITE_TYPE_ROWS);
        break;
    case 0x0au:
        decode_0aor0b(&r, dest, WRITE_TYPE_COLUMNS);
        bitstream = 1;
        break;
    case 0x0bu:
        decode_0aor0b(&r, dest, WRITE_TYPE_ROWS);
        bitstream = 1;
        break;
    default:
        /* 0x03 has no known decoder. */
        return 0;
    }

    if (r.bad) {
        return 0;
    }

    if (consumed) {
        /* For a bit-stream frame that ends mid-byte, that partial byte still
           belongs to this frame, so round the consumed count up to it. */
        *consumed = (bitstream && r.mask != 0x80u) ? r.pos + 1u : r.pos;
    }
    return 1;
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

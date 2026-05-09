#include "apexdmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void expect(int condition, const char *message)
{
    if (!condition) {
        fail(message);
    }
}

static void fill_expected_rows(uint8_t *expected)
{
    size_t i;
    for (i = 0; i < APEX_DMD_PAGE_BYTES; i++) {
        expected[i] = (uint8_t)i;
    }
}

static void fill_expected_columns(uint8_t *expected)
{
    size_t row;
    size_t col;
    for (row = 0; row < APEX_DMD_HEIGHT; row++) {
        for (col = 0; col < APEX_DMD_ROW_BYTES; col++) {
            expected[row * APEX_DMD_ROW_BYTES + col] =
                (uint8_t)(col * APEX_DMD_HEIGHT + row);
        }
    }
}

static size_t build_literal_stream(uint8_t *out, uint8_t type, uint8_t special)
{
    size_t pos = 0u;
    unsigned value;

    out[pos++] = type;
    out[pos++] = special;
    for (value = 0u; value < APEX_DMD_PAGE_BYTES; value++) {
        uint8_t ch = (uint8_t)value;
        if (ch == special) {
            out[pos++] = special;
            out[pos++] = 1u;
            out[pos++] = ch;
        } else {
            out[pos++] = ch;
        }
    }
    return pos;
}

static void test_decode_rows(void)
{
    uint8_t src[APEX_DMD_PAGE_BYTES * 3u];
    uint8_t plane[APEX_DMD_PAGE_BYTES];
    uint8_t expected[APEX_DMD_PAGE_BYTES];
    size_t src_size;
    size_t consumed = 0u;
    uint8_t type = 0u;

    fill_expected_rows(expected);
    src_size = build_literal_stream(src, 0x02u, 0xaau);
    expect(apexdmd_decode_fullframe(src, src_size, plane, &consumed, &type),
           "row decode failed");
    expect(type == 0x02u, "row type mismatch");
    expect(consumed == src_size, "row consumed mismatch");
    expect(memcmp(plane, expected, sizeof(expected)) == 0, "row payload mismatch");
}

static void test_decode_columns(void)
{
    uint8_t src[APEX_DMD_PAGE_BYTES * 3u];
    uint8_t plane[APEX_DMD_PAGE_BYTES];
    uint8_t expected[APEX_DMD_PAGE_BYTES];
    size_t src_size;
    size_t consumed = 0u;
    uint8_t type = 0u;

    fill_expected_columns(expected);
    src_size = build_literal_stream(src, 0x01u, 0xaau);
    expect(apexdmd_decode_fullframe(src, src_size, plane, &consumed, &type),
           "column decode failed");
    expect(type == 0x01u, "column type mismatch");
    expect(consumed == src_size, "column consumed mismatch");
    expect(memcmp(plane, expected, sizeof(expected)) == 0, "column payload mismatch");
}

static void test_repeat_zero_means_256(void)
{
    uint8_t src[] = {0x02u, 0xaau, 0xaau, 0x00u, 0x11u, 0xaau, 0x00u, 0x22u};
    uint8_t plane[APEX_DMD_PAGE_BYTES];
    size_t i;

    expect(apexdmd_decode_fullframe(src, sizeof(src), plane, NULL, NULL),
           "repeat-zero decode failed");
    for (i = 0; i < 256u; i++) {
        expect(plane[i] == 0x11u, "repeat-zero first half mismatch");
    }
    for (; i < APEX_DMD_PAGE_BYTES; i++) {
        expect(plane[i] == 0x22u, "repeat-zero second half mismatch");
    }
}

static void test_pbm_write(void)
{
    static const char *path = "out/apexdmd_test.pbm";
    uint8_t plane[APEX_DMD_PAGE_BYTES];
    FILE *in;
    char header[16];
    uint8_t first_row[APEX_DMD_ROW_BYTES];

    memset(plane, 0, sizeof(plane));
    plane[0] = 0x01u;
    expect(apexdmd_write_plane_pbm(path, plane), "pbm write failed");

    in = fopen(path, "rb");
    expect(in != NULL, "pbm reopen failed");
    expect(fread(header, 1, 10, in) == 10u, "pbm header short");
    expect(memcmp(header, "P4\n128 32\n", 10u) == 0, "pbm header mismatch");
    expect(fread(first_row, 1, sizeof(first_row), in) == sizeof(first_row), "pbm row short");
    fclose(in);

    expect(first_row[0] == 0x80u, "pbm bit order mismatch");
}

static void test_pgm_pair_write(void)
{
    static const char *path = "out/apexdmd_pair_test.pgm";
    uint8_t plane0[APEX_DMD_PAGE_BYTES];
    uint8_t plane1[APEX_DMD_PAGE_BYTES];
    FILE *in;
    char header[14];
    uint8_t first_pixels[4];

    memset(plane0, 0, sizeof(plane0));
    memset(plane1, 0, sizeof(plane1));
    plane0[0] = 0x01u;
    plane1[0] = 0x03u;
    expect(apexdmd_write_pair_pgm(path, plane0, plane1), "pgm write failed");

    in = fopen(path, "rb");
    expect(in != NULL, "pgm reopen failed");
    expect(fread(header, 1, 14, in) == 14u, "pgm header short");
    expect(memcmp(header, "P5\n128 32\n255\n", 14u) == 0, "pgm header mismatch");
    expect(fread(first_pixels, 1, sizeof(first_pixels), in) == sizeof(first_pixels), "pgm pixel short");
    fclose(in);

    expect(first_pixels[0] == 255u, "pgm pixel0 mismatch");
    expect(first_pixels[1] == 127u, "pgm pixel1 mismatch");
    expect(first_pixels[2] == 0u, "pgm pixel2 mismatch");
    expect(first_pixels[3] == 0u, "pgm pixel3 mismatch");
}

int main(void)
{
    test_decode_rows();
    test_decode_columns();
    test_repeat_zero_means_256();
    test_pbm_write();
    test_pgm_pair_write();
    return 0;
}

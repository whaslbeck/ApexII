#include "apexsprite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *m) { fprintf(stderr, "%s\n", m); exit(1); }
static void expect(int c, const char *m) { if (!c) fail(m); }

/* width=10 -> row_bytes=2, height=3 -> page=6 */
#define W 10u
#define H 3u
#define PAGE 6u

int main(void)
{
    uint8_t dest[APEX_SPRITE_MAX_BYTES];
    uint8_t enc;
    uint8_t w, h;
    size_t consumed;
    uint8_t buf[64];

    /* ---- 0x00 monochrome: 5 + page ---- */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x00; buf[3] = H; buf[4] = W;
    expect(apexsprite_decode(buf, sizeof(buf), dest, NULL, NULL, NULL, &w, &h, &enc, &consumed),
           "mono decode failed");
    expect(consumed == 5u + PAGE, "mono consumed wrong");
    expect(enc == APEX_SPRITE_ENC_MONO, "mono enc wrong");
    expect(w == W && h == H, "mono dims wrong");

    /* ---- 0xFD: 5 + page, enc fd ---- */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xFD; buf[3] = H; buf[4] = W;
    expect(apexsprite_decode(buf, sizeof(buf), dest, NULL, NULL, NULL, NULL, NULL, &enc, &consumed),
           "fd decode failed");
    expect(consumed == 5u + PAGE, "fd consumed wrong");
    expect(enc == APEX_SPRITE_ENC_FD, "fd enc wrong");

    /* ---- 0xFE bicolor-indirect: 5 + 2 + page ---- */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xFE; buf[3] = H; buf[4] = W;
    expect(apexsprite_decode(buf, sizeof(buf), dest, NULL, NULL, NULL, NULL, NULL, &enc, &consumed),
           "indirect decode failed");
    expect(consumed == 5u + 2u + PAGE, "indirect consumed wrong");
    expect(enc == APEX_SPRITE_ENC_BICOLOR_INDIRECT, "indirect enc wrong");

    /* ---- 0xFF bicolor-direct: 5 + 2*page, plane0|plane1 OR-ed into dest ---- */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xFF; buf[3] = H; buf[4] = W;
    {
        size_t i;
        for (i = 0; i < PAGE; i++) buf[5u + i]        = 0x0F; /* plane1 */
        for (i = 0; i < PAGE; i++) buf[5u + PAGE + i] = 0xF0; /* plane0 */
    }
    expect(apexsprite_decode(buf, sizeof(buf), dest, NULL, NULL, NULL, NULL, NULL, &enc, &consumed),
           "direct decode failed");
    expect(consumed == 5u + 2u * PAGE, "direct consumed wrong");
    expect(enc == APEX_SPRITE_ENC_BICOLOR_DIRECT, "direct enc wrong");
    {
        /* dest holds plane 0 (the second inline plane); plane 1 is at offset 5. */
        size_t i;
        for (i = 0; i < PAGE; i++)
            expect(dest[i] == 0xF0u, "direct plane0 wrong");
    }

    /* ---- 0xFF must fail when only one plane fits ---- */
    expect(!apexsprite_decode(buf, 5u + PAGE, dest, NULL, NULL, NULL, NULL, NULL, &enc, &consumed),
           "direct should fail with one plane of data");

    /* ---- no-header: 1 + page (height from table) ---- */
    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)W;
    expect(apexsprite_decode_noheader(buf, sizeof(buf), dest, (uint8_t)H, &w, &consumed),
           "noheader decode failed");
    expect(consumed == 1u + PAGE, "noheader consumed wrong");
    expect(w == W, "noheader width wrong");

    printf("apexsprite_test: OK\n");
    return 0;
}

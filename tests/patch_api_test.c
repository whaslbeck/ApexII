/* patch_api_test — the apex_patch editing API the Patch window uses:
 * add / move / remove ops, readable addresses, building against an explicit
 * original image while the project holds other bytes, and the romset lookup.
 *
 *   patch_api_test <patch_base.rom> <patch_base.ini> <patch_ops.patch.ini>
 */
#include "apex.h"
#include "apex_patch.h"
#include "apex_project.h"
#include "apex_rominfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* A one-member stored zip with the given name and CRC (enough for the
   central-directory lookup). */
static void write_zip(const char *path, const char *name, uint32_t crc) {
    FILE *f = fopen(path, "wb");
    size_t nl = strlen(name);
    uint8_t cd[46] = {0x50, 0x4b, 0x01, 0x02};
    uint8_t eocd[22] = {0x50, 0x4b, 0x05, 0x06};
    cd[16] = (uint8_t)crc;
    cd[17] = (uint8_t)(crc >> 8);
    cd[18] = (uint8_t)(crc >> 16);
    cd[19] = (uint8_t)(crc >> 24);
    cd[28] = (uint8_t)nl;
    eocd[10] = 1;                   /* entries */
    eocd[12] = (uint8_t)(46u + nl); /* central directory size */
    eocd[16] = 0;                   /* central directory offset */
    fwrite(cd, 1, sizeof(cd), f);
    fwrite(name, 1, nl, f);
    fwrite(eocd, 1, sizeof(eocd), f);
    fclose(f);
}

int main(int argc, char **argv) {
    char err[512], at[64], name[64];
    ApexPatch patch;
    ApexPatchBuild b1, b2;
    if (argc != 4) {
        fputs("usage: patch_api_test <rom> <ini> <patch.ini>\n", stderr);
        return 2;
    }
    ApexProject *p = apex_project_open(argv[1], argv[2]);
    CHECK(p && apex_project_analyze(p) == 0);
    CHECK(apex_patch_load(argv[3], &patch, err, sizeof(err)) == 0);

    /* readable addresses: nearest code label + offset */
    apex_patch_format_at(p, 0xffu, 0x8103u, at, sizeof(at));
    CHECK(!strcmp(at, "Check+0x3"));
    {
        /* several labels share 0x8000 (vector entries): any of them, as long
           as it resolves back to the address */
        uint8_t bk;
        uint32_t ad;
        apex_patch_format_at(p, 0xffu, 0x8000u, at, sizeof(at));
        CHECK(strchr(at, '+') == NULL &&
              apex_patch_resolve_addr(p, at, &bk, &ad, err, sizeof(err)) && bk == 0xffu &&
              ad == 0x8000u);
    }

    /* add (unique names), move, remove */
    long a = apex_patch_add_op(&patch, APEX_PATCH_NOP, "nop", "Start+0x2");
    long c = apex_patch_add_op(&patch, APEX_PATCH_NOP, "nop", "Start+0xe");
    CHECK(a == 4 && c == 5);
    CHECK(!strcmp(patch.ops[4].name, "nop") && !strcmp(patch.ops[5].name, "nop_2"));
    CHECK(apex_patch_move_op(&patch, 5, -1) == 1 && !strcmp(patch.ops[4].name, "nop_2"));
    CHECK(apex_patch_move_op(&patch, 5, 1) == 0);
    apex_patch_remove_op(&patch, 4);
    CHECK(patch.count == 5 && !strcmp(patch.ops[4].name, "nop"));

    /* build against the original while the project holds other bytes */
    CHECK(apex_patch_build(&patch, p, &b1) == 0);
    uint8_t *orig = malloc(p->rom.size);
    memcpy(orig, p->rom.data, p->rom.size);
    memcpy((void *)p->rom.data, b1.rom, p->rom.size); /* "patched view" */
    CHECK(apex_patch_build_base(&patch, p, orig, p->rom.size, &b2) == 0);
    CHECK(b1.rom_size == b2.rom_size && !memcmp(b1.rom, b2.rom, b1.rom_size));
    CHECK(b2.ops[4].length == 3 && b2.ops[4].orig[0] == 0xb7); /* NOP over STA */
    memcpy((void *)p->rom.data, orig, p->rom.size);

    /* romset member by CRC */
    uint32_t crc = ari_crc32(orig, p->rom.size);
    write_zip("patch_api_test.zip", "game_l1.rom", crc);
    CHECK(apex_patch_romset_member("patch_api_test.zip", crc, name, sizeof(name)) == 1 &&
          !strcmp(name, "game_l1.rom"));
    CHECK(apex_patch_romset_member("patch_api_test.zip", crc ^ 1u, name, sizeof(name)) == 0);
    remove("patch_api_test.zip");

    /* an op as "Add op" makes it (type and at only) must not crash the build:
       a missing key is an error of that op */
    for (int t = 0; t < APEX_PATCH_TYPE_COUNT; t++) {
        ApexPatch bare;
        ApexPatchBuild bb;
        memset(&bare, 0, sizeof(bare));
        apex_patch_add_op(&bare, t, apex_patch_type_name(t), "0x8000");
        apex_patch_build(&bare, p, &bb);
        CHECK(bb.count == 1);
        if (t != APEX_PATCH_MOVE && t != APEX_PATCH_NOP && t != APEX_PATCH_BRANCH)
            CHECK(bb.ops[0].status == APEX_PATCH_ERROR && bb.errors >= 1 &&
                  !strncmp(bb.ops[0].msg, "missing ", 8));
        apex_patch_build_free(&bb);
        apex_patch_free(&bare);
    }

    free(orig);
    apex_patch_build_free(&b1);
    apex_patch_build_free(&b2);
    apex_patch_free(&patch);
    apex_project_free(p);
    if (failures)
        return 1;
    puts("patch_api_test: OK");
    return 0;
}

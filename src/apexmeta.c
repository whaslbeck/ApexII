/*
 * apexmeta — WPC ROM metadata tool
 *
 * Displays OS version, game version, checksum status, and file hashes.
 * Optionally fixes the checksum or disables the hardware checksum check.
 */

#include "apex_rominfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
        "usage: apexmeta <rom-file> [options]\n"
        "\n"
        "Options:\n"
        "  --fix              Recompute and store a valid checksum (requires -o)\n"
        "  --disable          Set delta=0x00FF to bypass hardware checksum check\n"
        "                     (requires -o)\n"
        "  -o <output.rom>    Write modified ROM to this file\n"
        "  --verify           Exit 0 if checksum valid, 1 if invalid (no output)\n"
    );
    exit(2);
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL;
    const char *out_path = NULL;
    int do_fix = 0, do_disable = 0, do_verify = 0;
    int i, j;
    FILE *f;
    uint8_t *rom;
    size_t rom_size;
    long fsz;
    ApexRomInfo info;

    for (i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--fix")     == 0) do_fix     = 1;
        else if (strcmp(argv[i], "--disable") == 0) do_disable = 1;
        else if (strcmp(argv[i], "--verify")  == 0) do_verify  = 1;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (!rom_path) rom_path = argv[i];
        else usage();
    }
    if (!rom_path) usage();
    if ((do_fix || do_disable) && !out_path) {
        fprintf(stderr, "error: --fix / --disable require -o <output>\n");
        return 2;
    }
    if (do_fix && do_disable) {
        fprintf(stderr, "error: --fix and --disable are mutually exclusive\n");
        return 2;
    }

    f = fopen(rom_path, "rb");
    if (!f) { perror(rom_path); return 1; }
    fseek(f, 0, SEEK_END); fsz = ftell(f); rewind(f);
    if (fsz <= 0) { fprintf(stderr, "error: empty file\n"); fclose(f); return 1; }
    rom_size = (size_t)fsz;
    rom = (uint8_t *)malloc(rom_size);
    if (!rom) { fprintf(stderr, "error: out of memory\n"); fclose(f); return 1; }
    if (fread(rom, 1, rom_size, f) != rom_size) {
        fprintf(stderr, "error: read failed\n"); fclose(f); free(rom); return 1;
    }
    fclose(f);

    apex_rominfo_compute(rom, rom_size, &info);

    if (do_verify) {
        int ok = (info.computed_csum == info.stored_csum);
        if (!ok) fprintf(stderr, "INVALID  computed=0x%04X  stored=0x%04X\n",
                         info.computed_csum, info.stored_csum);
        free(rom);
        return ok ? 0 : 1;
    }

    /* ── Display ── */
    printf("ROM:           %s\n", rom_path);
    if (rom_size >= 1048576u)
        printf("Size:          %zu bytes (%zu MB)\n", rom_size, rom_size / 1048576u);
    else
        printf("Size:          %zu bytes (%zu KB)\n", rom_size, rom_size / 1024u);

    printf("\n");

    if (info.os_valid)
        printf("OS Version:    %u.%u\n", (unsigned)info.os_major, (unsigned)info.os_minor);
    else
        printf("OS Version:    unknown  (reset vector 0x%04X)\n", info.reset_addr);

    if (info.game_version[0])
        printf("Game Version:  %s  (offset 0x%zX)\n",
               info.game_version, info.game_version_offset);
    else
        printf("Game Version:  not found\n");

    printf("\n");
    printf("Checksum:\n");
    printf("  Stored:      0x%04X  (CPU 0xFFEE / file +0x%zX)\n",
           info.stored_csum, info.csum_file_off);
    printf("  Computed:    0x%04X\n", info.computed_csum);
    printf("  Status:      %s\n",
           info.computed_csum == info.stored_csum ? "VALID" : "INVALID");
    printf("  Delta:       0x%04X  (CPU 0xFFEC / file +0x%zX)%s\n",
           info.stored_delta, info.delta_file_off,
           info.stored_delta == APEX_ROMINFO_DISABLE_DELTA ? "  [check disabled]" : "");

    printf("\n");
    printf("Hashes:\n");
    printf("  CRC-32:      %08X\n", info.crc32_val);
    printf("  SHA-1:       ");
    for (j = 0; j < 20; j++) printf("%02x", info.sha1[j]);
    printf("\n");
    printf("  SHA-256:     ");
    for (j = 0; j < 32; j++) printf("%02x", info.sha256[j]);
    printf("\n");

    /* ── Modify ── */
    if (do_disable) {
        if (info.stored_delta == APEX_ROMINFO_DISABLE_DELTA) {
            printf("\nChecksum already disabled.\n");
        } else {
            ari_w16(rom, info.delta_file_off, APEX_ROMINFO_DISABLE_DELTA);
            printf("\nDelta set to 0x00FF — hardware checksum check disabled.\n");
        }
    } else if (do_fix) {
        if (info.computed_csum == info.stored_csum) {
            printf("\nChecksum already valid — no change needed.\n");
        } else if (apex_rominfo_fix_checksum(rom, rom_size)) {
            uint16_t nc = ari_r16(rom, rom_size - APEX_ROMINFO_CSUM_END);
            uint16_t nd = ari_r16(rom, rom_size - APEX_ROMINFO_DELTA_END);
            printf("\nChecksum fixed:  checksum=0x%04X  delta=0x%04X\n", nc, nd);
        } else {
            fprintf(stderr, "error: could not find a valid checksum solution\n");
            free(rom); return 1;
        }
    }

    if (out_path) {
        FILE *of = fopen(out_path, "wb");
        if (!of) { perror(out_path); free(rom); return 1; }
        if (fwrite(rom, 1, rom_size, of) != rom_size) {
            fprintf(stderr, "error: write failed\n"); fclose(of); free(rom); return 1;
        }
        fclose(of);
        printf("Written: %s\n", out_path);
    }

    free(rom);
    return 0;
}

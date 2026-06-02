#include "apex_match.h"
#include "apex_project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
        "usage: apexmatch <source.rom> <source.ini> <target.rom> [options]\n"
        "\n"
        "Options:\n"
        "  --min-confidence N     skip matches below N%% (default: %d)\n"
        "  --min-instrs N         min instructions for medium-confidence matches (default: 5)\n"
        "  --inject-paged         also inject paged-bank entries into target analysis\n"
        "                         (default: system-bank only — safer for cross-game)\n"
        "  --scan                 scan entire target system bank for exact matches\n"
        "                         finds functions at different addresses (e.g. Delay_N)\n"
        "  --output FILE          write .ini to FILE instead of stdout\n"
        "  --stats                print match statistics to stderr\n"
        "  --verbose              list unmatched named source functions to stderr\n",
        APEX_MATCH_CONF_MEDIUM
    );
    exit(1);
}

int main(int argc, char **argv)
{
    const char *src_rom  = NULL;
    const char *src_ini  = NULL;
    const char *dst_rom  = NULL;
    const char *out_path = NULL;
    int min_conf         = APEX_MATCH_CONF_MEDIUM;
    int min_instrs       = 5;
    int inject_paged     = 0;
    int scan_bank        = 0;
    int print_stats      = 0;
    int verbose          = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--min-confidence") == 0 && i + 1 < argc) {
            min_conf = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--min-instrs") == 0 && i + 1 < argc) {
            min_instrs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--inject-paged") == 0) {
            inject_paged = 1;
        } else if (strcmp(argv[i], "--scan") == 0) {
            scan_bank = 1;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--stats") == 0) {
            print_stats = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (!src_rom) {
            src_rom = argv[i];
        } else if (!src_ini) {
            src_ini = argv[i];
        } else if (!dst_rom) {
            dst_rom = argv[i];
        } else {
            usage();
        }
    }
    if (!src_rom || !src_ini || !dst_rom) usage();

    /* Load and analyse source project */
    ApexProject *src = apex_project_open(src_rom, src_ini);
    if (!src) { fprintf(stderr, "error: cannot open source %s\n", src_rom); return 1; }
    if (apex_project_analyze(src)) { fprintf(stderr, "error: source analysis failed\n"); return 1; }

    /* Load target, inject source entry points, then analyse */
    ApexProject *dst = apex_project_open(dst_rom, NULL);
    if (!dst) { fprintf(stderr, "error: cannot open target %s\n", dst_rom); return 1; }

    apex_match_inject_entries(dst, src, !inject_paged);

    if (apex_project_analyze(dst)) { fprintf(stderr, "error: target analysis failed\n"); return 1; }

    /* Build fingerprint databases */
    ApexFingerprintDB *src_db = apex_fingerprint_build(src);
    ApexFingerprintDB *dst_db = apex_fingerprint_build(dst);
    if (!src_db || !dst_db) { fprintf(stderr, "error: out of memory\n"); return 1; }

    if (print_stats || verbose) {
        size_t named = 0;
        for (i = 0; i < (int)apex_fingerprint_count(src_db); i++) {
            if (apex_fingerprint_get(src_db, (size_t)i)->label_name) named++;
        }
        fprintf(stderr, "source fingerprints: %zu  (named: %zu)\n",
                apex_fingerprint_count(src_db), named);
        fprintf(stderr, "target fingerprints: %zu\n", apex_fingerprint_count(dst_db));
    }

    /* Phase 1: fingerprint-DB match */
    size_t result_count = 0;
    ApexMatchResult *results = apex_match_roms(src_db, dst_db,
                                               min_conf, min_instrs,
                                               &result_count);

    if (print_stats) {
        size_t exact = 0, high = 0, medium = 0;
        for (i = 0; i < (int)result_count; i++) {
            if      (results[i].confidence >= APEX_MATCH_CONF_EXACT) exact++;
            else if (results[i].confidence >= APEX_MATCH_CONF_HIGH)  high++;
            else                                                       medium++;
        }
        fprintf(stderr, "phase1 matches: %zu  (exact=%zu  high=%zu  medium=%zu)\n",
                result_count, exact, high, medium);
    }

    /* Phase 2: raw system-bank scan for exact matches not found by flow */
    if (scan_bank) {
        size_t scan_count = 0;
        ApexMatchResult *scan_results = apex_match_scan_system_bank(
            src_db, dst_db, dst, results, result_count, min_instrs, &scan_count);

        if (scan_count > 0) {
            /* Append scan results to existing results */
            ApexMatchResult *combined = realloc(results,
                (result_count + scan_count) * sizeof(*results));
            if (combined) {
                results = combined;
                memcpy(results + result_count, scan_results,
                       scan_count * sizeof(*scan_results));
                result_count += scan_count;
            }
            apex_match_results_free(scan_results, scan_count);
        }

        if (print_stats) {
            fprintf(stderr, "phase2 scan:    %zu additional exact matches\n",
                    scan_count);
            fprintf(stderr, "total matches:  %zu\n", result_count);
        }
    }

    /* Verbose: list named source functions without a match */
    if (verbose) {
        size_t unmatched = 0;
        for (i = 0; i < (int)apex_fingerprint_count(src_db); i++) {
            const ApexFunctionFingerprint *fp = apex_fingerprint_get(src_db, (size_t)i);
            size_t j;
            int found = 0;

            if (!fp->label_name) continue;
            for (j = 0; j < result_count; j++) {
                if (results[j].src_addr == fp->addr &&
                    results[j].src_bank == fp->bank) { found = 1; break; }
            }
            if (!found) {
                fprintf(stderr, "  unmatched: B%02x_A%04x  %s  (%u instrs)\n",
                        fp->bank, fp->addr, fp->label_name, fp->instr_count);
                unmatched++;
            }
        }
        fprintf(stderr, "unmatched named source functions: %zu\n", unmatched);
    }

    /* Write output */
    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) { perror(out_path); return 1; }
    }
    apex_match_write_ini(out, results, result_count, src);
    if (out_path) fclose(out);

    apex_match_results_free(results, result_count);
    apex_fingerprint_free(src_db);
    apex_fingerprint_free(dst_db);
    apex_project_free(src);
    apex_project_free(dst);
    return 0;
}

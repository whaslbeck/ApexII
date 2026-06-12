#include "apex_compare.h"
#include "apex_project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
        "usage: apexcompare <romA> <romB> [iniA] [options]\n"
        "\n"
        "Compare two ROM versions and report code routines, strings and tables\n"
        "as identical / moved / changed / removed (only in A) / added (only in B).\n"
        "Correspondence is structural so it survives address shifts.\n"
        "\n"
        "Options:\n"
        "  --ini-b FILE     config for romB (enables B-only table discovery)\n"
        "  --inject-paged   also inject A's paged-bank entry points into B\n"
        "                   (default: system-bank only — safer when banks shifted)\n"
        "  --min-instrs N   skip code routines shorter than N instrs (default: 5)\n"
        "  --no-code        skip code comparison\n"
        "  --no-strings     skip string comparison\n"
        "  --no-tables      skip table comparison\n"
        "  --show-identical list identical entries too (default: summarised only)\n"
        "  --only STATUS    only print one status (identical/moved/changed/removed/added)\n");
    exit(1);
}

static int status_filter_ok(ApexCompareStatus s, int only)
{
    return only < 0 || (int)s == only;
}

int main(int argc, char **argv)
{
    const char *rom_a = NULL, *rom_b = NULL, *ini_a = NULL, *ini_b = NULL;
    ApexCompareOptions opt;
    ApexCompareReport report;
    ApexProject *a = NULL, *b = NULL;
    int only = -1;
    int inject_paged = 0;
    int i;
    size_t k;

    apex_compare_default_options(&opt);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ini-b") == 0 && i + 1 < argc) {
            ini_b = argv[++i];
        } else if (strcmp(argv[i], "--inject-paged") == 0) {
            inject_paged = 1;
        } else if (strcmp(argv[i], "--min-instrs") == 0 && i + 1 < argc) {
            opt.min_instrs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-code") == 0) {
            opt.include_code = 0;
        } else if (strcmp(argv[i], "--no-strings") == 0) {
            opt.include_strings = 0;
        } else if (strcmp(argv[i], "--no-tables") == 0) {
            opt.include_tables = 0;
        } else if (strcmp(argv[i], "--show-identical") == 0) {
            opt.include_identical = 1;
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            if      (!strcmp(s, "identical")) only = APEX_CMP_IDENTICAL;
            else if (!strcmp(s, "moved"))     only = APEX_CMP_MOVED;
            else if (!strcmp(s, "changed"))   only = APEX_CMP_CHANGED;
            else if (!strcmp(s, "removed"))   only = APEX_CMP_REMOVED;
            else if (!strcmp(s, "added"))     only = APEX_CMP_ADDED;
            else usage();
            opt.include_identical = (only == APEX_CMP_IDENTICAL) ? 1 : opt.include_identical;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            usage();
        } else if (!rom_a) {
            rom_a = argv[i];
        } else if (!rom_b) {
            rom_b = argv[i];
        } else if (!ini_a) {
            ini_a = argv[i];
        } else {
            usage();
        }
    }
    if (!rom_a || !rom_b) {
        usage();
    }

    a = apex_project_open(rom_a, ini_a);
    if (!a || apex_project_analyze(a)) {
        fprintf(stderr, "error: cannot open/analyse %s\n", rom_a);
        return 1;
    }
    b = apex_project_open(rom_b, ini_b);
    if (!b) {
        fprintf(stderr, "error: cannot open %s\n", rom_b);
        apex_project_free(a);
        return 1;
    }
    /* Seed B with A's entry points so its analysis discovers the same code
       regions; otherwise routines only A knows about look spuriously removed. */
    apex_match_inject_entries(b, a, !inject_paged);
    if (apex_project_analyze(b)) {
        fprintf(stderr, "error: cannot analyse %s\n", rom_b);
        apex_project_free(a);
        apex_project_free(b);
        return 1;
    }

    if (apex_compare_run(a, b, &opt, &report) != 0) {
        fprintf(stderr, "error: comparison failed\n");
        apex_project_free(a);
        apex_project_free(b);
        return 1;
    }

    printf("%zu identical, %zu moved, %zu changed, %zu removed, %zu added\n",
           report.n_identical, report.n_moved, report.n_changed,
           report.n_removed, report.n_added);

    for (k = 0; k < report.count; k++) {
        const ApexCompareEntry *e = &report.items[k];
        char a_buf[16] = "--", b_buf[16] = "--";

        if (!status_filter_ok(e->status, only)) {
            continue;
        }
        if (e->has_a) {
            snprintf(a_buf, sizeof(a_buf), "B%02x_A%04x", e->a_bank,
                     (unsigned)e->a_addr & 0xffffu);
        }
        if (e->has_b) {
            snprintf(b_buf, sizeof(b_buf), "B%02x_A%04x", e->b_bank,
                     (unsigned)e->b_addr & 0xffffu);
        }
        printf("%-9s %-6s %-10s -> %-10s  %-24s  %s\n",
               apex_compare_status_name(e->status),
               apex_compare_kind_name(e->kind),
               a_buf, b_buf, e->label, e->detail);
    }

    apex_compare_report_free(&report);
    apex_project_free(a);
    apex_project_free(b);
    return 0;
}

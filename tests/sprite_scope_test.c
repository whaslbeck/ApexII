/* Verifies that classifying a sprite pointer table triggers image
   classification even through an INCREMENTAL (scoped) re-analysis — i.e. the
   inject pass runs in analyze_system_region / analyze_bank_region, not only in
   the full analysis.  Argument: a ROM with a 2-entry ptr16_sprite table at
   Bff_A8000 (tests/sprite_table.asm assembled). */
#include "apex_project.h"
#include "apex_analysis.h"
#include "apex_config.h"

#include <stdio.h>
#include <stdlib.h>

static void fail(const char *m) { fprintf(stderr, "sprite_scope_test: %s\n", m); exit(1); }

int main(int argc, char **argv)
{
    ApexProject *p;
    const DataRange *nohdr;
    const DataRange *hdr;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <sprite_table.rom>\n", argv[0]);
        return 2;
    }
    p = apex_project_open(argv[1], NULL);   /* no config: table unclassified */
    if (!p || apex_project_analyze(p) != 0) fail("open/analyze failed");

    /* Classify the table at a system-bank address -> system-only scope. */
    if (apex_project_set_kind(p, 0, 0xffu, 0x8000u, APEX_KIND_TABLE,
                              "rows[2](ptr16_sprite(3))") != 0)
        fail("set_kind(table) failed");
    if (p->analysis_scope != APEX_ANALYZE_SCOPE_SYSTEM_ONLY)
        fail("expected system-only scope for the table edit");

    if (apex_project_analyze(p) != 0) fail("scoped re-analyze failed");

    /* The image targets must now be classified by the scoped-path inject. */
    nohdr = data_range_at(0xffu, 0x8010u, &p->data_ranges); /* no-header image */
    hdr   = data_range_at(0xffu, 0x8020u, &p->data_ranges); /* 0x00 header image */
    if (!nohdr || nohdr->kind != DATA_SPRITE_NOHEADER || nohdr->length != 3u)
        fail("no-header image not auto-classified via scoped analysis");
    if (!hdr || hdr->kind != DATA_SPRITE)
        fail("header image not auto-classified via scoped analysis");

    apex_project_free(p);

    /* A single DATA_PTR16_SPRITE pointer also classifies its target.  The second
       table entry at Bff_A8002 points at the header image Bff_A8020. */
    {
        ApexProject *p2 = apex_project_open(argv[1], NULL);
        const DataRange *tgt;
        if (!p2 || apex_project_analyze(p2) != 0) fail("p2 open/analyze failed");
        if (apex_project_set_kind(p2, 0, 0xffu, 0x8002u, APEX_KIND_DATA, "ptr16_sprite") != 0)
            fail("set_kind(ptr16_sprite data) failed");
        if (apex_project_analyze(p2) != 0) fail("p2 re-analyze failed");
        tgt = data_range_at(0xffu, 0x8020u, &p2->data_ranges);
        if (!tgt || tgt->kind != DATA_SPRITE)
            fail("single ptr16_sprite target not auto-classified");
        apex_project_free(p2);
    }

    printf("sprite_scope_test: OK\n");
    return 0;
}

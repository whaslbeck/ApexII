#include "apex_project.h"
#include "apex_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

static int line_contains(const ApexRenderedLine *line, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t i;

    if (needle_len == 0 || line->length < needle_len) {
        return 0;
    }
    for (i = 0; i + needle_len <= line->length; i++) {
        if (memcmp(line->text + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const ApexRenderedDocument *doc1;
    const ApexRenderedDocument *doc2;
    const ApexRenderedDocument *doc3;
    const ApexRenderedDocument *doc4;
    ApexProject *project;
    size_t i;
    size_t removed;
    int saw_source_coords = 0;
    int saw_location_line = 0;
    int saw_located_instruction = 0;
    int saw_code_block_line = 0;
    int saw_inline_line_location = 0;
    int saw_data_line_location = 0;
    size_t entry_line_index = 0;
    size_t transition_line_index = 0;
    const ApexRenderedLine *line;
    FILE *saved;
    char overlay_buf[512];
    size_t overlay_len;

    if (argc != 3 && argc != 5 && argc != 7 && argc != 9) {
        fprintf(stderr, "usage: %s ROM CONFIG [BANK_ROM BANK_CONFIG] [PRUNE_ROM PRUNE_CONFIG]"
                        " [SCOPED_DMD_ROM SCOPED_DMD_CONFIG]\n",
                argv[0]);
        return 2;
    }

    project = apex_project_open(argv[1], argv[2]);
    if (apex_project_analyze(project) != 0) {
        fprintf(stderr, "analyze failed\n");
        apex_project_free(project);
        return 1;
    }

    doc1 = apex_project_render(project, 0, 0);
    doc2 = apex_project_render(project, 0, 0);
    if (!doc1 || !doc2 || doc1 != doc2) {
        fprintf(stderr, "render cache unavailable\n");
        apex_project_free(project);
        return 1;
    }
    if (!contains(doc1->text, "Entry:") || !contains(doc1->text, "JSR Helper")) {
        fprintf(stderr, "baseline render missing expected labels\n");
        apex_project_free(project);
        return 1;
    }
    line = apex_render_find_line_by_address(doc1, 0xffu, 0x8000u, &entry_line_index);
    if (!line || line->kind != APEX_RENDER_LINE_LOCATION) {
        fprintf(stderr, "address lookup failed\n");
        apex_project_free(project);
        return 1;
    }
    for (i = 0; i < doc1->line_count; i++) {
        if (doc1->lines[i].kind == APEX_RENDER_LINE_LOCATION && doc1->lines[i].has_location &&
            doc1->lines[i].bank == 0xffu && doc1->lines[i].cpu_addr >= 0x8000u) {
            saw_location_line = 1;
        }
        if (doc1->lines[i].kind == APEX_RENDER_LINE_INSTRUCTION &&
            doc1->lines[i].has_location && line_contains(&doc1->lines[i], "JSR Helper")) {
            saw_located_instruction = 1;
        }
        if (doc1->lines[i].kind == APEX_RENDER_LINE_INSTRUCTION &&
            doc1->lines[i].has_location && line_contains(&doc1->lines[i], "RTS") &&
            doc1->lines[i].block_kind == APEX_RENDER_BLOCK_CODE) {
            saw_code_block_line = 1;
        }
        if (doc1->lines[i].has_location && line_contains(&doc1->lines[i], "INLINE_BYTE") &&
            doc1->lines[i].cpu_addr == 0x8003u) {
            saw_inline_line_location = 1;
        }
        if (doc1->lines[i].has_location && line_contains(&doc1->lines[i], ".DB 0x00") &&
            doc1->lines[i].cpu_addr == 0x8004u) {
            saw_data_line_location = 1;
        }
    }
    if (!saw_location_line || !saw_located_instruction || !saw_code_block_line ||
        !saw_inline_line_location || !saw_data_line_location) {
        fprintf(stderr, "render line metadata missing\n");
        apex_project_free(project);
        return 1;
    }
    line = apex_render_find_next_transition(doc1, entry_line_index,
                                            APEX_RENDER_TRANSITION_CODE_TO_UNCLASSIFIED,
                                            &transition_line_index);
    if (!line || !line->has_location || line->cpu_addr != 0x8004u) {
        fprintf(stderr, "next transition lookup failed\n");
        apex_project_free(project);
        return 1;
    }
    line = apex_render_find_prev_transition(doc1, transition_line_index,
                                            APEX_RENDER_TRANSITION_CODE_TO_UNCLASSIFIED, NULL);
    if (!line || !line->has_location || line->cpu_addr != 0x8004u) {
        fprintf(stderr, "prev transition lookup failed\n");
        apex_project_free(project);
        return 1;
    }
    for (i = 0; i < project->refs.count; i++) {
        if (project->refs.items[i].source_bank == 0xffu &&
            project->refs.items[i].source_addr >= 0x8000u) {
            saw_source_coords = 1;
            break;
        }
    }
    if (!saw_source_coords) {
        fprintf(stderr, "reference source coordinates missing\n");
        apex_project_free(project);
        return 1;
    }
    removed = remove_references_from_source_range(&project->refs, 0xffu, 0x8000u, 0x800bu);
    if (removed == 0) {
        fprintf(stderr, "reference pruning failed\n");
        apex_project_free(project);
        return 1;
    }
    apex_project_invalidate(project, APEX_DIRTY_ANALYSIS | APEX_DIRTY_RENDER);
    if (apex_project_analyze(project) != 0) {
        fprintf(stderr, "reanalyze after pruning failed\n");
        apex_project_free(project);
        return 1;
    }

    if (apex_project_set_label(project, 0, 0xffu, 0x8005u, "HelperRenamed") != 0) {
        fprintf(stderr, "set_label failed\n");
        apex_project_free(project);
        return 1;
    }
    doc3 = apex_project_render(project, 0, 0);
    if (!doc3 || !contains(doc3->text, "HelperRenamed:") ||
        !contains(doc3->text, "JSR HelperRenamed")) {
        fprintf(stderr, "renamed label not reflected in render\n");
        apex_project_free(project);
        return 1;
    }

    if (apex_project_set_doc(project, 0, 0xffu, 0x8005u, "Helper doc") != 0) {
        fprintf(stderr, "set_doc failed\n");
        apex_project_free(project);
        return 1;
    }
    doc4 = apex_project_render(project, 0, 0);
    if (!doc4 || !contains(doc4->text, "; doc Helper doc")) {
        fprintf(stderr, "doc not reflected in render\n");
        apex_project_free(project);
        return 1;
    }
    if (apex_project_save_overlay(project, "out/project_api_test_overlay.ini", NULL) != 0) {
        fprintf(stderr, "save_overlay failed\n");
        apex_project_free(project);
        return 1;
    }
    saved = fopen("out/project_api_test_overlay.ini", "rb");
    if (!saved) {
        fprintf(stderr, "failed to reopen saved overlay\n");
        apex_project_free(project);
        return 1;
    }
    overlay_len = fread(overlay_buf, 1, sizeof(overlay_buf) - 1u, saved);
    fclose(saved);
    overlay_buf[overlay_len] = '\0';
    if (!contains(overlay_buf, "[labels]") || !contains(overlay_buf, "HelperRenamed") ||
        !contains(overlay_buf, "[docs]") || !contains(overlay_buf, "Helper doc")) {
        fprintf(stderr, "saved overlay missing expected content\n");
        apex_project_free(project);
        return 1;
    }

    if (apex_project_clear_doc(project, 0, 0xffu, 0x8005u) != 0 ||
        apex_project_clear_label(project, 0, 0xffu, 0x8005u) != 0) {
        fprintf(stderr, "clear operation failed\n");
        apex_project_free(project);
        return 1;
    }
    doc4 = apex_project_render(project, 0, 0);
    if (!doc4 || contains(doc4->text, "HelperRenamed") || contains(doc4->text, "Helper doc")) {
        fprintf(stderr, "clear operations not reflected in render\n");
        apex_project_free(project);
        return 1;
    }

    if (apex_project_set_label(project, 0, 0xffu, 0x8005u, "HelperInline") != 0 ||
        apex_project_set_inline(project, 0, 0xffu, 0x8005u, "byte") != 0) {
        fprintf(stderr, "set_inline failed\n");
        apex_project_free(project);
        return 1;
    }
    if (project->analysis_scope != APEX_ANALYZE_SCOPE_SYSTEM_ONLY) {
        fprintf(stderr, "system inline edit did not select system-only scope\n");
        apex_project_free(project);
        return 1;
    }
    doc4 = apex_project_render(project, 0, 0);
    if (!doc4 || !contains(doc4->text, "HelperInline:") ||
        !contains(doc4->text, "JSR HelperInline") ||
        !contains(doc4->text, "INLINE_BYTE 0x42")) {
        fprintf(stderr, "inline update not reflected in render\n");
        apex_project_free(project);
        return 1;
    }

    if (apex_project_set_label(project, 0, 0xffu, 0x8004u, "GapData") != 0 ||
        apex_project_set_kind(project, 1, 0xffu, 0x8004u, APEX_KIND_DATA, "bytes[1]") != 0) {
        fprintf(stderr, "set_kind(data) failed\n");
        apex_project_free(project);
        return 1;
    }
    if (project->analysis_scope != APEX_ANALYZE_SCOPE_SYSTEM_ONLY) {
        fprintf(stderr, "system data edit did not select system-only scope\n");
        apex_project_free(project);
        return 1;
    }
    doc4 = apex_project_render(project, 0, 0);
    if (!doc4 || !contains(doc4->text, "GapData:") ||
        !contains(doc4->text, "; data type=bytes length=1")) {
        fprintf(stderr, "data kind update not reflected in render\n");
        apex_project_free(project);
        return 1;
    }

    /* ---- undo / redo ---- */
    if (apex_project_set_label(project, 0, 0xffu, 0x8005u, "UndoProbeLabel") != 0) {
        fprintf(stderr, "undo: set_label failed\n");
        apex_project_free(project);
        return 1;
    }
    doc4 = apex_project_render(project, 0, 0);
    if (!doc4 || !contains(doc4->text, "UndoProbeLabel") || !apex_project_can_undo(project)) {
        fprintf(stderr, "undo: probe label not applied\n");
        apex_project_free(project);
        return 1;
    }
    if (apex_project_undo(project) != 0) {
        fprintf(stderr, "undo: undo failed\n");
        apex_project_free(project);
        return 1;
    }
    doc4 = apex_project_render(project, 0, 0);
    if (!doc4 || contains(doc4->text, "UndoProbeLabel") || !apex_project_can_redo(project)) {
        fprintf(stderr, "undo: edit not reverted\n");
        apex_project_free(project);
        return 1;
    }
    if (apex_project_redo(project) != 0) {
        fprintf(stderr, "undo: redo failed\n");
        apex_project_free(project);
        return 1;
    }
    doc4 = apex_project_render(project, 0, 0);
    if (!doc4 || !contains(doc4->text, "UndoProbeLabel")) {
        fprintf(stderr, "undo: redo did not restore edit\n");
        apex_project_free(project);
        return 1;
    }
    /* grouped edits collapse into a single undo step */
    apex_project_begin_edit_group(project, "grp");
    apex_project_set_label(project, 0, 0xffu, 0x8005u, "GrpLabelA");
    apex_project_set_doc(project, 0, 0xffu, 0x8005u, "grp doc body");
    apex_project_end_edit_group(project);
    doc4 = apex_project_render(project, 0, 0);
    if (!doc4 || !contains(doc4->text, "GrpLabelA") || !contains(doc4->text, "grp doc body")) {
        fprintf(stderr, "undo: grouped edit not applied\n");
        apex_project_free(project);
        return 1;
    }
    if (apex_project_undo(project) != 0) {
        fprintf(stderr, "undo: group undo failed\n");
        apex_project_free(project);
        return 1;
    }
    doc4 = apex_project_render(project, 0, 0);
    if (!doc4 || contains(doc4->text, "GrpLabelA") || contains(doc4->text, "grp doc body") ||
        !contains(doc4->text, "UndoProbeLabel")) {
        fprintf(stderr, "undo: group did not revert in one step\n");
        apex_project_free(project);
        return 1;
    }

    apex_project_free(project);

    if (argc >= 5) {
        project = apex_project_open(argv[3], argv[4]);
        if (apex_project_analyze(project) != 0) {
            fprintf(stderr, "bank analyze failed\n");
            apex_project_free(project);
            return 1;
        }
        if (apex_project_set_inline(project, 1, 0x20u, 0x4006u, "byte") != 0) {
            fprintf(stderr, "bank set_inline failed\n");
            apex_project_free(project);
            return 1;
        }
        if (project->analysis_scope != APEX_ANALYZE_SCOPE_BANK_ONLY ||
            project->analysis_bank_id != 0x20u) {
            fprintf(stderr, "bank inline edit did not select bank-only scope\n");
            apex_project_free(project);
            return 1;
        }
        doc1 = apex_project_render(project, 0, 0);
        if (!doc1 || !contains(doc1->text, "INLINE_BYTE 0x42")) {
            fprintf(stderr, "bank inline update not reflected in render\n");
            apex_project_free(project);
            return 1;
        }
        apex_project_free(project);
    }

    if (argc == 7) {
        project = apex_project_open(argv[5], argv[6]);
        if (apex_project_analyze(project) != 0) {
            fprintf(stderr, "prune analyze failed\n");
            apex_project_free(project);
            return 1;
        }
        doc1 = apex_project_render(project, 0, 0);
        if (!doc1 || !contains(doc1->text, "INLINE_FAR_CODE B21_A4001")) {
            fprintf(stderr, "expected far target reference missing\n");
            apex_project_free(project);
            return 1;
        }
        if (apex_project_set_inline(project, 1, 0x20u, 0x4008u, "byte") != 0) {
            fprintf(stderr, "prune set_inline failed\n");
            apex_project_free(project);
            return 1;
        }
        if (project->analysis_scope != APEX_ANALYZE_SCOPE_BANK_ONLY ||
            project->analysis_bank_id != 0x20u) {
            fprintf(stderr, "prune edit did not select bank-only scope\n");
            apex_project_free(project);
            return 1;
        }
        doc1 = apex_project_render(project, 0, 0);
        if (!doc1 || contains(doc1->text, "INLINE_FAR_CODE B21_A4001")) {
            fprintf(stderr, "stale far target reference was not pruned\n");
            apex_project_free(project);
            return 1;
        }
        apex_project_free(project);
    }

    /* A manual label at an auto-generated address must take over the placeholder
       rather than add a second label there.  B21_A4001 is a generated far-code
       target in local_reanalysis_far; use a fresh project so the prune test
       above is unaffected. */
    if (argc == 7) {
        int bidx;
        LabelSet *ls;
        size_t cnt = 0;

        project = apex_project_open(argv[5], argv[6]);
        if (!project || apex_project_analyze(project) != 0) {
            fprintf(stderr, "takeover: analyze failed\n");
            return 1;
        }
        bidx = bank_index_for_id(project->rom.data, project->banks, 0x21u);
        if (bidx < 0) {
            fprintf(stderr, "takeover: bank 0x21 not found\n");
            apex_project_free(project);
            return 1;
        }
        if (apex_project_set_label(project, 1, 0x21u, 0x4001u, "ManualTakeover") != 0) {
            fprintf(stderr, "takeover: set_label failed\n");
            apex_project_free(project);
            return 1;
        }
        ls = &project->bank_labels[bidx];
        for (i = 0; i < ls->count; i++) {
            if (ls->items[i].addr == 0x4001u) {
                cnt++;
                if (strcmp(ls->items[i].name, "ManualTakeover") != 0) {
                    fprintf(stderr, "takeover: stale label '%s' left at address\n",
                            ls->items[i].name);
                    apex_project_free(project);
                    return 1;
                }
            }
        }
        if (cnt != 1) {
            fprintf(stderr, "takeover: expected 1 label at B21_A4001, found %zu\n", cnt);
            apex_project_free(project);
            return 1;
        }
        doc1 = apex_project_render(project, 0, 0);
        if (!doc1 || contains(doc1->text, "B21_A4001:") ||
            !contains(doc1->text, "ManualTakeover")) {
            fprintf(stderr, "takeover: render still shows the generated label\n");
            apex_project_free(project);
            return 1;
        }
        apex_project_free(project);

        /* Cross-bank code must survive a bank-scoped re-analysis.  In
           local_reanalysis_far, B21_A4001 (Target) is reached ONLY through an
           inline far-code call from bank 0x20, so classifying an unrelated new
           code address in bank 0x21 (which triggers a bank-only re-analysis)
           must not drop it. */
        {
            ApexProject *pr = apex_project_open(argv[5], argv[6]);
            int found_before = 0, found_after = 0;
            int bidx2;
            LabelSet *ls2;
            size_t k;

            if (!pr || apex_project_analyze(pr) != 0) {
                fprintf(stderr, "xbank: analyze failed\n");
                return 1;
            }
            bidx2 = bank_index_for_id(pr->rom.data, pr->banks, 0x21u);
            ls2 = &pr->bank_labels[bidx2];
            for (k = 0; k < ls2->count; k++) {
                if (ls2->items[k].addr == 0x4001u && ls2->items[k].is_code) {
                    found_before = 1;
                }
            }
            if (!found_before) {
                fprintf(stderr, "xbank: Target not code in full analysis\n");
                apex_project_free(pr);
                return 1;
            }
            if (apex_project_set_kind(pr, 1, 0x21u, 0x4002u, APEX_KIND_CODE, NULL) != 0 ||
                apex_project_analyze(pr) != 0) {
                fprintf(stderr, "xbank: scoped re-analysis failed\n");
                apex_project_free(pr);
                return 1;
            }
            bidx2 = bank_index_for_id(pr->rom.data, pr->banks, 0x21u);
            ls2 = &pr->bank_labels[bidx2];
            for (k = 0; k < ls2->count; k++) {
                if (ls2->items[k].addr == 0x4001u && ls2->items[k].is_code) {
                    found_after = 1;
                }
            }
            if (!found_after) {
                fprintf(stderr, "xbank: cross-bank code dropped by scoped re-analysis\n");
                apex_project_free(pr);
                return 1;
            }
            apex_project_free(pr);
        }
    }

    /* Injected sprite/DMD data labels must survive a bank-scoped re-analysis of
       an UNRELATED bank.  In scoped_dmd, a far-DMD table in bank 0x20 injects a
       DATA_DMD_FULLFRAME (with no inbound reference) in bank 0x21; classifying
       something in bank 0x22 triggers a bank-only re-analysis whose prune pass
       must not drop the (pinned) DMD label in bank 0x21. */
    if (argc == 9) {
        ApexProject *pr = apex_project_open(argv[7], argv[8]);
        int bidx;
        LabelSet *ls;
        size_t k;
        int dmd_before = 0, spr_before = 0, dmd_after = 0, spr_after = 0;

        if (!pr || apex_project_analyze(pr) != 0) {
            fprintf(stderr, "scoped_dmd: analyze failed\n");
            return 1;
        }
        bidx = bank_index_for_id(pr->rom.data, pr->banks, 0x21u);
        ls = &pr->bank_labels[bidx];
        for (k = 0; k < ls->count; k++) {
            if (ls->items[k].addr == 0x4001u && ls->items[k].is_data) dmd_before = 1;
            if (ls->items[k].addr == 0x4100u && ls->items[k].is_data) spr_before = 1;
        }
        if (!dmd_before || !spr_before) {
            fprintf(stderr, "scoped_dmd: dmd/sprite label missing in full analysis\n");
            apex_project_free(pr);
            return 1;
        }
        /* classify an unrelated byte in bank 0x22 -> bank-scoped re-analysis */
        if (apex_project_set_kind(pr, 1, 0x22u, 0x4001u, APEX_KIND_DATA, "bytes[2]") != 0 ||
            apex_project_analyze(pr) != 0) {
            fprintf(stderr, "scoped_dmd: scoped re-analysis failed\n");
            apex_project_free(pr);
            return 1;
        }
        bidx = bank_index_for_id(pr->rom.data, pr->banks, 0x21u);
        ls = &pr->bank_labels[bidx];
        for (k = 0; k < ls->count; k++) {
            if (ls->items[k].addr == 0x4001u && ls->items[k].is_data) dmd_after = 1;
            if (ls->items[k].addr == 0x4100u && ls->items[k].is_data) spr_after = 1;
        }
        if (!dmd_after || !spr_after) {
            fprintf(stderr, "scoped_dmd: dmd/sprite label dropped by scoped re-analysis\n");
            apex_project_free(pr);
            return 1;
        }
        apex_project_free(pr);
    }

    return 0;
}

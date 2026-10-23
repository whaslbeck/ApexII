/* Text output of a patch build: the per-op diff (apexpatch diff) and the
   Markdown report (apexpatch report, the Patch window's Report button). */
#include "apex.h"
#include "apex_patch.h"
#include "apex_project.h"
#include "apex_rominfo.h"
#include "cpu6809.h"

#include <stdio.h>
#include <string.h>

const char *apex_patch_status_word(int st) {
    switch (st) {
    case APEX_PATCH_OK:
        return "ok";
    case APEX_PATCH_WARN:
        return "warn";
    case APEX_PATCH_ERROR:
        return "ERROR";
    default:
        return "off";
    }
}

static void fputc_f(FILE *f, int c) {
    fputc(c, f);
}

/* Disassemble n bytes at addr (one line per instruction). */
static void print_code(FILE *f, const char *tag, const uint8_t *b, size_t n, uint32_t addr) {
    size_t pos = 0;
    while (pos < n) {
        char inst[64];
        size_t k = cpu6809_disassemble(b + pos, n - pos, addr + (uint32_t)pos, inst, sizeof(inst));
        if (!k || pos + k > n) {
            fprintf(f, "    %s %04x  .DB 0x%02x\n", tag, (unsigned)(addr + pos), b[pos]);
            pos++;
            continue;
        }
        /* a run of NOPs on one line */
        if (b[pos] == 0x12u) {
            size_t r = 1;
            while (pos + r < n && b[pos + r] == 0x12u)
                r++;
            if (r > 1) {
                fprintf(f, "    %s %04x  NOP x%lu\n", tag, (unsigned)(addr + pos),
                        (unsigned long)r);
                pos += r;
                continue;
            }
        }
        fprintf(f, "    %s %04x  %s\n", tag, (unsigned)(addr + pos), inst);
        pos += k;
    }
}

/* String bytes as text (escapes for the control characters strings hold). */
static void print_text(FILE *f, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n && b[i]; i++) {
        if (b[i] == 0x0a)
            fputs("\\n", f);
        else if (b[i] == 0x09)
            fputs("\\t", f);
        else if (b[i] == 0x07)
            fputs("\\a", f);
        else
            fputc_f(f, b[i]);
    }
}

/* The references an op rewrote (or could not). */
static void print_fixups(FILE *f, const ApexPatchBuild *build, const ApexPatchOpResult *r) {
    static const char *st[] = {"ok", "MANUAL", "UNREACHABLE"};
    for (size_t k = 0; k < r->fixup_count; k++) {
        const ApexPatchFixup *x = &build->fixups[r->fixup_first + k];
        fprintf(f, "    ref B%02x_A%04x %-12s %s B%02x_A%04x -> B%02x_A%04x  %s\n", x->src_bank,
                (unsigned)x->src_addr, x->what, x->far ? "far " : "word", x->old_bank,
                (unsigned)x->old_addr, x->new_bank, (unsigned)x->new_addr, st[x->status]);
    }
}

void apex_patch_write_diff_op(FILE *f, const ApexPatch *patch, const ApexPatchBuild *build,
                              size_t i) {
    {
        const ApexPatchOp *o = &patch->ops[i];
        const ApexPatchOpResult *r = &build->ops[i];
        fprintf(f, "[%s] %s %s", o->name, apex_patch_type_name(o->type),
                apex_patch_status_word(r->status));
        if (!r->length) {
            fprintf(f, "%s%s\n", r->msg[0] ? "  " : "", r->msg);
            return;
        }
        fprintf(f, "  B%02x_A%04x (file 0x%06lx), %lu byte(s)\n", r->bank, (unsigned)r->addr,
                (unsigned long)r->rom_off, (unsigned long)r->length);
        if (o->type == APEX_PATCH_STRING) {
            fprintf(f, "    - \"");
            print_text(f, r->guard, r->guard_len);
            fprintf(f, "\"\n    + \"%s\"  (%s", o->text, r->mode_used);
            if (r->has_alloc)
                fprintf(f, " to B%02x_A%04x", r->alloc_bank, (unsigned)r->alloc_addr);
            fprintf(f, ")\n");
            print_fixups(f, build, r);
        } else if (o->type == APEX_PATCH_DMD || o->type == APEX_PATCH_SPRITE) {
            fprintf(f, "    %s from %s: %u -> %lu byte(s), %s",
                    o->type == APEX_PATCH_DMD ? "frame" : "sprite", o->file,
                    (unsigned)r->old_length,
                    (unsigned long)(r->has_alloc ? r->alloc_length : r->length), r->mode_used);
            if (r->has_alloc)
                fprintf(f, " to B%02x_A%04x", r->alloc_bank, (unsigned)r->alloc_addr);
            fputc_f(f, '\n');
            print_fixups(f, build, r);
        } else if (o->type == APEX_PATCH_BLOB) {
            fprintf(f, "    %s: %lu byte(s) at B%02x_A%04x\n", o->file,
                    (unsigned long)(r->has_alloc ? r->alloc_length : r->length),
                    r->has_alloc ? r->alloc_bank : r->bank,
                    (unsigned)(r->has_alloc ? r->alloc_addr : r->addr));
        } else if (o->type == APEX_PATCH_MOVE) {
            fprintf(f, "    %lu byte(s) to B%02x_A%04x, old place cleared to 0xFF\n",
                    (unsigned long)r->alloc_length, r->alloc_bank, (unsigned)r->alloc_addr);
            print_fixups(f, build, r);
        } else if (o->type == APEX_PATCH_HOOK && r->has_alloc) {
            print_code(f, "-", r->orig, r->length, r->addr);
            print_code(f, "+", r->patched, r->length, r->addr);
            fprintf(f, "    detour B%02x_A%04x:\n", r->alloc_bank, (unsigned)r->alloc_addr);
            print_code(f, "+", r->alloc_bytes, r->alloc_length, r->alloc_addr);
        } else if (o->type == APEX_PATCH_BYTES || o->type == APEX_PATCH_PTR ||
                   o->type == APEX_PATCH_VALUE) {
            fprintf(f, "    - ");
            for (size_t k = 0; k < r->length; k++)
                fprintf(f, "%02x ", r->orig[k]);
            fprintf(f, "\n    + ");
            for (size_t k = 0; k < r->length; k++)
                fprintf(f, "%02x ", r->patched[k]);
            fputc_f(f, '\n');
        } else {
            print_code(f, "-", r->orig, r->length, r->addr);
            print_code(f, "+", r->patched, r->length, r->addr);
        }
    }
}

void apex_patch_write_report(FILE *f, const char *title, const ApexPatch *patch,
                             const ApexProject *project, const uint8_t *orig,
                             const ApexPatchBuild *build) {
    const ApexPatch *pt = patch;
    const ApexPatchBuild *bd = build;
    size_t size = project->rom.size, changed = 0, i;
    if (bd->rom)
        for (i = 0; i < size; i++)
            changed += orig[i] != bd->rom[i];
    const char *slash = strrchr(title, '/');
    fprintf(f, "# Patch report: %s\n\n", slash ? slash + 1 : title);
    fprintf(f, "| | |\n|---|---|\n");
    fprintf(f, "| Base ROM | `%s` (%lu KB, CRC-32 %08X) |\n", pt->base_rom,
            (unsigned long)(size / 1024u), (unsigned)ari_crc32(orig, size));
    if (pt->analysis)
        fprintf(f, "| Analysis | `%s` |\n", pt->analysis);
    static const char *const cks[] = {"fix", "disable", "keep"};
    fprintf(f, "| Checksum | %s |\n",
            cks[pt->checksum < 3 && pt->checksum >= 0 ? pt->checksum : 0]);
    if (bd->rom)
        fprintf(f, "| Patched ROM | CRC-32 %08X, %lu byte(s) changed |\n",
                (unsigned)ari_crc32(bd->rom, size), (unsigned long)changed);
    fprintf(f, "| Ops | %lu: %d error(s), %d warning(s) |\n\n", (unsigned long)pt->count,
            bd->errors, bd->warnings);
    if (bd->msg[0])
        fprintf(f, "**Error:** %s\n\n", bd->msg);

    fprintf(f, "## Ops\n\n| # | Op | Type | Where | Bytes | Status | Note |\n"
               "|---|---|---|---|---|---|---|\n");
    for (i = 0; i < pt->count; i++) {
        const ApexPatchOp *o = &pt->ops[i];
        const ApexPatchOpResult *r = &bd->ops[i];
        char where[64] = "";
        /* allocated code / blobs: the site is the allocation itself */
        int same = r->has_alloc && r->length && r->rom_off == r->alloc_rom_off;
        size_t bytes = r->length + (r->has_alloc && !same ? r->alloc_length : 0u);
        if (r->length)
            snprintf(where, sizeof(where), "B%02x_A%04x", r->bank, (unsigned)r->addr);
        else if (o->at)
            snprintf(where, sizeof(where), "%s%s", o->type == APEX_PATCH_RAM ? "RAM " : "", o->at);
        if (same)
            ;
        else if (r->has_alloc && o->type != APEX_PATCH_STRING && o->type != APEX_PATCH_DMD &&
                 o->type != APEX_PATCH_SPRITE && o->type != APEX_PATCH_MOVE &&
                 o->type != APEX_PATCH_BLOB)
            snprintf(where + strlen(where), sizeof(where) - strlen(where), "%sB%02x_A%04x",
                     r->length ? " + " : "", r->alloc_bank, (unsigned)r->alloc_addr);
        else if (r->has_alloc)
            snprintf(where + strlen(where), sizeof(where) - strlen(where), "%s B%02x_A%04x",
                     r->length ? " ->" : "", r->alloc_bank, (unsigned)r->alloc_addr);
        fprintf(f, "| %lu | %s | %s | %s | %lu | %s | %s |\n", (unsigned long)i + 1u, o->name,
                apex_patch_type_name(o->type), where, (unsigned long)bytes,
                apex_patch_status_word(r->status), o->note ? o->note : "");
    }

    fprintf(f, "\n## Changes\n");
    for (i = 0; i < pt->count; i++) {
        const ApexPatchOp *o = &pt->ops[i];
        const ApexPatchOpResult *r = &bd->ops[i];
        fprintf(f, "\n### %s\n\n", o->name);
        if (o->note)
            fprintf(f, "%s\n\n", o->note);
        if (r->msg[0])
            fprintf(f, "*%s:* %s\n\n", apex_patch_status_word(r->status), r->msg);
        if (r->status == APEX_PATCH_ERROR || r->status == APEX_PATCH_DISABLED)
            continue;
        fputs("```\n", f);
        apex_patch_write_diff_op(f, pt, bd, i);
        fputs("```\n", f);
    }

    if (bd->sym_count) {
        fprintf(f, "\n## Symbols defined by the patch\n\n| Symbol | Where | Kind |\n"
                   "|---|---|---|\n");
        for (i = 0; i < bd->sym_count; i++) {
            const ApexPatchSym *sy = &bd->syms[i];
            if (sy->is_ram)
                fprintf(f, "| %s | 0x%04x | RAM |\n", sy->name, (unsigned)sy->addr);
            else
                fprintf(f, "| %s | B%02x_A%04x | %s |\n", sy->name, sy->bank, (unsigned)sy->addr,
                        sy->is_code ? "code" : "data");
        }
    }

    unsigned long tot[256] = {0}, left[256] = {0};
    for (i = 0; i < bd->free_total.count; i++)
        tot[bd->free_total.items[i].bank] += bd->free_total.items[i].len;
    for (i = 0; i < bd->free_left.count; i++)
        left[bd->free_left.items[i].bank] += bd->free_left.items[i].len;
    fprintf(f, "\n## Free space\n\n| Bank | Free | Used by the patch | Left |\n"
               "|---|---|---|---|\n");
    for (int b = 0; b < 256; b++) {
        if (tot[b] && tot[b] != left[b])
            fprintf(f, "| %02x | %lu | %lu | %lu |\n", b, tot[b], tot[b] - left[b], left[b]);
    }
    unsigned long all_t = 0, all_l = 0;
    for (int b = 0; b < 256; b++) {
        all_t += tot[b];
        all_l += left[b];
    }
    fprintf(f, "| all | %lu | %lu | %lu |\n", all_t, all_t - all_l, all_l);
}

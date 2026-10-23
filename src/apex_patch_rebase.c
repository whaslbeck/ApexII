/* Rebase a patch onto another version of the ROM: find every op's site in the
   target image and write a patch file for it.  See apex_patch_rebase(). */
#include "apex.h"
#include "apex_analysis.h"
#include "apex_match.h"
#include "apex_patch.h"
#include "apex_project.h"
#include "apex_rominfo.h"
#include "cpu6809.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t off_of(const ApexProject *p, uint8_t bank, uint32_t addr) {
    if (bank == 0xffu)
        return p->paged_size + (addr - APEX_SYSTEM_ORG);
    int i = bank_index_for_id(p->rom.data, p->banks, bank);
    return i < 0 ? (size_t)-1 : (size_t)i * APEX_BANK_SIZE + (addr - APEX_PAGED_ORG);
}

static void loc_of(const ApexProject *p, size_t off, uint8_t *bank, uint32_t *addr) {
    if (off >= p->paged_size) {
        *bank = 0xffu;
        *addr = APEX_SYSTEM_ORG + (uint32_t)(off - p->paged_size);
    } else {
        *bank = bank_id_for_index(p->banks, (int)(off / APEX_BANK_SIZE));
        *addr = APEX_PAGED_ORG + (uint32_t)(off % APEX_BANK_SIZE);
    }
}

/* [start, end) file offsets of the bank that holds off. */
static void bank_span(const ApexProject *p, size_t off, size_t *start, size_t *end) {
    if (off >= p->paged_size) {
        *start = p->paged_size;
        *end = p->rom.size;
    } else {
        *start = off / APEX_BANK_SIZE * APEX_BANK_SIZE;
        *end = *start + APEX_BANK_SIZE;
    }
}

/* Unique position of needle in the target (in banks of the same kind:
   paged / system), as the offset of needle[0]; -1 none, -2 ambiguous. */
static long find_unique(const ApexProject *d, const uint8_t *needle, size_t n, int system) {
    size_t lo = system ? d->paged_size : 0, hi = system ? d->rom.size : d->paged_size;
    long found = -1;
    if (n == 0 || hi - lo < n)
        return -1;
    for (size_t i = lo; i + n <= hi; i++) {
        if (d->rom.data[i] != needle[0] || memcmp(d->rom.data + i, needle, n))
            continue;
        size_t bs, be;
        bank_span(d, i, &bs, &be);
        if (i + n > be) /* must not straddle a bank */
            continue;
        if (found >= 0)
            return -2;
        found = (long)i;
    }
    return found;
}

/* Like find_unique, ignoring the bytes where mask is 0. */
static long find_unique_masked(const ApexProject *d, const uint8_t *needle, const uint8_t *mask,
                               size_t n, int system) {
    size_t lo = system ? d->paged_size : 0, hi = system ? d->rom.size : d->paged_size;
    long found = -1;
    if (n == 0 || hi - lo < n)
        return -1;
    for (size_t i = lo; i + n <= hi; i++) {
        size_t k;
        for (k = 0; k < n; k++)
            if (mask[k] && d->rom.data[i + k] != needle[k])
                break;
        if (k < n)
            continue;
        size_t bs, be;
        bank_span(d, i, &bs, &be);
        if (i + n > be)
            continue;
        if (found >= 0)
            return -2;
        found = (long)i;
    }
    return found;
}

/* Instructions from `from` to at least `until` + 24 bytes, with their 16-bit
   operands (addresses, long branches, 16-bit immediates) masked out: code
   that only moved, or calls code that moved, still matches.  Returns the
   window length (0 if the code does not decode up to `until`). */
static size_t code_window(const ApexProject *s, size_t from, size_t until, size_t end,
                          uint8_t *mask, size_t cap) {
    size_t pos = from;
    int hit = 0;
    while (pos < end && pos - from < cap) {
        char txt[64];
        uint8_t bank;
        uint32_t addr;
        loc_of(s, pos, &bank, &addr);
        Cpu6809InstrInfo in =
            cpu6809_disassemble_info(s->rom.data + pos, end - pos, addr, txt, sizeof(txt));
        if (!in.size || pos + in.size > end || pos - from + in.size > cap)
            break;
        size_t oplen = (s->rom.data[pos] == 0x10u || s->rom.data[pos] == 0x11u) ? 2u : 1u;
        for (size_t k = 0; k < in.size; k++)
            mask[pos - from + k] = !(in.size >= oplen + 2u && k >= in.size - 2u);
        if (pos <= until && until < pos + in.size) /* the site may be an operand byte */
            hit = 1;
        pos += in.size;
        if (pos > until && pos - until >= 24u)
            break;
    }
    return hit ? pos - from : 0;
}

/* Rebase one site: returns the target file offset, or -1 (method / msg set). */
static long rebase_site(const ApexProject *s, const ApexProject *d, const ApexFingerprintDB *sdb,
                        const ApexMatchResult *mr, size_t mn, uint8_t bank, uint32_t addr,
                        const uint8_t *guard, size_t glen, ApexRebaseOp *ro) {
    size_t so = off_of(s, bank, addr), bs, be;
    if (so == (size_t)-1) {
        snprintf(ro->msg, sizeof(ro->msg), "no bank %02x in the source", bank);
        return -1;
    }
    bank_span(s, so, &bs, &be);
    int system = bank == 0xffu;
    /* the guard: the op's original bytes (at least one byte to compare) */
    if (!glen) {
        guard = s->rom.data + so;
        glen = 1;
    }
    /* 1. same place, same bytes and context */
    size_t ctx_lo = so >= bs + 16u ? so - 16u : bs,
           ctx_hi = so + glen + 16u <= be ? so + glen + 16u : be;
    size_t dso = off_of(d, bank, addr);
    if (s->rom.size == d->rom.size && dso != (size_t)-1 && dso + glen <= d->rom.size &&
        !memcmp(d->rom.data + dso, guard, glen) &&
        !memcmp(d->rom.data + (dso - (so - ctx_lo)), s->rom.data + ctx_lo, ctx_hi - ctx_lo)) {
        snprintf(ro->method, sizeof(ro->method), "same address");
        return (long)dso;
    }
    /* 2. the matched routine that contains the site, same offset in it */
    const ApexFunctionFingerprint *best = NULL;
    for (size_t i = 0; i < apex_fingerprint_count(sdb); i++) {
        const ApexFunctionFingerprint *f = apex_fingerprint_get(sdb, i);
        if (f->bank == bank && f->addr <= addr && addr - f->addr < 0x800u && f->label_name &&
            (!best || f->addr > best->addr))
            best = f;
    }
    for (size_t i = 0; best && i < mn; i++) {
        if (mr[i].src_bank != best->bank || mr[i].src_addr != best->addr)
            continue;
        size_t to = off_of(d, mr[i].dst_bank, mr[i].dst_addr + (addr - best->addr));
        if (to != (size_t)-1 && to + glen <= d->rom.size &&
            !memcmp(d->rom.data + to, guard, glen)) {
            snprintf(ro->method, sizeof(ro->method), "routine %.40s+0x%x", best->label_name,
                     (unsigned)(addr - best->addr));
            ro->confidence = mr[i].confidence;
            return (long)to;
        }
    }
    /* 3. the bytes around the site, found once in the target */
    static const size_t pads[] = {16, 32, 64, 8};
    for (size_t k = 0; k < sizeof(pads) / sizeof(pads[0]); k++) {
        size_t lo = so >= bs + pads[k] ? so - pads[k] : bs;
        size_t hi = so + glen + pads[k] <= be ? so + glen + pads[k] : be;
        long at = find_unique(d, s->rom.data + lo, hi - lo, system);
        if (at >= 0) {
            snprintf(ro->method, sizeof(ro->method), "context %lu bytes", (unsigned long)(hi - lo));
            return at + (long)(so - lo);
        }
    }
    /* 4. the code around the site with its addresses masked (the routine's
       start if known, else the site itself) */
    for (int pass = 0; pass < 2; pass++) {
        size_t from = so;
        if (pass == 0) {
            const ApexFunctionFingerprint *f0 = NULL;
            for (size_t i = 0; i < apex_fingerprint_count(sdb); i++) {
                const ApexFunctionFingerprint *f = apex_fingerprint_get(sdb, i);
                if (f->bank == bank && f->addr <= addr && addr - f->addr < 0x100u &&
                    (!f0 || f->addr > f0->addr))
                    f0 = f;
            }
            if (!f0 || f0->addr == addr)
                continue;
            from = so - (addr - f0->addr);
        }
        uint8_t mask[320];
        size_t n = code_window(s, from, so, be, mask, sizeof(mask));
        size_t fixed = 0;
        for (size_t k = 0; k < n; k++)
            fixed += mask[k];
        if (n < 8 || fixed < 6)
            continue;
        long at = find_unique_masked(d, s->rom.data + from, mask, n, system);
        if (at < 0)
            continue;
        size_t to = (size_t)at + (so - from);
        if (to + glen > d->rom.size)
            continue;
        /* the guard must agree except in masked (address) bytes */
        int same = 1, ok = 1;
        for (size_t k = 0; k < glen; k++) {
            if (d->rom.data[to + k] == guard[k])
                continue;
            same = 0;
            if (so - from + k >= n || mask[so - from + k])
                ok = 0;
        }
        if (!ok)
            continue;
        snprintf(ro->method, sizeof(ro->method), "code pattern %lu bytes%s", (unsigned long)n,
                 same ? "" : ", addresses differ");
        ro->guard_changed = !same;
        return (long)to;
    }
    if (glen >= 6) { /* the guard alone (e.g. a string that moved) */
        long at = find_unique(d, guard, glen, system);
        if (at >= 0) {
            snprintf(ro->method, sizeof(ro->method), "original bytes");
            return at;
        }
    }
    snprintf(ro->msg, sizeof(ro->msg), "not found in the target");
    return -1;
}

/* A generated label name (Bbb_Aaaaa): not a name a user gave. */
static int generated_name(const char *n) {
    return n && strlen(n) == 9 && n[0] == 'B' && n[3] == '_' && n[4] == 'A';
}

/* Where the paged routine at (bank, addr) of the source is in the target: the
   same place with the same first bytes, else its code with the 16-bit
   operands masked, found once in the paged banks.  Returns the target file
   offset or -1. */
static long locate_routine(const ApexProject *s, const ApexProject *d, uint8_t bank,
                           uint32_t addr) {
    size_t so = off_of(s, bank, addr), bs, be;
    if (so == (size_t)-1 || so >= s->paged_size)
        return -1;
    bank_span(s, so, &bs, &be);
    size_t dso = off_of(d, bank, addr);
    size_t n16 = be - so < 16u ? be - so : 16u;
    if (dso != (size_t)-1 && dso + n16 <= d->paged_size &&
        !memcmp(d->rom.data + dso, s->rom.data + so, n16))
        return (long)dso;
    uint8_t mask[320];
    size_t n = code_window(s, so, so, be, mask, sizeof(mask));
    size_t fixed = 0;
    for (size_t k = 0; k < n; k++)
        fixed += mask[k];
    if (n < 12 || fixed < 8)
        return -1;
    long at = find_unique_masked(d, s->rom.data + so, mask, n, 0);
    return at >= 0 ? at : -1;
}

int apex_patch_rebase_paged_entries(const ApexProject *sp, ApexProject *dp) {
    int found = 0;
    for (size_t bi = 0; bi < sp->banks; bi++) {
        const LabelSet *ls = &sp->bank_labels[bi];
        uint8_t bank = bank_id_for_index(sp->banks, (int)bi);
        for (size_t li = 0; li < ls->count; li++) {
            const Label *lb = &ls->items[li];
            if (!lb->is_code || (!lb->is_explicit_entry && generated_name(lb->name)))
                continue;
            long to = locate_routine(sp, dp, bank, lb->addr);
            if (to < 0)
                continue;
            uint8_t nb;
            uint32_t na;
            loc_of(dp, (size_t)to, &nb, &na);
            apex_project_set_kind(dp, 1, nb, na, APEX_KIND_CODE, NULL);
            found++;
        }
    }
    return found;
}

/* Rewrite a file path relative to the old patch file as one relative to the
   new patch file. */
static void move_path(char **field, const ApexPatch *src, const char *new_path) {
    char absb[1024], rel[1024];
    if (!*field)
        return;
    apex_patch_path(src, *field, absb, sizeof(absb));
    apex_patch_relpath(new_path, absb, rel, sizeof(rel));
    apex_patch_set_field(field, rel);
}

/* An op that cannot be carried over stays in the file, switched off. */
static void park(ApexPatchOp *o, const char *why) {
    char note[400];
    o->enabled = 0;
    snprintf(note, sizeof(note), "rebase: %s (was at %s)%s%s", why, o->at ? o->at : "?",
             o->note ? "; " : "", o->note ? o->note : "");
    apex_patch_set_field(&o->note, note);
}

int apex_patch_rebase(const ApexPatch *src, const ApexProject *sp, const ApexPatchBuild *sb,
                      const ApexProject *dp, const char *new_path, const char *dst_rom,
                      const char *dst_ini, ApexPatch *out, ApexRebaseOp *ops, char *err,
                      size_t errsz) {
    char *text = apex_patch_to_text(src);
    if (!text) {
        snprintf(err, errsz, "cannot copy the patch");
        return -1;
    }
    if (apex_patch_load_text(new_path, text, out, err, errsz) != 0) {
        free(text);
        return -1;
    }
    free(text);
    char rel[1024];
    apex_patch_relpath(new_path, dst_rom, rel, sizeof(rel));
    apex_patch_set_field(&out->base_rom, rel);
    out->base_crc32 = ari_crc32(dp->rom.data, dp->rom.size);
    out->has_crc = 1;
    if (dst_ini) {
        apex_patch_relpath(new_path, dst_ini, rel, sizeof(rel));
        apex_patch_set_field(&out->analysis, rel);
    } else {
        apex_patch_set_field(&out->analysis, NULL);
    }
    move_path(&out->output, src, new_path);

    ApexFingerprintDB *sdb = apex_fingerprint_build(sp), *ddb = apex_fingerprint_build(dp);
    size_t mn = 0;
    ApexMatchResult *mr =
        sdb && ddb ? apex_match_roms(sdb, ddb, APEX_MATCH_CONF_MEDIUM, 6, &mn) : NULL;
    int failed = 0;
    for (size_t i = 0; i < out->count; i++) {
        ApexPatchOp *o = &out->ops[i];
        const ApexPatchOpResult *r = i < sb->count ? &sb->ops[i] : NULL;
        ApexRebaseOp *ro = &ops[i];
        memset(ro, 0, sizeof(*ro));
        move_path(&o->file, src, new_path);
        apex_patch_set_field(&o->placed, NULL); /* allocations are chosen anew */
        if (!o->enabled) {
            ro->status = APEX_PATCH_DISABLED;
            continue;
        }
        if (o->type == APEX_PATCH_RAM) {
            ro->status = APEX_PATCH_WARN;
            snprintf(ro->msg, sizeof(ro->msg), "RAM %s kept: check it is unused in the target",
                     o->at ? o->at : "?");
            continue;
        }
        if (!o->at) {
            ro->status = APEX_PATCH_OK;
            snprintf(ro->method, sizeof(ro->method), "no address (allocated)");
            continue;
        }
        if (o->type == APEX_PATCH_FREE || o->type == APEX_PATCH_RESERVE) {
            ro->status = APEX_PATCH_WARN;
            snprintf(ro->msg, sizeof(ro->msg), "%s kept at %s: check the target's free space",
                     apex_patch_type_name(o->type), o->at);
            continue;
        }
        if (!r || r->status == APEX_PATCH_ERROR) {
            ro->status = APEX_PATCH_ERROR;
            snprintf(ro->msg, sizeof(ro->msg), "the op has errors in the source patch");
            park(o, "it has errors in the source patch");
            failed++;
            continue;
        }
        ro->old_bank = r->bank;
        ro->old_addr = r->addr;
        if (!r->length &&
            !apex_patch_resolve_addr(sp, o->at, &ro->old_bank, &ro->old_addr, NULL, 0)) {
            ro->status = APEX_PATCH_ERROR;
            snprintf(ro->msg, sizeof(ro->msg), "cannot resolve %s", o->at);
            park(o, "address not resolved");
            failed++;
            continue;
        }
        const uint8_t *g = r->guard ? r->guard : r->orig;
        size_t gl = r->guard ? r->guard_len : r->length;
        long to = rebase_site(sp, dp, sdb, mr, mn, ro->old_bank, ro->old_addr, g, gl, ro);
        if (to < 0) {
            ro->status = APEX_PATCH_ERROR;
            park(o, "not found in the target");
            failed++;
            continue;
        }
        loc_of(dp, (size_t)to, &ro->new_bank, &ro->new_addr);
        char at[160];
        apex_patch_format_at(dp, ro->new_bank, ro->new_addr, at, sizeof(at));
        apex_patch_set_field(&o->at, at);
        ro->status = APEX_PATCH_OK;
        if (ro->old_bank != ro->new_bank || ro->old_addr != ro->new_addr)
            ro->moved = 1;
        if (ro->guard_changed) {
            /* operand addresses differ: guard the target's bytes; the op's
               own bytes may hold such an address too -- review it */
            ro->status = APEX_PATCH_WARN;
            snprintf(ro->msg, sizeof(ro->msg), "addresses in the original bytes differ: review");
            if (o->expect && !r->guard) {
                char hx[3 * 512 + 1];
                size_t n = 0, gl2 = gl < 512 ? gl : 512;
                for (size_t k = 0; k < gl2; k++)
                    n += (size_t)snprintf(hx + n, sizeof(hx) - n, k ? " %02x" : "%02x",
                                          dp->rom.data[to + (long)k]);
                apex_patch_set_field(&o->expect, hx);
            }
        }
        if (o->type == APEX_PATCH_HOOK || o->type == APEX_PATCH_ASM || o->type == APEX_PATCH_NOP)
            if (ro->moved && strstr(ro->method, "context"))
                ro->status = APEX_PATCH_WARN; /* code found by bytes only: review it */
    }
    if (mr)
        apex_match_results_free(mr, mn);
    apex_fingerprint_free(sdb);
    apex_fingerprint_free(ddb);
    return failed;
}

int apex_patch_rebase_files(const ApexPatch *src, const ApexProject *sp, const ApexPatchBuild *sb,
                            const char *trom, const char *tini, const char *out, ApexPatch *np,
                            ApexRebaseOp **ops_out, char *labels, size_t labelsz, char *err,
                            size_t errsz) {
    *ops_out = NULL;
    if (labels && labelsz)
        labels[0] = '\0';
    ApexProject *dp = apex_project_open(trom, tini);
    if (!dp) {
        snprintf(err, errsz, "cannot open %s", trom);
        return -1;
    }
    int paged = 0;
    if (!tini) { /* no analysis of the target: reach its code from the source's */
        apex_match_inject_entries(dp, sp, 1);
        paged = apex_patch_rebase_paged_entries(sp, dp);
    }
    if (apex_project_analyze(dp) != 0) {
        snprintf(err, errsz, "cannot analyse %s", trom);
        apex_project_free(dp);
        return -1;
    }
    /* without a target analysis, write one with the source's labels matched
       into the target (as apexmatch does), so the patch code's symbols resolve */
    char ini_path[1100];
    const char *use_ini = tini;
    if (!tini) {
        snprintf(ini_path, sizeof(ini_path), "%s", out);
        size_t n = strlen(ini_path);
        if (n > 10 && !strcmp(ini_path + n - 10, ".patch.ini"))
            ini_path[n - 10] = '\0';
        strncat(ini_path, ".labels.ini", sizeof(ini_path) - strlen(ini_path) - 1);
        ApexFingerprintDB *sdb = apex_fingerprint_build(sp), *ddb = apex_fingerprint_build(dp);
        size_t n1 = 0, n2 = 0;
        ApexMatchResult *r1 = apex_match_roms(sdb, ddb, APEX_MATCH_CONF_MEDIUM, 6, &n1);
        ApexMatchResult *r2 = apex_match_scan_system_bank(sdb, ddb, dp, r1, n1, 4, &n2);
        ApexMatchResult *all = calloc(n1 + n2 + 1u, sizeof(*all));
        memcpy(all, r1, n1 * sizeof(*all));
        if (n2)
            memcpy(all + n1, r2, n2 * sizeof(*all));
        FILE *f = fopen(ini_path, "w");
        if (f) {
            apex_match_write_ini(f, all, n1 + n2, sp);
            fclose(f);
            if (labels)
                snprintf(labels, labelsz, "%s (%lu labels; %d paged routines located)", ini_path,
                         (unsigned long)(n1 + n2), paged);
            use_ini = ini_path;
            /* re-open the target with those labels, so asm code resolves */
            ApexProject *dp2 = apex_project_open(trom, ini_path);
            if (dp2) {
                apex_match_inject_entries(dp2, sp, 1);
                if (apex_project_analyze(dp2) == 0) {
                    apex_project_free(dp);
                    dp = dp2;
                } else {
                    apex_project_free(dp2);
                }
            }
        }
        free(all);
        apex_match_results_free(r1, n1);
        apex_match_results_free(r2, n2);
        apex_fingerprint_free(sdb);
        apex_fingerprint_free(ddb);
    }
    ApexRebaseOp *ro = calloc(src->count + 1u, sizeof(*ro));
    int failed = apex_patch_rebase(src, sp, sb, dp, out, trom, use_ini, np, ro, err, errsz);
    apex_project_free(dp);
    if (failed < 0) {
        free(ro);
        return -1;
    }
    if (apex_patch_save(np, out) != 0) {
        snprintf(err, errsz, "cannot write %s", out);
        apex_patch_free(np);
        free(ro);
        return -1;
    }
    *ops_out = ro;
    return failed;
}

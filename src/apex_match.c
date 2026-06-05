#include "apex_match.h"
#include "apex_project.h"
#include "apex_analysis.h"
#include "apex_config.h"
#include "cpu6809.h"
#include "apex.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* FNV-32 helpers                                                       */
/* ------------------------------------------------------------------ */

#define FNV32_INIT   0x811c9dc5u
#define FNV32_PRIME  0x01000193u

static uint32_t fnv32_byte(uint32_t h, uint8_t b)
{
    return (h ^ b) * FNV32_PRIME;
}

static uint32_t fnv32_u32(uint32_t h, uint32_t v)
{
    h = fnv32_byte(h, (uint8_t)(v >> 24));
    h = fnv32_byte(h, (uint8_t)(v >> 16));
    h = fnv32_byte(h, (uint8_t)(v >>  8));
    h = fnv32_byte(h, (uint8_t)(v      ));
    return h;
}

static uint32_t fnv32_str(uint32_t h, const char *s)
{
    for (; *s; s++) {
        h = fnv32_byte(h, (uint8_t)*s);
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* ROM byte navigation (mirrors locate_bank_bytes in apexdis.c)        */
/* ------------------------------------------------------------------ */

static int rom_bytes_at(const uint8_t *rom, size_t banks,
                        uint8_t bank, uint32_t addr,
                        const uint8_t **out, size_t *out_len)
{
    size_t offset;

    if (bank == 0xffu) {
        if (addr < APEX_SYSTEM_ORG || addr >= 0x10000u) return 0;
        offset = banks * APEX_BANK_SIZE + (size_t)(addr - APEX_SYSTEM_ORG);
    } else {
        if (addr < APEX_PAGED_ORG || addr >= 0x8000u) return 0;
        int idx = bank_index_for_far_ref(rom, banks, bank);
        if (idx < 0) return 0;
        offset = (size_t)idx * APEX_BANK_SIZE + (size_t)(addr - APEX_PAGED_ORG);
    }
    *out     = rom + offset;
    *out_len = banks * APEX_BANK_SIZE + APEX_SYSTEM_SIZE - offset;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Instruction text parsing                                             */
/* ------------------------------------------------------------------ */

/* Extract the mnemonic (first whitespace-delimited word) into buf[]. */
static size_t extract_mnemonic(const char *text, char *buf, size_t cap)
{
    const char *p = text;
    size_t n = 0;

    while (*p == ' ') p++;
    while (*p && !isspace((unsigned char)*p) && n + 1 < cap) {
        buf[n++] = *p++;
    }
    buf[n] = '\0';
    return n;
}

/*
 * Return a pointer to the immediate value string (the part after '#'),
 * or NULL if there is no immediate operand.
 */
static const char *find_immediate(const char *text)
{
    const char *p = strchr(text, '#');
    return p ? p + 1 : NULL;
}

/*
 * True if the mnemonic is a subroutine call (JSR / BSR / LBSR).
 * These contribute to the callee hash.
 */
static int is_call_mnemonic(const char *mnem)
{
    return strcmp(mnem, "JSR")  == 0 ||
           strcmp(mnem, "BSR")  == 0 ||
           strcmp(mnem, "LBSR") == 0;
}

/* ------------------------------------------------------------------ */
/* Internal fingerprint entry (with callee list for pass-2)            */
/* ------------------------------------------------------------------ */

#define MAX_CALLEES  64
#define MAX_INSTRS  256

typedef struct {
    ApexFunctionFingerprint fp;
    /* raw callee (bank, addr) pairs collected during pass 1 */
    uint8_t  callee_banks[MAX_CALLEES];
    uint32_t callee_addrs[MAX_CALLEES];
    size_t   callee_count;
} FPEntry;

struct ApexFingerprintDB {
    FPEntry *entries;
    size_t   count;
    size_t   cap;
    /* sorted flag for binary search by (bank, addr) */
    int      sorted;
};

/* ------------------------------------------------------------------ */
/* DB helpers                                                           */
/* ------------------------------------------------------------------ */

static int entry_cmp(const void *a, const void *b)
{
    const FPEntry *ea = (const FPEntry *)a;
    const FPEntry *eb = (const FPEntry *)b;
    if (ea->fp.bank != eb->fp.bank) return (int)ea->fp.bank - (int)eb->fp.bank;
    if (ea->fp.addr < eb->fp.addr) return -1;
    if (ea->fp.addr > eb->fp.addr) return  1;
    return 0;
}

static FPEntry *db_find(ApexFingerprintDB *db, uint8_t bank, uint32_t addr)
{
    size_t lo = 0, hi = db->count;

    if (!db->sorted) return NULL;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        FPEntry *e = &db->entries[mid];
        if (e->fp.bank < bank || (e->fp.bank == bank && e->fp.addr < addr)) lo = mid + 1;
        else if (e->fp.bank > bank || (e->fp.bank == bank && e->fp.addr > addr)) hi = mid;
        else return e;
    }
    return NULL;
}

static int db_add(ApexFingerprintDB *db, const FPEntry *e)
{
    if (db->count == db->cap) {
        size_t new_cap = db->cap ? db->cap * 2 : 128;
        FPEntry *buf = realloc(db->entries, new_cap * sizeof(*buf));
        if (!buf) return 0;
        db->entries = buf;
        db->cap = new_cap;
    }
    db->entries[db->count++] = *e;
    db->sorted = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Pass 1: disassemble one routine and fill L1/L2 + raw callee list    */
/* ------------------------------------------------------------------ */

static void fingerprint_one(const uint8_t *rom, size_t banks,
                             uint8_t bank, uint32_t addr,
                             const LabelSet *labels,  /* for stop-at-label */
                             FPEntry *out)
{
    const uint8_t *data;
    size_t         avail;
    char           text[256];
    char           mnem[32];
    uint32_t       l1  = FNV32_INIT;
    uint32_t       l2  = FNV32_INIT;
    uint32_t       cur = addr;
    uint16_t       n   = 0;

    memset(out, 0, sizeof(*out));
    out->fp.bank = bank;
    out->fp.addr = addr;
    out->fp.l1_hash     = FNV32_INIT;
    out->fp.l2_hash     = FNV32_INIT;
    out->fp.callee_hash = FNV32_INIT;

    if (!rom_bytes_at(rom, banks, bank, addr, &data, &avail)) return;

    while (n < MAX_INSTRS) {
        size_t pos = (size_t)(cur - addr);
        if (pos >= avail) break;

        /* Stop if we've walked into a different named label's territory
           (but allow the first instruction through at addr itself). */
        if (n > 0 && labels) {
            size_t i;
            for (i = 0; i < labels->count; i++) {
                if (labels->items[i].addr == cur &&
                    labels->items[i].is_code &&
                    labels->items[i].name &&
                    !generated_any_label_name(labels->items[i].name)) {
                    goto done;
                }
            }
        }

        Cpu6809InstrInfo info = cpu6809_disassemble_info(
            data + pos, avail - pos, cur, text, sizeof(text));

        if (info.size == 0) break;

        extract_mnemonic(text, mnem, sizeof(mnem));

        /* L1: hash mnemonic */
        l1 = fnv32_str(l1, mnem);

        /* L2: hash mnemonic + non-address operand content.
           Operands that are NOT address references (register masks, indexed
           offsets, small immediates) fully distinguish otherwise-identical
           functions like PSHS X vs PSHS A,X, LDX 2,S vs LDX 3,S, etc.
           Operands that ARE address references (branches, JSR targets, data
           pointers) are excluded so L2 stays address-independent.
           Gate: has_target==true means the operand encodes a CPU address. */
        l2 = fnv32_str(l2, mnem);
        if (!info.has_target) {
            /* Include full operand: register lists, indexed offsets, immediates. */
            const char *op = text;
            while (*op == ' ') op++;
            while (*op && !isspace((unsigned char)*op)) op++;  /* skip mnemonic */
            while (*op == ' ') op++;
            if (*op) l2 = fnv32_str(l2, op);
        } else {
            /* Address operand: keep only the #-immediate part if present
               (e.g. indexed indirect addressing can have both an address and
               an immediate offset — only the latter is address-independent). */
            const char *imm = find_immediate(text);
            if (imm) l2 = fnv32_str(l2, imm);
        }

        /* Callee collection */
        if (info.has_target && is_call_mnemonic(mnem) &&
            out->callee_count < MAX_CALLEES) {
            /* Resolve target bank: same bank for in-bank calls.
               For JSR to system addresses, use bank 0xff. */
            uint8_t tb = (info.target >= APEX_SYSTEM_ORG) ? 0xffu : bank;
            out->callee_banks[out->callee_count] = tb;
            out->callee_addrs[out->callee_count] = info.target;
            out->callee_count++;
        }

        cur += (uint32_t)info.size;
        n++;

        if (info.flags & CPU6809_FLOW_STOP) break;
    }
done:
    out->fp.l1_hash    = l1;
    out->fp.l2_hash    = l2;
    out->fp.instr_count = n;
    /* callee_hash filled in pass 2 */
}

/* ------------------------------------------------------------------ */
/* Pass 2: resolve callee hashes                                        */
/* ------------------------------------------------------------------ */

static void resolve_callee_hashes(ApexFingerprintDB *db)
{
    size_t i, j;

    for (i = 0; i < db->count; i++) {
        FPEntry *e = &db->entries[i];
        uint32_t ch = FNV32_INIT;

        for (j = 0; j < e->callee_count; j++) {
            FPEntry *ce = db_find(db, e->callee_banks[j], e->callee_addrs[j]);
            uint32_t callee_l1 = ce ? ce->fp.l1_hash : 0u;
            ch = fnv32_u32(ch, callee_l1);
        }
        e->fp.callee_hash = ch;
    }
}

/* ------------------------------------------------------------------ */
/* Iterate over all code labels in a project                           */
/* ------------------------------------------------------------------ */

typedef void (*FPCallback)(const uint8_t *rom, size_t banks,
                            uint8_t bank, uint32_t addr,
                            const char *label_name,
                            const LabelSet *bank_ls,
                            ApexFingerprintDB *db);

static void fp_callback(const uint8_t *rom, size_t banks,
                         uint8_t bank, uint32_t addr,
                         const char *label_name,
                         const LabelSet *bank_ls,
                         ApexFingerprintDB *db)
{
    FPEntry e;
    fingerprint_one(rom, banks, bank, addr, bank_ls, &e);
    if (e.fp.instr_count < 2) return;  /* too short to be reliable */
    e.fp.label_name = label_name;
    db_add(db, &e);
}

static void collect_labels(const struct ApexProject *p, ApexFingerprintDB *db)
{
    size_t bi, li;

    /* paged banks */
    for (bi = 0; bi < p->banks; bi++) {
        const LabelSet *ls = &p->bank_labels[bi];
        uint8_t bank_id = p->rom.data[bi * APEX_BANK_SIZE];

        for (li = 0; li < ls->count; li++) {
            const Label *lb = &ls->items[li];
            if (!lb->is_code || lb->is_data) continue;
            fp_callback(p->rom.data, p->banks,
                        bank_id, lb->addr,
                        lb->name && !generated_any_label_name(lb->name) ? lb->name : NULL,
                        ls, db);
        }
    }

    /* system bank */
    {
        const LabelSet *ls = &p->system_labels;
        for (li = 0; li < ls->count; li++) {
            const Label *lb = &ls->items[li];
            if (!lb->is_code || lb->is_data) continue;
            fp_callback(p->rom.data, p->banks,
                        0xffu, lb->addr,
                        lb->name && !generated_any_label_name(lb->name) ? lb->name : NULL,
                        ls, db);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public: build / free                                                 */
/* ------------------------------------------------------------------ */

ApexFingerprintDB *apex_fingerprint_build(const struct ApexProject *p)
{
    ApexFingerprintDB *db = calloc(1, sizeof(*db));
    if (!db) return NULL;

    collect_labels(p, db);

    if (db->count == 0) return db;

    qsort(db->entries, db->count, sizeof(*db->entries), entry_cmp);
    db->sorted = 1;

    resolve_callee_hashes(db);
    return db;
}

void apex_fingerprint_free(ApexFingerprintDB *db)
{
    if (!db) return;
    free(db->entries);
    free(db);
}

size_t apex_fingerprint_count(const ApexFingerprintDB *db)
{
    return db ? db->count : 0;
}

const ApexFunctionFingerprint *apex_fingerprint_get(const ApexFingerprintDB *db, size_t i)
{
    if (!db || i >= db->count) return NULL;
    return &db->entries[i].fp;
}

const ApexFunctionFingerprint *apex_fingerprint_at(const ApexFingerprintDB *db,
                                                    uint8_t bank, uint32_t addr)
{
    FPEntry *e = db_find((ApexFingerprintDB *)db, bank, addr);
    return e ? &e->fp : NULL;
}

/* ------------------------------------------------------------------ */
/* Entry injection                                                      */
/* ------------------------------------------------------------------ */

void apex_match_inject_entries(struct ApexProject       *dst,
                               const struct ApexProject *src,
                               int                       system_only)
{
    size_t bi, li;

    /* System bank: inject ALL named code labels, not just explicit entries.
       Many OS helper functions (Delay_N, Mem_*, Lamp*) are discovered only
       by code flow in the source and never appear in [entries], but their
       addresses are stable across ROM versions and we need them as entry
       points in the target so the target analysis can reach them too. */
    for (li = 0; li < src->system_labels.count; li++) {
        const Label *lb = &src->system_labels.items[li];
        if (!lb->is_code) continue;
        apex_project_set_kind(dst, 0, 0xffu, lb->addr, APEX_KIND_CODE, NULL);
    }

    if (system_only) return;

    /* Paged banks: inject only explicit entries (named labels from paged banks
       are game-specific and their addresses differ between ROM versions). */
    for (bi = 0; bi < src->banks; bi++) {
        const LabelSet *ls = &src->bank_labels[bi];
        uint8_t bank_id = src->rom.data[bi * APEX_BANK_SIZE];

        for (li = 0; li < ls->count; li++) {
            const Label *lb = &ls->items[li];
            if (!lb->is_code || !lb->is_explicit_entry) continue;
            apex_project_set_kind(dst, 1, bank_id, lb->addr, APEX_KIND_CODE, NULL);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Matching                                                             */
/* ------------------------------------------------------------------ */

static int score_pair(const ApexFunctionFingerprint *src,
                      const ApexFunctionFingerprint *dst)
{
    if (src->l1_hash != dst->l1_hash) return 0;
    if (src->l2_hash == dst->l2_hash) return APEX_MATCH_CONF_EXACT;
    if (src->callee_hash == dst->callee_hash &&
        src->callee_hash != FNV32_INIT)       return APEX_MATCH_CONF_HIGH;
    return APEX_MATCH_CONF_MEDIUM;
}

ApexMatchResult *apex_match_roms(const ApexFingerprintDB *src_db,
                                 const ApexFingerprintDB *dst_db,
                                 int min_confidence,
                                 int min_instrs_for_medium,
                                 size_t *out_count)
{
    size_t si, di;
    /* Result buffer — at most one result per named source entry. */
    ApexMatchResult *results = NULL;
    size_t           result_count = 0;
    size_t           result_cap   = 0;

    *out_count = 0;
    if (!src_db || !dst_db) return NULL;

    /* For each named source fingerprint, find the best matching target. */
    for (si = 0; si < src_db->count; si++) {
        const FPEntry *se = &src_db->entries[si];
        if (!se->fp.label_name) continue;  /* unnamed — nothing to transfer */

        int best_score = 0;
        size_t best_di = 0;

        for (di = 0; di < dst_db->count; di++) {
            int s = score_pair(&se->fp, &dst_db->entries[di].fp);
            if (s > best_score) {
                best_score = s;
                best_di    = di;
                if (s == APEX_MATCH_CONF_EXACT) break;  /* can't do better */
            }
        }

        if (best_score < (min_confidence > 0 ? min_confidence : 1)) continue;

        /* Filter: medium-confidence matches on short routines are unreliable */
        if (best_score < APEX_MATCH_CONF_HIGH && min_instrs_for_medium > 0 &&
            se->fp.instr_count < (uint16_t)min_instrs_for_medium) continue;

        /* Grow result buffer */
        if (result_count == result_cap) {
            size_t nc = result_cap ? result_cap * 2 : 64;
            ApexMatchResult *nb = realloc(results, nc * sizeof(*nb));
            if (!nb) { free(results); *out_count = 0; return NULL; }
            results     = nb;
            result_cap  = nc;
        }

        ApexMatchResult *r = &results[result_count++];
        r->label_name = se->fp.label_name;
        r->src_addr   = se->fp.addr;
        r->src_bank   = se->fp.bank;
        r->dst_addr   = dst_db->entries[best_di].fp.addr;
        r->dst_bank   = dst_db->entries[best_di].fp.bank;
        r->confidence = best_score;
    }

    /* Resolve conflicts: if two source functions matched the same target,
       keep only the one with higher confidence; the other gets dropped. */
    for (si = 0; si < result_count; si++) {
        for (di = si + 1; di < result_count; di++) {
            if (results[si].dst_bank == results[di].dst_bank &&
                results[si].dst_addr == results[di].dst_addr) {
                /* Keep higher-confidence match, zero out the weaker one. */
                if (results[si].confidence >= results[di].confidence) {
                    results[di].label_name = NULL;
                } else {
                    results[si].label_name = NULL;
                }
            }
        }
    }
    /* Compact out the NULLed entries */
    {
        size_t w = 0;
        for (si = 0; si < result_count; si++) {
            if (results[si].label_name) results[w++] = results[si];
        }
        result_count = w;
    }

    *out_count = result_count;
    return results;
}

void apex_match_results_free(ApexMatchResult *results, size_t count)
{
    (void)count;
    free(results);
}

/* ------------------------------------------------------------------ */
/* System-bank scan phase                                               */
/* ------------------------------------------------------------------ */

/* Secondary index entry for L1+L2 lookup during scan. */
typedef struct {
    uint32_t l1_hash;
    uint32_t l2_hash;
    uint32_t callee_hash;  /* L3 of the source function (FNV32_INIT = unknown) */
    size_t   src_idx;   /* index into src_db->entries */
} L12Entry;

static int l12_cmp(const void *a, const void *b)
{
    const L12Entry *ea = (const L12Entry *)a;
    const L12Entry *eb = (const L12Entry *)b;
    if (ea->l1_hash != eb->l1_hash)
        return (ea->l1_hash < eb->l1_hash) ? -1 : 1;
    if (ea->l2_hash != eb->l2_hash)
        return (ea->l2_hash < eb->l2_hash) ? -1 : 1;
    return 0;
}

/*
 * Compute the callee L1-hash for one JSR target address in the target ROM.
 * Fast path: look up in the target DB.  Slow path: fingerprint on-the-fly.
 */
static uint32_t scan_callee_l1(const uint8_t *rom, size_t banks,
                                 uint8_t callee_bank, uint32_t callee_addr,
                                 const ApexFingerprintDB *dst_db)
{
    const ApexFunctionFingerprint *fp = apex_fingerprint_at(dst_db,
                                                             callee_bank,
                                                             callee_addr);
    if (fp) return fp->l1_hash;

    /* Not in DB (unreachable from vectors): fingerprint on-the-fly. */
    FPEntry tmp;
    fingerprint_one(rom, banks, callee_bank, callee_addr, NULL, &tmp);
    return tmp.fp.l1_hash;
}

ApexMatchResult *apex_match_scan_system_bank(
    const ApexFingerprintDB  *src_db,
    const ApexFingerprintDB  *dst_db,
    const struct ApexProject *dst,
    const ApexMatchResult    *prior,
    size_t                    prior_count,
    int                       min_instrs,
    size_t                   *out_count)
{
    size_t si, pi;
    L12Entry *idx = NULL;
    size_t    idx_count = 0;
    ApexMatchResult *results = NULL;
    size_t    result_count = 0, result_cap = 0;

    *out_count = 0;
    if (min_instrs <= 0) min_instrs = 4;

    /* Build L1+L2 index for named, unmatched source functions only. */
    idx = malloc(src_db->count * sizeof(*idx));
    if (!idx) return NULL;

    for (si = 0; si < src_db->count; si++) {
        const FPEntry *se = &src_db->entries[si];
        if (!se->fp.label_name)            continue;  /* unnamed */
        if (se->fp.instr_count < (uint16_t)min_instrs) continue;
        /* Skip already matched */
        int already = 0;
        for (pi = 0; pi < prior_count; pi++) {
            if (prior[pi].src_bank == se->fp.bank &&
                prior[pi].src_addr == se->fp.addr) { already = 1; break; }
        }
        if (already) continue;

        idx[idx_count].l1_hash     = se->fp.l1_hash;
        idx[idx_count].l2_hash     = se->fp.l2_hash;
        idx[idx_count].callee_hash = se->fp.callee_hash;
        idx[idx_count].src_idx     = si;
        idx_count++;
    }

    if (idx_count == 0) { free(idx); return NULL; }

    qsort(idx, idx_count, sizeof(*idx), l12_cmp);

    /*
     * Per-source-function hit tracking:
     *   l12_hits   total L1+L2 matches in the target
     *   l12_addr   address of the L1+L2 match (valid only if l12_hits==1)
     *   l3_hits    subset where L1+L2+L3 all match
     *   l3_addr    address of the L3 match (valid only if l3_hits==1)
     *
     * Selection at collection time:
     *   l3_hits==1  → use l3_addr  (L3 disambiguated, highest confidence)
     *   l3_hits==0 && l12_hits==1 → use l12_addr  (unique L1+L2, no L3 needed)
     *   otherwise   → ambiguous, skip
     */
    typedef struct {
        uint32_t l12_addr;
        int      l12_hits;
        uint32_t l3_addr;
        int      l3_hits;
    } ScanHit;
    ScanHit *hits = calloc(idx_count, sizeof(*hits));
    if (!hits) { free(idx); return NULL; }

    /* Scan every byte position in the target's system bank. */
    const uint8_t *sys_data = NULL;
    size_t         sys_avail = 0;
    if (!rom_bytes_at(dst->rom.data, dst->banks, 0xffu, APEX_SYSTEM_ORG,
                      &sys_data, &sys_avail))
        goto done;
    if (sys_avail > APEX_SYSTEM_SIZE) sys_avail = APEX_SYSTEM_SIZE;

    {
        uint32_t off;
        for (off = 0; off < (uint32_t)sys_avail; off++) {
            FPEntry cand;
            uint32_t cpu = APEX_SYSTEM_ORG + off;
            fingerprint_one(dst->rom.data, dst->banks, 0xffu, cpu, NULL, &cand);
            if (cand.fp.instr_count < (uint16_t)min_instrs) continue;

            /* Binary search for L1 match in index. */
            size_t lo = 0, hi = idx_count;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (idx[mid].l1_hash < cand.fp.l1_hash) lo = mid + 1;
                else hi = mid;
            }
            /* L3: compute callee hash lazily — only when an L1+L2 match is found. */
            int cand_callee_computed = 0;
            for (; lo < idx_count && idx[lo].l1_hash == cand.fp.l1_hash; lo++) {
                if (idx[lo].l2_hash != cand.fp.l2_hash) continue;

                const FPEntry *se = &src_db->entries[idx[lo].src_idx];
                int diff = (int)se->fp.instr_count - (int)cand.fp.instr_count;
                if (diff < -1 || diff > 1) continue;

                size_t ii = (size_t)(&idx[lo] - idx);

                /* L1+L2 hit */
                if (hits[ii].l12_hits == 0) hits[ii].l12_addr = cpu;
                hits[ii].l12_hits++;

                /* L3: compute candidate callee hash on first L1+L2 hit at this pos */
                if (!cand_callee_computed) {
                    uint32_t ch = FNV32_INIT;
                    size_t j;
                    for (j = 0; j < cand.callee_count; j++) {
                        uint32_t cl1 = scan_callee_l1(dst->rom.data, dst->banks,
                                                       cand.callee_banks[j],
                                                       cand.callee_addrs[j],
                                                       dst_db);
                        ch = fnv32_u32(ch, cl1);
                    }
                    cand.fp.callee_hash = ch;
                    cand_callee_computed = 1;
                }

                /* L3 hit: source callee_hash must be non-trivial to be useful */
                if (idx[lo].callee_hash != FNV32_INIT &&
                    idx[lo].callee_hash == cand.fp.callee_hash) {
                    if (hits[ii].l3_hits == 0) hits[ii].l3_addr = cpu;
                    hits[ii].l3_hits++;
                }
            }
        }
    }

    /* Collect unambiguous matches.
     *
     * Priority:
     *  1. l3_hits == 1: L3 uniquely identified one position → use l3_addr.
     *  2. l3_hits == 0 && l12_hits == 1: only one L1+L2 match, no L3 needed.
     *  3. everything else: ambiguous → skip.
     */
    for (si = 0; si < idx_count; si++) {
        uint32_t chosen_addr;

        if (hits[si].l3_hits == 1) {
            chosen_addr = hits[si].l3_addr;
        } else if (hits[si].l3_hits == 0 && hits[si].l12_hits == 1) {
            chosen_addr = hits[si].l12_addr;
        } else {
            continue;  /* not found or ambiguous */
        }

        /* Check the target address isn't already used in prior results. */
        int conflict = 0;
        for (pi = 0; pi < prior_count; pi++) {
            if (prior[pi].dst_bank == 0xffu &&
                prior[pi].dst_addr == chosen_addr) { conflict = 1; break; }
        }
        if (conflict) continue;

        if (result_count == result_cap) {
            size_t nc = result_cap ? result_cap * 2 : 32;
            ApexMatchResult *nb = realloc(results, nc * sizeof(*nb));
            if (!nb) goto done;
            results    = nb;
            result_cap = nc;
        }
        const FPEntry *se = &src_db->entries[idx[si].src_idx];
        ApexMatchResult *r = &results[result_count++];
        r->label_name = se->fp.label_name;
        r->src_bank   = se->fp.bank;
        r->src_addr   = se->fp.addr;
        r->dst_bank   = 0xffu;
        r->dst_addr   = chosen_addr;
        r->confidence = APEX_MATCH_CONF_EXACT;
    }

done:
    free(hits);
    free(idx);
    *out_count = result_count;
    return results;
}

/* ------------------------------------------------------------------ */
/* INI output                                                           */
/* ------------------------------------------------------------------ */

static const char *conf_label(int c)
{
    if (c >= APEX_MATCH_CONF_EXACT)  return "exact";
    if (c >= APEX_MATCH_CONF_HIGH)   return "high";
    if (c >= APEX_MATCH_CONF_MEDIUM) return "medium";
    return "low";
}

void apex_match_write_ini(FILE *out,
                          const ApexMatchResult    *results,
                          size_t                    count,
                          const struct ApexProject *src)
{
    size_t i;

    fprintf(out, "; Generated by apexmatch\n");
    fprintf(out, "; Source: %s\n\n", src->rom_path ? src->rom_path : "?");

    /* ---- [labels] ---- */
    fprintf(out, "[labels]\n");
    for (i = 0; i < count; i++) {
        const ApexMatchResult *r = &results[i];
        fprintf(out, "B%02x_A%04x = %s ; matched confidence=%s src=B%02x_A%04x\n",
                r->dst_bank, r->dst_addr, r->label_name, conf_label(r->confidence),
                r->src_bank, r->src_addr);
    }
    fprintf(out, "\n");

    /* ---- [entries] ---- */
    fprintf(out, "[entries]\n");
    for (i = 0; i < count; i++) {
        const ApexMatchResult *r = &results[i];
        fprintf(out, "B%02x_A%04x = code\n", r->dst_bank, r->dst_addr);
    }
    fprintf(out, "\n");

    /* ---- [inline] — transfer signatures for matched functions ---- */
    {
        int printed_header = 0;

        for (i = 0; i < count; i++) {
            const ApexMatchResult *r = &results[i];
            /* Look up inline sig in source project */
            const InlineSignature *sig =
                inline_signature_for(&src->inline_sigs, r->src_bank, r->src_addr);
            if (!sig) continue;

            if (!printed_header) {
                fprintf(out, "[inline]\n");
                printed_header = 1;
            }

            /* Rebuild the spec string from the signature schema */
            {
                size_t f;
                fprintf(out, "B%02x_A%04x = ", r->dst_bank, r->dst_addr);
                for (f = 0; f < sig->schema.count; f++) {
                    const TableField *tf = &sig->schema.items[f];
                    size_t n;
                    if (f > 0) fprintf(out, ", ");
                    for (n = 0; n < tf->count; n++) {
                        if (n > 0) fprintf(out, ", ");
                        switch (tf->kind) {
                        case TABLE_BYTE:             fprintf(out, "byte");      break;
                        case TABLE_WORD:             fprintf(out, "word");      break;
                        case TABLE_FAR_CODE:         fprintf(out, "far_code");  break;
                        case TABLE_FAR_DATA:         fprintf(out, "far_data");  break;
                        case TABLE_FAR_STRING:       fprintf(out, "far_string"); break;
                        case TABLE_FAR_TABLE:        fprintf(out, "far_table"); break;
                        default:                     fprintf(out, "byte");      break;
                        }
                    }
                }
                fprintf(out, " ; transferred from B%02x_A%04x\n",
                        r->src_bank, r->src_addr);
            }
        }
        if (printed_header) fprintf(out, "\n");
    }

    /* ---- [docs] — transfer docs for matched functions ---- */
    {
        int printed_header = 0;

        for (i = 0; i < count; i++) {
            const ApexMatchResult *r = &results[i];
            const char *doc = config_doc_at(&src->docs, r->src_bank, r->src_addr);
            if (!doc) continue;

            if (!printed_header) {
                fprintf(out, "[docs]\n");
                printed_header = 1;
            }
            fprintf(out, "B%02x_A%04x = \"%s\"\n", r->dst_bank, r->dst_addr, doc);
        }
    }
}

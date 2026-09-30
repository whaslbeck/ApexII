#include "apex_inline_detect.h"

#include "apex.h"
#include "apex_analysis.h"
#include "apex_config.h"
#include "cpu6809.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- field lists ------------------------------------------------------- */

enum {
    F_BYTE = 1,
    F_WORD = 2,
    F_FAR = 3,     /* far pointer to code */
    F_DPAIR = 4,   /* read into D: reported as word */
    F_FAR_DATA = 5 /* far pointer to data */
};

#define MAX_FIELDS 32
#define MAX_INLINE_LEN 32u

typedef struct {
    unsigned off;
    unsigned size; /* 1 or 2 bytes as read */
    int into_d;    /* read into D (LDD / PULU A,B): a word or two bytes */
} Access;

typedef struct {
    int type[MAX_FIELDS]; /* F_* */
    int count;
    int partial; /* some bytes were never read directly (filled as byte) */
} FieldList;

static unsigned field_size(int type) {
    return (type == F_FAR || type == F_FAR_DATA) ? 3u : type == F_DPAIR ? 2u : (unsigned)type;
}

static void spec_from_fields(const FieldList *f, char *out, size_t out_size) {
    int i;
    size_t used = 0;

    out[0] = '\0';
    for (i = 0; i < f->count && used + 12 < out_size; i++) {
        const char *name = f->type[i] == F_FAR        ? "far_code"
                           : f->type[i] == F_FAR_DATA ? "far_data"
                           : f->type[i] == F_WORD     ? "word"
                                                      : "byte";
        used += (size_t)snprintf(out + used, out_size - used, "%s%s", i ? ", " : "", name);
    }
}

/* Lay the recorded accesses over [0, len): each access becomes a byte or word
   field; bytes never read directly are filled as bytes (partial). */
static int fields_from_accesses(const Access *acc, int nacc, unsigned len, FieldList *f) {
    unsigned o = 0;
    int i;

    f->count = 0;
    f->partial = 0;
    while (o < len && f->count < MAX_FIELDS) {
        int best = -1;
        for (i = 0; i < nacc; i++) {
            if (acc[i].off == o && o + acc[i].size <= len &&
                (best < 0 || acc[i].size > acc[best].size)) {
                best = i;
            }
        }
        if (best >= 0) {
            f->type[f->count++] =
                acc[best].size == 2u ? (acc[best].into_d ? F_DPAIR : F_WORD) : F_BYTE;
            o += acc[best].size;
        } else {
            f->type[f->count++] = F_BYTE;
            f->partial = 1;
            o++;
        }
    }
    return o == len;
}

/* ---- 6809 helpers ------------------------------------------------------ */

enum { IDX_CONST, IDX_INC1, IDX_INC2, IDX_OTHER };

/* Decode an indexed postbyte (p points at it).  reg: 0=X 1=Y 2=U 3=S. */
static void idx_decode(const uint8_t *p, size_t avail, int *reg, int *mode, long *offset) {
    uint8_t pb = p[0];

    *reg = (pb >> 5) & 3;
    *offset = 0;
    if (!(pb & 0x80u)) {
        long o = pb & 0x1f;
        *offset = (o & 0x10) ? o - 32 : o;
        *mode = IDX_CONST;
        return;
    }
    if (pb & 0x10u) {
        *mode = IDX_OTHER; /* indirect */
        return;
    }
    switch (pb & 0x0fu) {
    case 0x0:
        *mode = IDX_INC1;
        return;
    case 0x1:
        *mode = IDX_INC2;
        return;
    case 0x4:
        *mode = IDX_CONST;
        return;
    case 0x8:
        if (avail >= 2) {
            *offset = (int8_t)p[1];
            *mode = IDX_CONST;
            return;
        }
        break;
    case 0x9:
        if (avail >= 3) {
            *offset = (int16_t)((p[1] << 8) | p[2]);
            *mode = IDX_CONST;
            return;
        }
        break;
    default:
        break;
    }
    *mode = IDX_OTHER;
}

static int is_indexed_op(int prefix, uint8_t op) {
    if (prefix == 0) {
        return (op >= 0x60 && op <= 0x6f) || (op >= 0xa0 && op <= 0xaf) ||
               (op >= 0xe0 && op <= 0xef) || (op >= 0x30 && op <= 0x33);
    }
    return (op >= 0xa0 && op <= 0xaf) || (op >= 0xe0 && op <= 0xef);
}

static unsigned op_width(int prefix, uint8_t op) {
    uint8_t lo = op & 0x0f;
    if (prefix == 0) {
        if ((op & 0xf0) == 0xa0 || (op & 0xf0) == 0xe0) {
            if (lo == 0x3 || lo == 0xc || lo == 0xd || lo == 0xe || lo == 0xf) {
                return (op == 0xad) ? 0 : 2u; /* 0xAD = JSR */
            }
        }
        return 1u;
    }
    if (prefix == 0x10) {
        return (lo == 0x3 || lo == 0xc || lo == 0xe || lo == 0xf) ? 2u : 1u;
    }
    return (lo == 0x3 || lo == 0xc) ? 2u : 1u;
}

/* Pull/push byte counts per mask bit: CC A B DP X Y U/S PC */
static const unsigned stack_bytes[8] = {1, 1, 1, 1, 2, 2, 2, 2};

static unsigned mask_bytes(uint8_t mask) {
    unsigned n = 0, b;
    for (b = 0; b < 8; b++) {
        if (mask & (1u << b)) {
            n += stack_bytes[b];
        }
    }
    return n;
}

/* ---- the walker -------------------------------------------------------- */

typedef struct {
    int known[3]; /* X, Y, U hold "inline base + off" */
    long off[3];
    long depth; /* bytes pushed since entry: the return address is at depth,S */
    int popped; /* the return address was pulled into a register */
    int pushed_ptr;
    unsigned found_len; /* return address written back: the inline length */
    Access acc[MAX_FIELDS];
    int nacc;
} WalkState;

static void record_d(WalkState *w, long off, unsigned size, int into_d) {
    if (off >= 0 && off < (long)MAX_INLINE_LEN && w->nacc < MAX_FIELDS) {
        w->acc[w->nacc].off = (unsigned)off;
        w->acc[w->nacc].size = size;
        w->acc[w->nacc].into_d = into_d && size == 2u;
        w->nacc++;
    }
}

static void record(WalkState *w, long off, unsigned size) {
    record_d(w, off, size, 0);
}

static int reg3(int idxreg) {
    return idxreg <= 2 ? idxreg : -1; /* X Y U; S is not a pointer here */
}

/* TFR/EXG register code -> tracked index (X Y U) or -1 */
static int tfr_reg(unsigned code) {
    return code == 1 ? 0 : code == 2 ? 1 : code == 3 ? 2 : -1;
}

/* Walk the routine at data[0..avail).  Returns the inline length (0 = not an
   inline routine) and the accesses seen. */
static unsigned walk_routine(const uint8_t *data, size_t avail, WalkState *w) {
    size_t pos = 0;
    int steps;

    memset(w, 0, sizeof(*w));
    for (steps = 0; steps < 40 && pos < avail; steps++) {
        char tmp[64];
        Cpu6809InstrInfo info =
            cpu6809_disassemble_info(data + pos, avail - pos, 0, tmp, sizeof(tmp));
        int prefix = 0;
        uint8_t op;
        size_t pbi;

        if (info.size == 0) {
            return w->found_len;
        }
        op = data[pos];
        if (op == 0x10 || op == 0x11) {
            prefix = op;
            op = data[pos + 1];
        }
        pbi = pos + (prefix ? 2u : 1u); /* operand / postbyte */

        if (prefix == 0 && op == 0x34) { /* PSHS */
            uint8_t mask = data[pbi];
            w->depth += (long)mask_bytes(mask);
            if (w->popped) {
                int r;
                for (r = 0; r < 3; r++) {
                    static const uint8_t bit[3] = {0x10, 0x20, 0x40};
                    if ((mask & bit[r]) && w->known[r]) {
                        w->pushed_ptr = r + 1;
                    }
                }
            }
        } else if (prefix == 0 && op == 0x35) { /* PULS */
            uint8_t mask = data[pbi];
            unsigned b, cur = 0;
            for (b = 0; b < 8; b++) {
                if (!(mask & (1u << b))) {
                    continue;
                }
                if (b >= 4 && b <= 6) {
                    int r = (int)b - 4; /* X Y U */
                    if (!w->popped && (long)cur == w->depth) {
                        w->known[r] = 1; /* pulled the return address */
                        w->off[r] = 0;
                        w->popped = 1;
                    } else {
                        w->known[r] = 0;
                    }
                }
                if (b == 7) {
                    return w->found_len; /* PULS ...,PC: returns (0 unless written back) */
                }
                cur += stack_bytes[b];
            }
            w->depth -= (long)cur;
        } else if (prefix == 0 && op == 0x37) { /* PULU */
            uint8_t mask = data[pbi];
            unsigned b;
            if (!w->known[2]) {
                return 0;
            }
            for (b = 0; b < 8; b++) {
                if (!(mask & (1u << b))) {
                    continue;
                }
                if (b == 1 && (mask & 0x04)) { /* A and B: one word (D) */
                    record_d(w, w->off[2], 2, 1);
                    w->off[2] += 2;
                    b = 2;
                    continue;
                }
                record(w, w->off[2], stack_bytes[b]);
                w->off[2] += (long)stack_bytes[b];
                if (b == 4 || b == 5) {
                    w->known[b - 4] = 0;
                }
            }
        } else if (prefix == 0 && (op == 0x1f || op == 0x1e)) { /* TFR / EXG */
            unsigned src = data[pbi] >> 4, dst = data[pbi] & 0x0f;
            int s = tfr_reg(src), d = tfr_reg(dst);
            if (op == 0x1f) {
                if (d >= 0) {
                    w->known[d] = s >= 0 && w->known[s];
                    w->off[d] = s >= 0 ? w->off[s] : 0;
                }
            } else {
                int ks = s >= 0 && w->known[s], kd = d >= 0 && w->known[d];
                long os = s >= 0 ? w->off[s] : 0, od = d >= 0 ? w->off[d] : 0;
                if (s >= 0) {
                    w->known[s] = kd;
                    w->off[s] = od;
                }
                if (d >= 0) {
                    w->known[d] = ks;
                    w->off[d] = os;
                }
            }
        } else if (is_indexed_op(prefix, op)) {
            int reg, mode;
            long k;
            int base;

            idx_decode(data + pbi, avail - pbi, &reg, &mode, &k);
            base = reg3(reg);
            if (prefix == 0 && op >= 0x30 && op <= 0x33) { /* LEAX/LEAY/LEAS/LEAU */
                int dst = op == 0x30 ? 0 : op == 0x31 ? 1 : op == 0x33 ? 2 : -1;
                if (op == 0x32 && reg == 3 && mode == IDX_CONST) {
                    w->depth -= k; /* LEAS k,S */
                } else if (dst >= 0) {
                    if (base >= 0 && w->known[base] && mode == IDX_CONST) {
                        w->known[dst] = 1;
                        w->off[dst] = w->off[base] + k;
                    } else {
                        w->known[dst] = 0;
                    }
                }
            } else if (prefix == 0 && op == 0x6e) { /* JMP indexed */
                if (w->popped && base >= 0 && w->known[base] && mode == IDX_CONST && k == 0) {
                    return (unsigned)w->off[base]; /* JMP ,R past the inline data */
                }
                return w->found_len;
            } else if (prefix == 0 && op == 0xad) { /* JSR indexed */
                return w->found_len;
            } else {
                unsigned width = op_width(prefix, op);
                int is_store = (prefix == 0 && (op == 0xaf || op == 0xef || op == 0xed)) ||
                               (prefix == 0x10 && (op == 0xaf || op == 0xef));
                int dst = -1; /* tracked register this instruction loads */

                if (prefix == 0 && op == 0xae) {
                    dst = 0;
                } else if (prefix == 0 && op == 0xee) {
                    dst = 2;
                } else if (prefix == 0x10 && op == 0xae) {
                    dst = 1;
                }
                if (reg == 3 && mode == IDX_CONST) {
                    if (k == w->depth && !w->popped) {
                        if (is_store) {
                            int src = (prefix == 0 && op == 0xaf)      ? 0
                                      : (prefix == 0 && op == 0xef)    ? 2
                                      : (prefix == 0x10 && op == 0xaf) ? 1
                                                                       : -1;
                            if (src >= 0 && w->known[src] && w->off[src] > 0 && !w->found_len) {
                                /* return address written back: that is the length; keep
                                   walking, many routines read the bytes only afterwards */
                                w->found_len = (unsigned)w->off[src];
                                pos += info.size;
                                continue;
                            }
                            return w->found_len;
                        }
                        if (dst >= 0) {
                            w->known[dst] = 1; /* loaded the return address */
                            w->off[dst] = 0;
                            pos += info.size;
                            continue;
                        }
                    }
                } else if (base >= 0 && w->known[base] && !is_store) {
                    int into_d = prefix == 0 && op == 0xec; /* LDD */
                    if (mode == IDX_INC1 || mode == IDX_INC2) {
                        unsigned step = mode == IDX_INC1 ? 1u : 2u;
                        record_d(w, w->off[base], width == 2 ? 2u : step == 2 ? 2u : 1u, into_d);
                        w->off[base] += (long)step;
                    } else if (mode == IDX_CONST) {
                        record_d(w, w->off[base] + k, width ? width : 1u, into_d);
                    }
                }
                if (dst >= 0 && !(reg == 3 && mode == IDX_CONST && k == w->depth)) {
                    w->known[dst] = 0;
                }
            }
        } else {
            /* control flow ends the walk */
            if ((prefix == 0 && (op == 0x39 || op == 0x3b)) && w->popped && w->pushed_ptr) {
                int r = w->pushed_ptr - 1;
                return w->known[r] ? (unsigned)w->off[r] : 0; /* PSHS R; RTS */
            }
            if (info.has_target || (prefix == 0 && (op == 0x39 || op == 0x3b || op == 0x7e ||
                                                    op == 0x0e || op == 0xbd || op == 0x9d))) {
                return w->found_len; /* control flow ends the walk */
            }
            /* other writes to tracked registers */
            if (prefix == 0 && (op == 0x8e || op == 0x9e || op == 0xbe || op == 0x3a)) {
                w->known[0] = 0;
            } else if (prefix == 0 && (op == 0xce || op == 0xde || op == 0xfe)) {
                w->known[2] = 0;
            } else if (prefix == 0x10 && (op == 0x8e || op == 0x9e || op == 0xbe)) {
                w->known[1] = 0;
            }
        }
        pos += info.size;
    }
    return w->found_len;
}

/* ---- callsites --------------------------------------------------------- */

#define MAX_CALLSITES 64

typedef struct {
    const uint8_t *data; /* bytes after the call instruction */
    size_t avail;
} Callsite;

static int collect_callsites(const ApexProject *p, uint32_t addr, Callsite *cs, int max) {
    size_t i;
    int n = 0;

    if (!p->refs.sorted) {
        return 0;
    }
    for (i = refs_lower_bound(&p->refs, 0xffu, addr);
         i < p->refs.count && p->refs.items[i].bank == 0xffu && p->refs.items[i].addr == addr;
         i++) {
        const Reference *r = &p->refs.items[i];
        size_t off, end;
        char tmp[64];
        Cpu6809InstrInfo info;

        if (r->row_index >= 0 || strcmp(r->kind, "code") != 0) {
            continue;
        }
        if (r->source_bank == 0xffu) {
            if (r->source_addr < APEX_SYSTEM_ORG || r->source_addr > 0xffffu) {
                continue;
            }
            off = p->paged_size + (r->source_addr - APEX_SYSTEM_ORG);
            end = p->rom.size;
        } else {
            int bi = bank_index_for_id(p->rom.data, p->banks, r->source_bank);
            if (bi < 0 || r->source_addr < APEX_PAGED_ORG || r->source_addr >= APEX_SYSTEM_ORG) {
                continue;
            }
            off = (size_t)bi * APEX_BANK_SIZE + (r->source_addr - APEX_PAGED_ORG);
            end = (size_t)(bi + 1) * APEX_BANK_SIZE;
        }
        info = cpu6809_disassemble_info(p->rom.data + off, end - off, r->source_addr, tmp,
                                        sizeof(tmp));
        if (info.size == 0 || !(info.flags & CPU6809_CALL) || !info.has_target ||
            (info.target & 0xffffu) != addr) {
            continue; /* only a call passes inline data */
        }
        if (n < max) {
            cs[n].data = p->rom.data + off + info.size;
            cs[n].avail = end - off - info.size;
            n++;
        }
    }
    return n;
}

static int callsites_resume_ok(const Callsite *cs, int n, unsigned len) {
    int i, ok = 0;
    for (i = 0; i < n; i++) {
        char tmp[64];
        if (cs[i].avail > len &&
            cpu6809_disassemble_info(cs[i].data + len, cs[i].avail - len, 0, tmp, sizeof(tmp))
                    .size > 0) {
            ok++;
        }
    }
    return ok;
}

static int word_is_rom_addr(const Callsite *cs, int n, unsigned o) {
    int i;
    if (n < 1) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned v;
        if (cs[i].avail < o + 2u) {
            return 0;
        }
        v = ((unsigned)cs[i].data[o] << 8) | cs[i].data[o + 1u];
        if (v < APEX_PAGED_ORG) {
            return 0;
        }
    }
    return 1;
}

static int far_pointer_at(const ApexProject *p, const Callsite *cs, int n, unsigned o) {
    int i;
    if (n < 2) {
        return 0; /* one call site is too little evidence for a far pointer */
    }
    for (i = 0; i < n; i++) {
        unsigned v;
        if (cs[i].avail < o + 3u) {
            return 0;
        }
        v = ((unsigned)cs[i].data[o] << 8) | cs[i].data[o + 1u];
        if (cs[i].data[o + 2u] == 0xffu) {
            if (v < APEX_SYSTEM_ORG) {
                return 0; /* bank 0xff: a system-bank target */
            }
        } else if (v < APEX_PAGED_ORG || v >= APEX_SYSTEM_ORG ||
                   bank_index_for_id(p->rom.data, p->banks, cs[i].data[o + 2u]) < 0) {
            return 0;
        }
    }
    return 1;
}

/* A 2-byte read into D (LDD, PULU A,B) is reported as a word, as the CPU reads
   it.  Whether the callee treats it as a word or as two bytes is not visible in
   the wrapper, and the hand-made signatures are not consistent about it either
   (the same OS routine is "word" in one game and "byte, byte" in another). */
static void resolve_d_pairs(FieldList *f) {
    int i;
    for (i = 0; i < f->count; i++) {
        if (f->type[i] == F_DPAIR) {
            f->type[i] = F_WORD;
        }
    }
}

/* Does any call pass a far pointer (at o) whose target the analysis already
   classifies as data?  Then the field is far_data, not far_code. */
static int far_targets_data(const ApexProject *p, const Callsite *cs, int n, unsigned o) {
    int i;
    for (i = 0; i < n; i++) {
        unsigned v = ((unsigned)cs[i].data[o] << 8) | cs[i].data[o + 1u];
        if (data_range_at(cs[i].data[o + 2u], v, &p->data_ranges)) {
            return 1;
        }
    }
    return 0;
}

/* Upgrade word,byte pairs to far pointers where every call passes a valid far
   pointer there: far_code, or far_data when a target is known data. */
static void refine_far(const ApexProject *p, const Callsite *cs, int n, FieldList *f) {
    int i;
    unsigned o = 0;
    for (i = 0; i + 1 < f->count; i++) {
        if ((f->type[i] == F_WORD || f->type[i] == F_DPAIR) && f->type[i + 1] == F_BYTE &&
            far_pointer_at(p, cs, n, o)) {
            int j;
            f->type[i] = far_targets_data(p, cs, n, o) ? F_FAR_DATA : F_FAR;
            for (j = i + 1; j + 1 < f->count; j++) {
                f->type[j] = f->type[j + 1];
            }
            f->count--;
        }
        o += field_size(f->type[i]);
    }
}

/* Layout for a routine the walker could not read: from the call sites. */
static void fields_from_callsites(const ApexProject *p, const Callsite *cs, int n, unsigned len,
                                  FieldList *f) {
    unsigned o = 0;
    f->count = 0;
    f->partial = 1;
    while (o < len && f->count < MAX_FIELDS) {
        if (len - o >= 3u && far_pointer_at(p, cs, n, o)) {
            f->type[f->count++] = far_targets_data(p, cs, n, o) ? F_FAR_DATA : F_FAR;
            o += 3u;
        } else if (len - o >= 2u && n >= 2 && word_is_rom_addr(cs, n, o)) {
            f->type[f->count++] = F_WORD;
            o += 2u;
        } else if (len == 2u && o == 0 && n == 0) {
            f->type[f->count++] = F_WORD; /* a lone 2-byte argument is usually a word */
            o += 2u;
        } else {
            f->type[f->count++] = F_BYTE;
            o++;
        }
    }
}

/* ---- detection --------------------------------------------------------- */

static void push_finding(ApexInlineFindings *out, const ApexInlineFinding *f) {
    if (out->count == out->cap) {
        size_t cap = out->cap ? out->cap * 2u : 64u;
        ApexInlineFinding *ni = realloc(out->items, cap * sizeof(*ni));
        if (!ni) {
            die("out of memory");
        }
        out->items = ni;
        out->cap = cap;
    }
    out->items[out->count++] = *f;
}

static int system_code_label(const ApexProject *p, uint32_t addr) {
    size_t idx[8], n, k;
    n = label_set_find_all((LabelSet *)&p->system_labels, addr, idx, 8);
    for (k = 0; k < n && k < 8; k++) {
        if (p->system_labels.items[idx[k]].is_code) {
            return 1;
        }
    }
    return 0;
}

static int finding_cmp(const void *a, const void *b) {
    const ApexInlineFinding *x = (const ApexInlineFinding *)a;
    const ApexInlineFinding *y = (const ApexInlineFinding *)b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

/* Does the code just before sys[off] end a routine?  (PULS ...,PC / RTS / RTI /
   JMP ext / BRA / LBRA, or an inline marker.)  Used to accept a routine found by
   its prologue alone, which the analysis has not reached yet. */
static int after_routine_end(const uint8_t *sys, size_t off) {
    if (off >= 1 && (sys[off - 1] == 0x39 || sys[off - 1] == 0x3b)) {
        return 1; /* RTS / RTI */
    }
    if (off >= 2 && sys[off - 2] == 0x35 && (sys[off - 1] & 0x80)) {
        return 1; /* PULS ...,PC */
    }
    if (off >= 2 && (sys[off - 2] == 0x20 || (sys[off - 2] == 0xcf && sys[off - 1] >= 0x73))) {
        return 1; /* BRA rel8, or an inline marker */
    }
    if (off >= 3 && (sys[off - 3] == 0x7e || sys[off - 3] == 0x16)) {
        return 1; /* JMP ext / LBRA */
    }
    return 0;
}

static void analyse_routine(const ApexProject *p, uint32_t addr, unsigned marker_len,
                            ApexInlineFindings *out) {
    const uint8_t *sys = p->rom.data + p->paged_size;
    size_t off = addr - APEX_SYSTEM_ORG;
    WalkState w;
    FieldList f;
    Callsite cs[MAX_CALLSITES];
    int ncs;
    unsigned walker_len;
    ApexInlineFinding r;
    const InlineSignature *sig;

    walker_len = walk_routine(sys + off, APEX_SYSTEM_SIZE - off, &w);
    if (walker_len > MAX_INLINE_LEN) {
        walker_len = 0;
    }
    if (!marker_len && !walker_len) {
        return;
    }
    memset(&r, 0, sizeof(r));
    r.addr = addr;
    r.marker_len = marker_len;
    r.walker_len = walker_len;
    r.sources = (marker_len ? APEX_INL_SRC_MARKER : 0u) | (walker_len ? APEX_INL_SRC_WALKER : 0u);
    /* What the code does wins over the marker: on one older OS (harr_lx2) a group
       of routines reads one byte more than its marker states, and the code is
       right there (e.g. LDD ,X++ behind a 0x74 marker). */
    r.length = walker_len ? walker_len : marker_len;
    r.is_code = system_code_label(p, addr);
    ncs = collect_callsites(p, addr, cs, MAX_CALLSITES);
    r.callsites = ncs;
    r.callsites_ok = callsites_resume_ok(cs, ncs < MAX_CALLSITES ? ncs : MAX_CALLSITES, r.length);

    if (walker_len && fields_from_accesses(w.acc, w.nacc, r.length, &f)) {
        refine_far(p, cs, ncs, &f); /* before resolving D pairs: they may be far addresses */
        resolve_d_pairs(&f);
        r.confidence = f.partial ? APEX_INL_MEDIUM : APEX_INL_HIGH;
        if (marker_len && marker_len != walker_len) {
            r.confidence = APEX_INL_MEDIUM;
            snprintf(r.note, sizeof(r.note), "code reads %u byte(s), marker says %u", walker_len,
                     marker_len);
        } else if (f.partial) {
            snprintf(r.note, sizeof(r.note), "some bytes not read directly (typed as byte)");
        }
    } else {
        fields_from_callsites(p, cs, ncs, r.length, &f);
        r.confidence = ncs > 0 ? APEX_INL_MEDIUM : APEX_INL_LOW;
        snprintf(r.note, sizeof(r.note), "layout guessed from %d call site(s)", ncs);
    }
    if (ncs > 0 && r.callsites_ok < ncs) {
        if (r.confidence > APEX_INL_MEDIUM) {
            r.confidence = APEX_INL_MEDIUM;
        }
        if (!r.note[0]) {
            snprintf(r.note, sizeof(r.note), "code does not resume after %d of %d call(s)",
                     ncs - r.callsites_ok, ncs);
        }
    }
    spec_from_fields(&f, r.spec, sizeof(r.spec));

    sig = inline_signature_for(&p->inline_sigs, 0xffu, addr);
    if (sig) {
        unsigned cfg_len = sig->length;
        char buf[128];
        const TableSchema *s = &sig->schema;
        size_t i, used = 0;
        buf[0] = '\0';
        for (i = 0; i < s->count && used + 24 < sizeof(buf); i++) {
            const char *nm = s->items[i].type_name            ? s->items[i].type_name
                             : s->items[i].kind == TABLE_WORD ? "word"
                             : s->items[i].kind == TABLE_BYTE ? "byte"
                                                              : "field";
            used += (size_t)snprintf(buf + used, sizeof(buf) - used, "%s%s", i ? ", " : "", nm);
        }
        snprintf(r.config_spec, sizeof(r.config_spec), "%s", buf);
        r.status = (table_schema_is_variable(s) || cfg_len == r.length) ? APEX_INL_CONFIRMED
                                                                        : APEX_INL_CONFLICT;
    }
    push_finding(out, &r);
}

int apex_detect_inline_routines(const ApexProject *p, ApexInlineFindings *out) {
    const uint8_t *sys;
    size_t i;
    uint32_t first_marker = 0x10000u;
    unsigned char *seen;
    unsigned short *raw_jsr;

    memset(out, 0, sizeof(*out));
    if (!p || p->rom.size < APEX_SYSTEM_SIZE) {
        return 1;
    }
    sys = p->rom.data + p->paged_size;
    seen = calloc(APEX_SYSTEM_SIZE, 1);
    if (!seen) {
        return 1;
    }

    /* A: markers 0xCF, 0x73+N directly before the routine */
    for (i = 2; i < APEX_SYSTEM_SIZE - 16u; i++) {
        if (sys[i - 2] == 0xcfu && sys[i - 1] >= 0x74u && sys[i - 1] <= 0x7fu) {
            uint32_t addr = (uint32_t)(APEX_SYSTEM_ORG + i);
            if (addr >= 0xfff0u) {
                continue;
            }
            analyse_routine(p, addr, (unsigned)(sys[i - 1] - 0x73u), out);
            seen[i] = 1;
            if (addr < first_marker) {
                first_marker = addr;
            }
        }
    }
    raw_jsr = calloc(0x10000u, sizeof(*raw_jsr));
    if (raw_jsr) {
        size_t k;
        for (k = 0; k + 2 < p->rom.size; k++) {
            if (p->rom.data[k] == 0xbdu) {
                unsigned t = ((unsigned)p->rom.data[k + 1] << 8) | p->rom.data[k + 2];
                if (raw_jsr[t] < 0xffffu) {
                    raw_jsr[t]++;
                }
            }
        }
    }
    /* B: the return-address walker over every known system-bank routine */
    for (i = 0; i < p->system_labels.count; i++) {
        const Label *l = &p->system_labels.items[i];
        size_t off;
        if (!l->is_code || l->addr < APEX_SYSTEM_ORG || l->addr >= 0xfff0u) {
            continue;
        }
        off = l->addr - APEX_SYSTEM_ORG;
        if (seen[off]) {
            continue;
        }
        seen[off] = 1;
        analyse_routine(p, l->addr, 0, out);
    }
    /* C: prologue scan for routines the analysis has not reached yet: a PSHS
       (optionally after LEAS -n,S) right after the end of a previous routine,
       which the walker then has to confirm.  Their callers are usually not
       analysed yet either, so raw "JSR ext" byte patterns anywhere in the ROM
       count as supporting evidence. */
    for (i = 3; i + 16 < APEX_SYSTEM_SIZE; i++) {
        if (seen[i] || !(sys[i] == 0x34 || (sys[i] == 0x32 && sys[i + 2] == 0x34))) {
            continue;
        }
        if (!after_routine_end(sys, i)) {
            continue;
        }
        analyse_routine(p, (uint32_t)(APEX_SYSTEM_ORG + i), 0, out);
        seen[i] = 1;
    }
    /* A marker-less finding needs evidence that a routine starts there: an
       analysed call, raw JSRs, or the end of the previous routine right before
       it.  (Without, it is typically a branch target inside a routine that
       reads the enclosing routine's return address.) */
    {
        size_t k;
        for (k = 0; k < out->count; k++) {
            ApexInlineFinding *f = &out->items[k];
            unsigned raw = raw_jsr ? raw_jsr[f->addr & 0xffffu] : 0;
            if (f->marker_len || f->callsites > 0 || raw >= 2u) {
                continue;
            }
            if (!after_routine_end(sys, f->addr - APEX_SYSTEM_ORG)) {
                if (f->confidence > APEX_INL_LOW) {
                    f->confidence = APEX_INL_LOW;
                }
                snprintf(f->note, sizeof(f->note),
                         "no caller and no routine end before it: branch target?");
            } else if (f->confidence > APEX_INL_MEDIUM) {
                f->confidence = APEX_INL_MEDIUM;
                if (!f->note[0]) {
                    snprintf(f->note, sizeof(f->note), "no known caller (%u raw JSR)", raw);
                }
            }
        }
    }
    free(raw_jsr);
    free(seen);

    /* A marker without any supporting evidence (not code, no callers, walker
       silent) is likely a coincidental 0xCF 0x7n in data: drop it. */
    {
        size_t r, wi = 0;
        for (r = 0; r < out->count; r++) {
            ApexInlineFinding *f = &out->items[r];
            if (f->sources == APEX_INL_SRC_MARKER && !f->is_code && f->callsites == 0) {
                continue;
            }
            out->items[wi++] = *f;
        }
        out->count = wi;
    }
    if (out->count > 1) {
        qsort(out->items, out->count, sizeof(out->items[0]), finding_cmp);
    }

    /* The marker-less routines at the front of the bank come ordered by inline
       length (1-byte ones first, then 2, 3, ...): one out of order is suspect. */
    {
        unsigned max_len = 0;
        for (i = 0; i < out->count; i++) {
            ApexInlineFinding *f = &out->items[i];
            if (f->addr >= first_marker || f->sources != APEX_INL_SRC_WALKER) {
                continue;
            }
            if (f->length < max_len) {
                if (f->confidence > APEX_INL_MEDIUM) {
                    f->confidence = APEX_INL_MEDIUM;
                }
                if (!f->note[0]) {
                    snprintf(f->note, sizeof(f->note), "out of length order (after %u-byte ones)",
                             max_len);
                }
            } else {
                max_len = f->length;
            }
        }
    }

    /* ...and that order is evidence too: a walker-confirmed routine without a
       known caller, right after the end of the previous routine, whose length
       lies between the high-confidence routines around it, belongs to the run. */
    {
        size_t k;
        for (k = 0; k < out->count; k++) {
            ApexInlineFinding *f = &out->items[k];
            unsigned lo = 0, hi = 0;
            size_t j;
            if (f->addr >= first_marker || f->confidence != APEX_INL_MEDIUM ||
                f->sources != APEX_INL_SRC_WALKER || f->walker_len != f->length ||
                strstr(f->note, "not read directly") || strstr(f->note, "out of length") ||
                !after_routine_end(sys, f->addr - APEX_SYSTEM_ORG)) {
                continue;
            }
            for (j = k; j-- > 0;) {
                if (out->items[j].confidence == APEX_INL_HIGH) {
                    lo = out->items[j].length;
                    break;
                }
            }
            for (j = k + 1; j < out->count && out->items[j].addr < first_marker; j++) {
                if (out->items[j].confidence == APEX_INL_HIGH) {
                    hi = out->items[j].length;
                    break;
                }
            }
            if (lo && hi && lo <= f->length && f->length <= hi) {
                f->confidence = APEX_INL_HIGH;
                snprintf(f->note, sizeof(f->note), "in the length-ordered run (%u..%u)", lo, hi);
            }
        }
    }
    return 0;
}

void apex_free_inline_findings(ApexInlineFindings *findings) {
    if (findings) {
        free(findings->items);
        memset(findings, 0, sizeof(*findings));
    }
}

static int marker_is_data(const ApexProject *p, uint32_t addr) {
    size_t i;
    for (i = 0; i < p->data_ranges.count; i++) {
        const DataRange *r = &p->data_ranges.items[i];
        if (r->bank == 0xffu && r->addr == addr) {
            return 1;
        }
    }
    return 0;
}

int apex_apply_inline_findings(ApexProject *p, const ApexInlineFindings *findings,
                               int min_confidence, int replace_conflicts, long index) {
    size_t i;
    int applied = 0;

    if (!p || !findings) {
        return 0;
    }
    apex_project_begin_edit_group(p, "detect inline");
    for (i = 0; i < findings->count; i++) {
        const ApexInlineFinding *f = &findings->items[i];
        if (index >= 0) {
            if ((size_t)index != i) {
                continue;
            }
        } else {
            if (f->confidence < min_confidence) {
                continue;
            }
            if (f->status == APEX_INL_CONFIRMED ||
                (f->status == APEX_INL_CONFLICT && !replace_conflicts)) {
                continue;
            }
        }
        if (apex_project_set_inline(p, 1, 0xffu, f->addr, f->spec) != 0) {
            continue;
        }
        if (f->marker_len && !marker_is_data(p, f->addr - 2u)) {
            apex_project_set_kind(p, 1, 0xffu, f->addr - 2u, APEX_KIND_DATA, "bytes[2]");
        }
        if (!f->is_code) {
            apex_project_set_kind(p, 1, 0xffu, f->addr, APEX_KIND_CODE, NULL);
        }
        applied++;
    }
    apex_project_end_edit_group(p);
    return applied;
}

const char *apex_inline_confidence_name(int confidence) {
    return confidence >= APEX_INL_HIGH ? "high" : confidence == APEX_INL_MEDIUM ? "medium" : "low";
}

const char *apex_inline_status_name(int status) {
    return status == APEX_INL_CONFIRMED  ? "confirmed"
           : status == APEX_INL_CONFLICT ? "conflict"
                                         : "new";
}

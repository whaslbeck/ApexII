#include "apex_card.h"

#include "apex.h"
#include "apex_analysis.h"
#include "cpu6809.h"

#include <stdlib.h>
#include <string.h>

#define CARD_SPAN 0x1000u /* how far a routine may reach from its start */

static void window(const ApexProject *p, uint8_t bank, const uint8_t **data, size_t *size,
                   uint32_t *org) {
    if (bank == 0xffu) {
        *data = p->rom.data + p->paged_size;
        *size = APEX_SYSTEM_SIZE;
        *org = APEX_SYSTEM_ORG;
    } else {
        int bi = bank_index_for_id(p->rom.data, p->banks, bank);
        *data = bi < 0 ? NULL : p->rom.data + (size_t)bi * APEX_BANK_SIZE;
        *size = bi < 0 ? 0 : APEX_BANK_SIZE;
        *org = APEX_PAGED_ORG;
    }
}

/* Routine starts of one bank: [entries] addresses, call targets, named code
   labels (a name someone gave) and code reached from two or more places --
   a jump there is a tail call into shared code, not a branch of this routine. */
typedef struct {
    uint32_t *addr;
    size_t count;
} Starts;

static int u32_cmp(const void *x, const void *y) {
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return a < b ? -1 : a > b;
}

static void starts_build(const ApexProject *p, uint8_t bank, Starts *st) {
    size_t cap = 256;
    st->addr = malloc(cap * sizeof(*st->addr));
    st->count = 0;
    const LabelSet *ls = NULL;
    if (bank == 0xffu) {
        ls = &p->system_labels;
    } else {
        int bi = bank_index_for_id(p->rom.data, p->banks, bank);
        ls = bi < 0 ? NULL : &p->bank_labels[bi];
    }
    for (size_t i = 0; ls && i < ls->count; i++) {
        const char *n = ls->items[i].name;
        int named = n && !(strlen(n) == 9 && n[0] == 'B' && n[3] == '_' && n[4] == 'A');
        if (!ls->items[i].is_explicit_entry && !(ls->items[i].is_code && named))
            continue;
        if (st->count == cap)
            st->addr = realloc(st->addr, (cap *= 2) * sizeof(*st->addr));
        st->addr[st->count++] = ls->items[i].addr;
    }
    for (size_t i = 0; i < p->refs.count; i++) {
        const Reference *r = &p->refs.items[i];
        if (r->bank != bank || r->row_index >= 0)
            continue;
        const uint8_t *data;
        size_t size;
        uint32_t org;
        window(p, r->source_bank, &data, &size, &org);
        if (!data || r->source_addr < org || r->source_addr >= org + size)
            continue;
        char t[64];
        Cpu6809InstrInfo in =
            cpu6809_disassemble_info(data + (r->source_addr - org), size - (r->source_addr - org),
                                     r->source_addr, t, sizeof(t));
        if (!in.has_target || in.target != r->addr)
            continue;
        if (st->count == cap)
            st->addr = realloc(st->addr, (cap *= 2) * sizeof(*st->addr));
        /* calls count at once; jumps / branches are marked twice-referenced
           below (negative marker: the top bit) */
        st->addr[st->count++] = (in.flags & CPU6809_CALL) ? r->addr : (r->addr | 0x80000000u);
    }
    qsort(st->addr, st->count, sizeof(*st->addr), u32_cmp);
    /* jump targets: keep those referenced from two or more places */
    size_t w = 0;
    for (size_t i = 0; i < st->count;) {
        uint32_t v = st->addr[i];
        size_t j = i;
        while (j < st->count && st->addr[j] == v)
            j++;
        if (!(v & 0x80000000u) || j - i >= 2)
            st->addr[w++] = v & 0x7fffffffu;
        i = j;
    }
    st->count = w;
    qsort(st->addr, st->count, sizeof(*st->addr), u32_cmp);
}

static int routine_start(const Starts *st, uint32_t addr) {
    return bsearch(&addr, st->addr, st->count, sizeof(*st->addr), u32_cmp) != NULL;
}

static void add_callee(ApexRoutineCard *c, uint8_t b, uint32_t a, int far, int tail) {
    for (size_t i = 0; i < c->callee_count; i++) {
        if (c->callees[i].bank == b && c->callees[i].addr == a) {
            c->callees[i].count++;
            return;
        }
    }
    c->callees = realloc(c->callees, (c->callee_count + 1) * sizeof(*c->callees));
    c->callees[c->callee_count++] = (ApexCardCallee){b, a, far, tail, 1};
}

static void add_ram(ApexRoutineCard *c, uint32_t a, int rd, int wr) {
    for (size_t i = 0; i < c->ram_count; i++) {
        if (c->ram[i].addr == a) {
            c->ram[i].reads += rd;
            c->ram[i].writes += wr;
            return;
        }
    }
    c->ram = realloc(c->ram, (c->ram_count + 1) * sizeof(*c->ram));
    c->ram[c->ram_count++] = (ApexCardRam){a, rd, wr};
}

/* RAM access of one instruction: direct (<0xNN, DP = 0) or extended. */
static void ram_access(ApexRoutineCard *c, const char *t, const Cpu6809InstrInfo *in) {
    if (strchr(t, '#') || strchr(t, ','))
        return; /* immediate / indexed */
    uint32_t a;
    const char *dp = strstr(t, "<0x");
    if (dp)
        a = (uint32_t)strtoul(dp + 1, NULL, 16);
    else if (in->has_addr_ref && in->addr_ref < 0x2000u && !in->has_target)
        a = in->addr_ref;
    else
        return;
    if (a >= 0x2000u)
        return;
    int st = !strncmp(t, "ST", 2) || !strncmp(t, "CLR", 3);
    int rmw = !strncmp(t, "INC", 3) || !strncmp(t, "DEC", 3) || !strncmp(t, "COM", 3) ||
              !strncmp(t, "NEG", 3) || !strncmp(t, "ASL", 3) || !strncmp(t, "ASR", 3) ||
              !strncmp(t, "LSL", 3) || !strncmp(t, "LSR", 3) || !strncmp(t, "ROL", 3) ||
              !strncmp(t, "ROR", 3);
    add_ram(c, a, !st || rmw, st || rmw);
}

static int loc_cmp(const void *x, const void *y) {
    const ApexCardLoc *a = x, *b = y;
    if (a->bank != b->bank)
        return a->bank < b->bank ? -1 : 1;
    return a->addr < b->addr ? -1 : a->addr > b->addr;
}

int apex_routine_card(const ApexProject *p, uint8_t bank, uint32_t addr, ApexRoutineCard *c) {
    memset(c, 0, sizeof(*c));
    c->bank = bank;
    c->addr = addr;
    const uint8_t *data;
    size_t size;
    uint32_t org;
    window(p, bank, &data, &size, &org);
    if (!data || addr < org || addr >= org + size)
        return -1;
    uint32_t hi = addr + CARD_SPAN < org + size ? addr + CARD_SPAN : org + (uint32_t)size;
    uint8_t *seen = calloc(hi - addr, 1);
    Starts st;
    starts_build(p, bank, &st);
    uint32_t *work = malloc(4096 * sizeof(*work));
    size_t nw = 0;
    work[nw++] = addr;
    c->low = addr;
    c->high = addr;
    while (nw) {
        uint32_t a = work[--nw];
        while (a >= addr && a < hi && !seen[a - addr]) {
            if (a != addr && routine_start(&st, a))
                break; /* the next routine (fall-through into it) */
            if (data_range_at(bank, a, &p->data_ranges) || table_def_at(bank, a, &p->tables))
                break;
            char t[64];
            Cpu6809InstrInfo in =
                cpu6809_disassemble_info(data + (a - org), size - (a - org), a, t, sizeof(t));
            if (!in.size)
                break;
            seen[a - addr] = 1;
            c->insn = realloc(c->insn, (c->insn_count + 1) * sizeof(*c->insn));
            c->insn[c->insn_count++] = (ApexCardLoc){bank, a};
            ram_access(c, t, &in);
            uint32_t next = a + (uint32_t)in.size;
            if (next > c->high)
                c->high = next;
            int is_call = in.has_target && (in.flags & CPU6809_CALL);
            if (is_call) {
                uint8_t tb = in.target >= APEX_SYSTEM_ORG ? 0xffu : bank;
                const InlineSignature *sig = inline_signature_for(&p->inline_sigs, bank, in.target);
                int far = 0;
                if (sig) {
                    /* a far-call helper: the callee is its inline far target */
                    size_t pos = next - org;
                    for (size_t f = 0; f < sig->schema.count; f++) {
                        const TableField *fl = &sig->schema.items[f];
                        if (fl->kind == TABLE_FAR_CODE && pos + 3u <= size) {
                            uint16_t fa = (uint16_t)((data[pos] << 8) | data[pos + 1]);
                            uint8_t fb = data[pos + 2];
                            if (valid_far_code_target(fa, fb, p->rom.data, p->banks)) {
                                add_callee(c, fa >= APEX_SYSTEM_ORG ? 0xffu : fb, fa, 1, 0);
                                far = 1;
                            }
                            break;
                        }
                        if (fl->kind == TABLE_BYTES_UNTIL || fl->kind == TABLE_COUNTED_BYTES)
                            break;
                        pos += fl->count * (fl->kind == TABLE_BYTE        ? 1u
                                            : table_kind_is_far(fl->kind) ? 3u
                                                                          : 2u);
                    }
                    int ok;
                    next += (uint32_t)inline_actual_length(sig, data, next - org, size, &ok);
                    if (!far)
                        add_callee(c, tb, in.target, 0, 0);
                    if (!ok || sig->flow_stop)
                        break;
                } else {
                    add_callee(c, tb, in.target, 0, 0);
                }
                a = next;
                continue;
            }
            if (in.has_target) { /* a branch / jump */
                uint8_t tb = in.target >= APEX_SYSTEM_ORG ? 0xffu : bank;
                int other = tb != bank || in.target < addr || in.target >= hi ||
                            (in.target != addr && routine_start(&st, in.target));
                if (other) {
                    add_callee(c, tb, in.target, 0, 1); /* tail call */
                } else if (nw < 4096) {
                    work[nw++] = in.target;
                }
                if (in.flags & CPU6809_FLOW_STOP)
                    break;
                a = next;
                continue;
            }
            if (in.flags & CPU6809_FLOW_STOP) {
                if (!strncmp(t, "RTS", 3) || !strncmp(t, "RTI", 3) || strstr(t, "PC"))
                    c->returns++;
                break;
            }
            a = next;
        }
    }
    free(seen);
    free(work);
    free(st.addr);
    if (!c->insn_count)
        return -1;
    qsort(c->insn, c->insn_count, sizeof(*c->insn), loc_cmp);
    c->insns = c->insn_count;
    c->low = c->insn[0].addr;
    /* callers: references to the start */
    for (size_t i = 0; i < p->refs.count; i++) {
        const Reference *r = &p->refs.items[i];
        if (r->bank != bank || r->addr != addr)
            continue;
        uint32_t src = r->row_index >= 0 ? r->row_cpu_addr : r->source_addr;
        int dup = 0;
        for (size_t k = 0; k < c->caller_count && !dup; k++)
            dup = c->callers[k].bank == r->source_bank && c->callers[k].addr == src;
        if (dup)
            continue;
        c->callers = realloc(c->callers, (c->caller_count + 1) * sizeof(*c->callers));
        c->callers[c->caller_count++] = (ApexCardCaller){r->source_bank, src, r->row_index >= 0};
    }
    /* data the routine refers to */
    for (size_t i = 0; i < p->refs.count; i++) {
        const Reference *r = &p->refs.items[i];
        if (r->row_index >= 0 || !apex_routine_card_has(c, r->source_bank, r->source_addr))
            continue;
        int code = 0;
        for (size_t k = 0; k < c->callee_count && !code; k++)
            code = c->callees[k].bank == r->bank && c->callees[k].addr == r->addr;
        if (code || apex_routine_card_has(c, r->bank, r->addr))
            continue; /* calls and branches */
        const uint8_t *td;
        size_t ts;
        uint32_t to;
        window(p, r->bank, &td, &ts, &to);
        if (!td || r->addr < to || r->addr >= to + ts)
            continue;
        const DataRange *dr = data_range_at(r->bank, r->addr, &p->data_ranges);
        int str = dr ? (dr->kind == DATA_STRING || dr->kind == DATA_STRING_FIXED)
                     : valid_string_len(td + (r->addr - to), ts - (r->addr - to)) >= 4;
        if (!dr && !str && !table_def_at(r->bank, r->addr, &p->tables))
            continue; /* not known data (e.g. a computed address) */
        c->data = realloc(c->data, (c->data_count + 1) * sizeof(*c->data));
        c->data[c->data_count++] = (ApexCardData){r->bank, r->addr, r->source_addr, str};
    }
    return 0;
}

int apex_routine_card_has(const ApexRoutineCard *c, uint8_t bank, uint32_t addr) {
    ApexCardLoc key = {bank, addr};
    return c->insn_count &&
           bsearch(&key, c->insn, c->insn_count, sizeof(*c->insn), loc_cmp) != NULL;
}

void apex_routine_card_free(ApexRoutineCard *c) {
    free(c->callers);
    free(c->callees);
    free(c->ram);
    free(c->data);
    free(c->insn);
    memset(c, 0, sizeof(*c));
}

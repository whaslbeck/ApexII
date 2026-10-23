#include "apex_args.h"

#include "cpu6809.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_reg_names[] = {"A", "B", "D", "X", "Y", "U", "inline"};

const char *apex_arg_reg_name(int reg) {
    return (reg >= 0 && reg <= APEX_ARG_INLINE) ? k_reg_names[reg] : "?";
}

static int reg_from_char(char c) {
    switch (toupper((unsigned char)c)) {
    case 'A':
        return APEX_ARG_REG_A;
    case 'B':
        return APEX_ARG_REG_B;
    case 'D':
        return APEX_ARG_REG_D;
    case 'X':
        return APEX_ARG_REG_X;
    case 'Y':
        return APEX_ARG_REG_Y;
    case 'U':
        return APEX_ARG_REG_U;
    default:
        return -1;
    }
}

int apex_parse_args_doc(const char *doc, char *buf, size_t bufsz, const char *types[6]) {
    int n = 0;
    size_t used = 0;
    for (int r = 0; r < 6; r++)
        types[r] = NULL;
    const char *p = doc ? strstr(doc, "@args") : NULL;
    if (!p || bufsz == 0)
        return 0;
    p += 5;
    /* REG:TYPE items, separated by blanks and/or commas, up to the end of line. */
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p || *p == '\n' || *p == '\r')
            break;
        int reg = reg_from_char(*p);
        if (reg < 0 || p[1] != ':')
            break;
        p += 2;
        const char *t = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_'))
            p++;
        size_t len = (size_t)(p - t);
        if (len == 0 || used + len + 1 > bufsz)
            break;
        memcpy(buf + used, t, len);
        buf[used + len] = '\0';
        if (!types[reg])
            n++;
        types[reg] = buf + used;
        used += len + 1;
    }
    return n;
}

/* Register tracking state for one basic block.  D is not tracked on its own:
   it is the pair A:B. */
typedef struct {
    int known;
    uint32_t value;
    uint32_t addr; /* the load instruction */
} RegVal;

static void reg_forget(RegVal *r, int reg) {
    if (reg == APEX_ARG_REG_D) {
        r[APEX_ARG_REG_A].known = 0;
        r[APEX_ARG_REG_B].known = 0;
    } else if (reg >= 0 && reg < 6) {
        r[reg].known = 0;
    }
}

static void reg_forget_all(RegVal *r) {
    for (int i = 0; i < 6; i++)
        r[i].known = 0;
}

/* Registers named in a TFR/EXG/PSH/PUL operand list ("A,B", "X,Y,PC"). */
static void forget_reg_list(RegVal *r, const char *list) {
    for (const char *p = list; *p; p++) {
        if (p != list && isalnum((unsigned char)p[-1]))
            continue; /* only the first letter of each name */
        if (isalpha((unsigned char)p[0]) && !isalnum((unsigned char)p[1]))
            reg_forget(r, reg_from_char(p[0]));
    }
}

/* Drop every tracked register the instruction may change.  Loads of an
   immediate were handled by the caller; this sees all other instructions. */
static void forget_written_regs(RegVal *r, const char *inst) {
    char mn[16];
    size_t m = 0;
    while (inst[m] && inst[m] != ' ' && m + 1 < sizeof(mn)) {
        mn[m] = (char)toupper((unsigned char)inst[m]);
        m++;
    }
    mn[m] = '\0';
    const char *opnd = inst + m;
    while (*opnd == ' ')
        opnd++;

    /* Auto-increment/decrement changes the index register (,X+ / ,--Y). */
    const char *comma = strchr(opnd, ',');
    if (comma && (strchr(comma, '+') || comma[1] == '-')) {
        const char *q = comma + 1;
        while (*q == '-')
            q++;
        reg_forget(r, reg_from_char(*q));
    }

    if (strncmp(mn, "ST", 2) == 0 || strncmp(mn, "CMP", 3) == 0 || strncmp(mn, "TST", 3) == 0 ||
        strncmp(mn, "BIT", 3) == 0 || strncmp(mn, "PSH", 3) == 0 || mn[0] == 'B' ||
        strncmp(mn, "LB", 2) == 0 || strcmp(mn, "NOP") == 0) {
        return;
    }
    if (strcmp(mn, "TFR") == 0) {
        const char *c = strchr(opnd, ',');
        if (c)
            forget_reg_list(r, c + 1);
        return;
    }
    if (strcmp(mn, "EXG") == 0 || strncmp(mn, "PUL", 3) == 0) {
        forget_reg_list(r, opnd);
        return;
    }
    if (strcmp(mn, "MUL") == 0) {
        reg_forget(r, APEX_ARG_REG_D);
        return;
    }
    if (strcmp(mn, "SEX") == 0 || strcmp(mn, "DAA") == 0) {
        reg_forget(r, APEX_ARG_REG_A);
        return;
    }
    if (strcmp(mn, "ABX") == 0) {
        reg_forget(r, APEX_ARG_REG_X);
        return;
    }
    if (m > 0)
        reg_forget(r, reg_from_char(mn[m - 1])); /* LDA, INCB, ADDD, LEAX, ... */
}

static int add_site(ApexArgSites *out, const ApexArgSite *s) {
    if (out->count == out->cap) {
        size_t ncap = out->cap ? out->cap * 2u : 128u;
        ApexArgSite *n = realloc(out->items, ncap * sizeof(*n));
        if (!n)
            return -1;
        out->items = n;
        out->cap = ncap;
    }
    out->items[out->count++] = *s;
    return 0;
}

/* The [types] entry of a declared name (sites borrow its name string). */
static const char *project_type_name(const ApexProject *p, const char *name) {
    const ConfigType *t = find_config_type(&p->config_types, name);
    return t ? t->name : NULL;
}

typedef struct {
    const ApexProject *p;
    const uint8_t *data;
    size_t size;
    uint32_t org;
    uint8_t bank;
    LabelSet *labels;
    int include_inline;
    ApexArgSites *out;
    /* apex_collect_calls: calls to the targets `want` accepts */
    ApexCallWant want;
    void *want_ctx;
    ApexCallSites *calls;
} ArgScan;

static int add_call(ApexCallSites *out, const ApexCallSite *c) {
    if (out->count == out->cap) {
        size_t ncap = out->cap ? out->cap * 2u : 128u;
        ApexCallSite *n = realloc(out->items, ncap * sizeof(*n));
        if (!n)
            return -1;
        out->items = n;
        out->cap = ncap;
    }
    out->items[out->count++] = *c;
    return 0;
}

/* Report the register arguments of one call (or tail jump) to (tb, ta). */
static int report_reg_args(const ArgScan *s, const RegVal *r, uint32_t call_addr, uint8_t tb,
                           uint32_t ta) {
    const char *doc = config_doc_at(&s->p->docs, tb, ta);
    char buf[256];
    const char *types[6];
    if (!doc || apex_parse_args_doc(doc, buf, sizeof(buf), types) == 0)
        return 0;
    for (int reg = 0; reg < 6; reg++) {
        const char *tn = types[reg] ? project_type_name(s->p, types[reg]) : NULL;
        if (!tn)
            continue;
        ApexArgSite site;
        memset(&site, 0, sizeof(site));
        if (reg == APEX_ARG_REG_D) {
            const RegVal *a = &r[APEX_ARG_REG_A], *b = &r[APEX_ARG_REG_B];
            if (!a->known || !b->known)
                continue;
            site.value = (a->value << 8) | b->value;
            site.value_addr = a->addr > b->addr ? a->addr : b->addr;
        } else {
            if (!r[reg].known)
                continue;
            site.value = r[reg].value;
            site.value_addr = r[reg].addr;
        }
        site.bank = s->bank;
        site.call_addr = call_addr;
        site.target_bank = tb;
        site.target_addr = ta;
        site.reg = reg;
        site.type_name = tn;
        if (add_site(s->out, &site) != 0)
            return -1;
    }
    return 0;
}

/* Report the typed fields of an inline payload starting at window offset pos. */
static int report_inline_args(const ArgScan *s, const InlineSignature *sig, size_t pos,
                              uint32_t call_addr, uint8_t tb, uint32_t ta) {
    for (size_t i = 0; i < sig->schema.count; i++) {
        const TableField *f = &sig->schema.items[i];
        if (f->kind == TABLE_BYTES_UNTIL || f->kind == TABLE_COUNTED_BYTES)
            return 0; /* variable length: later offsets unknown */
        size_t w = f->kind == TABLE_BYTE ? 1u : table_kind_is_far(f->kind) ? 3u : 2u;
        for (size_t n = 0; n < f->count; n++, pos += w) {
            if (pos + w > s->size)
                return 0;
            if (!f->type_name || (f->kind != TABLE_BYTE && f->kind != TABLE_WORD))
                continue;
            ApexArgSite site;
            memset(&site, 0, sizeof(site));
            site.bank = s->bank;
            site.value_addr = s->org + (uint32_t)pos;
            site.call_addr = call_addr;
            site.target_bank = tb;
            site.target_addr = ta;
            site.reg = APEX_ARG_INLINE;
            site.type_name = f->type_name;
            site.value = f->kind == TABLE_BYTE ? s->data[pos] : (uint32_t)read_be16(s->data + pos);
            if (add_site(s->out, &site) != 0)
                return -1;
        }
    }
    return 0;
}

/* Far target of a helper call: the first far_code field of its inline payload,
   if only fixed-width fields precede it. */
static int inline_far_target(const ArgScan *s, const InlineSignature *sig, size_t pos, uint8_t *fb,
                             uint32_t *fa) {
    for (size_t i = 0; i < sig->schema.count; i++) {
        const TableField *f = &sig->schema.items[i];
        if (f->kind == TABLE_FAR_CODE) {
            if (pos + 3u > s->size)
                return 0;
            *fa = read_be16(s->data + pos);
            *fb = s->data[pos + 2];
            return valid_far_code_target((uint16_t)*fa, *fb, s->p->rom.data, s->p->banks);
        }
        if (f->kind == TABLE_BYTES_UNTIL || f->kind == TABLE_COUNTED_BYTES)
            return 0;
        pos += f->count * (f->kind == TABLE_BYTE ? 1u : table_kind_is_far(f->kind) ? 3u : 2u);
    }
    return 0;
}

/* Decode one basic block from the code label at window offset start until the
   next label, a data/table range or the end of the flow. */
static int scan_block(const ArgScan *s, size_t start) {
    RegVal r[6];
    size_t pos = start;
    memset(r, 0, sizeof(r));
    while (pos < s->size) {
        uint32_t addr = s->org + (uint32_t)pos;
        if (pos != start && label_set_name_at(s->labels, addr))
            break;
        if (data_range_at(s->bank, addr, &s->p->data_ranges) ||
            table_def_at(s->bank, addr, &s->p->tables))
            break;
        char inst[64];
        Cpu6809InstrInfo info =
            cpu6809_disassemble_info(s->data + pos, s->size - pos, addr, inst, sizeof(inst));
        if (info.size == 0)
            break;
        const uint8_t *op = s->data + pos;
        int is_call = info.has_target && (info.flags & CPU6809_CALL);
        int is_tail = info.has_target && (info.flags & CPU6809_FLOW_STOP) &&
                      (strncmp(inst, "JMP", 3) == 0 || strncmp(inst, "BRA", 3) == 0 ||
                       strncmp(inst, "LBRA", 4) == 0);
        size_t next = pos + info.size;

        if (op[0] == 0x86u || op[0] == 0xc6u) { /* LDA / LDB #imm8 */
            RegVal *v = &r[op[0] == 0x86u ? APEX_ARG_REG_A : APEX_ARG_REG_B];
            v->known = 1;
            v->value = op[1];
            v->addr = addr;
        } else if (op[0] == 0x4fu || op[0] == 0x5fu) { /* CLRA / CLRB */
            RegVal *v = &r[op[0] == 0x4fu ? APEX_ARG_REG_A : APEX_ARG_REG_B];
            v->known = 1;
            v->value = 0;
            v->addr = addr;
        } else if (op[0] == 0xccu) { /* LDD #imm16 */
            r[APEX_ARG_REG_A] = (RegVal){1, op[1], addr};
            r[APEX_ARG_REG_B] = (RegVal){1, op[2], addr};
        } else if (op[0] == 0x8eu || op[0] == 0xceu) { /* LDX / LDU #imm16 */
            r[op[0] == 0x8eu ? APEX_ARG_REG_X : APEX_ARG_REG_U] =
                (RegVal){1, (uint32_t)read_be16(op + 1), addr};
        } else if (op[0] == 0x10u && info.size == 4 && op[1] == 0x8eu) { /* LDY #imm16 */
            r[APEX_ARG_REG_Y] = (RegVal){1, (uint32_t)read_be16(op + 2), addr};
        } else if (is_call || is_tail) {
            uint8_t tb = s->bank;
            uint32_t ta = info.target;
            int resolved = 1;
            if (ta >= APEX_SYSTEM_ORG)
                tb = 0xffu;
            else if (s->bank == 0xffu)
                resolved = 0; /* system code into the paged window: bank unknown */
            const InlineSignature *sig =
                is_call ? inline_signature_for(&s->p->inline_sigs, s->bank, info.target) : NULL;
            if (resolved && s->want) {
                if (s->want(s->want_ctx, tb, ta)) {
                    ApexCallSite c;
                    memset(&c, 0, sizeof(c));
                    c.bank = s->bank;
                    c.call_addr = addr;
                    c.target_bank = tb;
                    c.target_addr = ta;
                    c.is_tail = is_tail;
                    for (int k = 0; k < 6; k++) {
                        c.known[k] = r[k].known;
                        c.value[k] = r[k].value;
                        c.value_addr[k] = r[k].addr;
                    }
                    if (is_call && next < s->size) {
                        c.has_after = 1;
                        c.after = s->data[next];
                    }
                    if (add_call(s->calls, &c) != 0)
                        return -1;
                }
            } else if (resolved) {
                if (report_reg_args(s, r, addr, tb, ta) != 0)
                    return -1;
                uint8_t fb;
                uint32_t fa;
                if (sig && !config_doc_at(&s->p->docs, tb, ta) &&
                    inline_far_target(s, sig, next, &fb, &fa) &&
                    report_reg_args(s, r, addr, fb, fa) != 0)
                    return -1;
                if (sig && s->include_inline && report_inline_args(s, sig, next, addr, tb, ta) != 0)
                    return -1;
            }
            if (is_tail)
                break;
            if (sig) {
                int ok;
                next += inline_actual_length(sig, s->data, next, s->size, &ok);
                if (!ok || sig->flow_stop)
                    break;
            }
            reg_forget_all(r); /* the callee may change any register */
        } else {
            forget_written_regs(r, inst);
        }
        if (info.flags & CPU6809_FLOW_STOP)
            break;
        pos = next;
    }
    return 0;
}

static int site_cmp(const void *a, const void *b) {
    const ApexArgSite *x = a, *y = b;
    if (x->bank != y->bank)
        return x->bank < y->bank ? -1 : 1;
    if (x->value_addr != y->value_addr)
        return x->value_addr < y->value_addr ? -1 : 1;
    if (x->call_addr != y->call_addr)
        return x->call_addr < y->call_addr ? -1 : 1;
    return x->reg - y->reg;
}

int apex_collect_arg_sites(const ApexProject *p, int include_inline, ApexArgSites *out) {
    memset(out, 0, sizeof(*out));
    if (!p || !p->analysis_ready || p->rom.size < APEX_SYSTEM_SIZE)
        return 0;
    /* Without any @args declaration only inline fields can produce sites. */
    int any_args = 0;
    for (size_t i = 0; i < p->docs.count && !any_args; i++)
        any_args = p->docs.items[i].text && strstr(p->docs.items[i].text, "@args") != NULL;
    if (!any_args && !include_inline)
        return 0;

    for (size_t w = 0; w <= p->banks; w++) {
        ArgScan s;
        memset(&s, 0, sizeof(s));
        s.p = p;
        s.include_inline = include_inline;
        s.out = out;
        if (w < p->banks) {
            s.data = p->rom.data + w * APEX_BANK_SIZE;
            s.size = APEX_BANK_SIZE;
            s.org = APEX_PAGED_ORG;
            s.bank = bank_id_for_index(p->banks, (int)w);
            s.labels = &p->bank_labels[w];
        } else {
            s.data = p->rom.data + p->paged_size;
            s.size = APEX_SYSTEM_SIZE;
            s.org = APEX_SYSTEM_ORG;
            s.bank = 0xffu;
            s.labels = (LabelSet *)&p->system_labels;
        }
        for (size_t i = 0; i < s.labels->count; i++) {
            const Label *l = &s.labels->items[i];
            if (!l->is_code || l->addr < s.org || l->addr >= s.org + s.size)
                continue;
            if (scan_block(&s, l->addr - s.org) != 0) {
                apex_free_arg_sites(out);
                return -1;
            }
        }
    }
    if (out->count > 1) {
        /* Several labels at one address (e.g. vector entries) scan the same
           block more than once: keep each site once. */
        qsort(out->items, out->count, sizeof(*out->items), site_cmp);
        size_t w = 1;
        for (size_t i = 1; i < out->count; i++) {
            if (site_cmp(&out->items[i], &out->items[w - 1]) != 0)
                out->items[w++] = out->items[i];
        }
        out->count = w;
    }
    return 0;
}

int apex_collect_calls(const ApexProject *p, ApexCallWant want, void *ctx, ApexCallSites *out) {
    memset(out, 0, sizeof(*out));
    if (!p || !p->analysis_ready || p->rom.size < APEX_SYSTEM_SIZE || !want)
        return 0;
    for (size_t w = 0; w <= p->banks; w++) {
        ArgScan s;
        memset(&s, 0, sizeof(s));
        s.p = p;
        s.want = want;
        s.want_ctx = ctx;
        s.calls = out;
        if (w < p->banks) {
            s.data = p->rom.data + w * APEX_BANK_SIZE;
            s.size = APEX_BANK_SIZE;
            s.org = APEX_PAGED_ORG;
            s.bank = bank_id_for_index(p->banks, (int)w);
            s.labels = &p->bank_labels[w];
        } else {
            s.data = p->rom.data + p->paged_size;
            s.size = APEX_SYSTEM_SIZE;
            s.org = APEX_SYSTEM_ORG;
            s.bank = 0xffu;
            s.labels = (LabelSet *)&p->system_labels;
        }
        for (size_t i = 0; i < s.labels->count; i++) {
            const Label *l = &s.labels->items[i];
            if (!l->is_code || l->addr < s.org || l->addr >= s.org + s.size)
                continue;
            if (scan_block(&s, l->addr - s.org) != 0) {
                free(out->items);
                memset(out, 0, sizeof(*out));
                return -1;
            }
        }
    }
    /* several labels at one address scan a block twice: keep each call once */
    size_t w = 0;
    for (size_t i = 0; i < out->count; i++) {
        int dup = 0;
        for (size_t k = w; k-- > 0 && !dup;) {
            const ApexCallSite *a = &out->items[k];
            if (a->bank != out->items[i].bank)
                break;
            dup = a->call_addr == out->items[i].call_addr;
        }
        if (!dup)
            out->items[w++] = out->items[i];
    }
    out->count = w;
    return 0;
}

void apex_free_calls(ApexCallSites *calls) {
    free(calls->items);
    memset(calls, 0, sizeof(*calls));
}

void apex_free_arg_sites(ApexArgSites *sites) {
    if (!sites)
        return;
    free(sites->items);
    memset(sites, 0, sizeof(*sites));
}

const ApexArgSite *apex_arg_site_at(const ApexArgSites *sites, uint8_t bank, uint32_t addr,
                                    size_t *count) {
    size_t lo = 0, hi = sites ? sites->count : 0;
    *count = 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const ApexArgSite *m = &sites->items[mid];
        if (m->bank < bank || (m->bank == bank && m->value_addr < addr))
            lo = mid + 1;
        else
            hi = mid;
    }
    size_t n = 0;
    while (lo + n < sites->count && sites->items[lo + n].bank == bank &&
           sites->items[lo + n].value_addr == addr && sites->items[lo + n].reg != APEX_ARG_INLINE)
        n++;
    *count = n;
    return n ? &sites->items[lo] : NULL;
}

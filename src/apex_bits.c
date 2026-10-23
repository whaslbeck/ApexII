#include "apex_bits.h"

#include "apex.h"
#include "apex_analysis.h"
#include "apex_args.h"
#include "cpu6809.h"

#include <stdlib.h>
#include <string.h>

/* The bytes of a bank window. */
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

/* Is the routine at (bank, addr) a wrapper: [PSHS] [LDX n,S ; LDA ,X+ ;
   STX n,S] with LDB #code somewhere before a call / jump to the path? */
static int wrapper_shape(const ApexProject *p, uint8_t bank, uint32_t addr, uint8_t *code,
                         int *inline_arg, uint8_t *pb, uint32_t *pa) {
    const uint8_t *data;
    size_t size;
    uint32_t org;
    window(p, bank, &data, &size, &org);
    if (!data || addr < org || addr >= org + size)
        return 0;
    size_t pos = addr - org;
    int have_b = 0, ldx = 0, lda = 0;
    *inline_arg = 0;
    for (int n = 0; n < 8 && pos < size; n++) {
        char t[64];
        Cpu6809InstrInfo in =
            cpu6809_disassemble_info(data + pos, size - pos, org + (uint32_t)pos, t, sizeof(t));
        if (!in.size)
            return 0;
        if (!strncmp(t, "PSHS ", 5)) {
        } else if (!strncmp(t, "LDX ", 4) && strstr(t, ",S")) {
            ldx = 1;
        } else if (!strcmp(t, "LDA ,X+") && ldx) {
            lda = 1;
        } else if (!strncmp(t, "STX ", 4) && strstr(t, ",S") && lda) {
            *inline_arg = 1;
        } else if (data[pos] == 0xc6u) { /* LDB #imm */
            *code = data[pos + 1];
            have_b = 1;
        } else if (in.has_target &&
                   (!strncmp(t, "JSR", 3) || !strncmp(t, "BSR", 3) || !strncmp(t, "LBSR", 4) ||
                    !strncmp(t, "JMP", 3) || !strncmp(t, "BRA", 3) || !strncmp(t, "LBRA", 4))) {
            if (!have_b || (ldx && !*inline_arg))
                return 0;
            *pa = in.target;
            *pb = in.target >= APEX_SYSTEM_ORG ? 0xffu : bank;
            return 1;
        } else {
            return 0;
        }
        pos += in.size;
    }
    return 0;
}

/* A naming type for the wrapper's bit number: its typed first inline field,
   or `@args A:TYPE`. */
static const char *wrapper_type(const ApexProject *p, const ApexBitWrapper *w) {
    if (w->inline_arg) {
        const InlineSignature *sig = inline_signature_for(&p->inline_sigs, w->bank, w->addr);
        if (sig && sig->schema.count && sig->schema.items[0].type_name)
            return sig->schema.items[0].type_name;
    }
    const char *doc = config_doc_at(&p->docs, w->bank, w->addr);
    char buf[256];
    const char *types[6];
    if (doc && apex_parse_args_doc(doc, buf, sizeof(buf), types) && types[APEX_ARG_REG_A]) {
        const ConfigType *t = find_config_type(&p->config_types, types[APEX_ARG_REG_A]);
        return t ? t->name : NULL;
    }
    return NULL;
}

typedef struct {
    const ApexBitMap *m;
} WantCtx;

static int want_target(void *vctx, uint8_t bank, uint32_t addr) {
    const ApexBitMap *m = ((WantCtx *)vctx)->m;
    for (size_t i = 0; i < m->wrapper_count; i++)
        if (m->wrappers[i].bank == bank && m->wrappers[i].addr == addr)
            return 1;
    for (size_t i = 0; i < m->path_count; i++)
        if (m->paths[i].bank == bank && m->paths[i].addr == addr)
            return 1;
    return 0;
}

static int site_cmp(const void *a, const void *b) {
    const ApexBitSite *x = a, *y = b;
    if (x->matrix != y->matrix)
        return x->matrix < y->matrix ? -1 : 1;
    if (x->bit != y->bit)
        return x->bit < y->bit ? -1 : 1;
    if (x->path != y->path)
        return x->path - y->path;
    if (x->bank != y->bank)
        return x->bank < y->bank ? -1 : 1;
    return x->call_addr < y->call_addr ? -1 : x->call_addr > y->call_addr;
}

int apex_bit_map(const ApexProject *p, ApexBitMap *m) {
    memset(m, 0, sizeof(*m));
    if (!p || !p->analysis_ready || p->rom.size < APEX_SYSTEM_SIZE)
        return 0;
    /* 1. wrapper candidates in every bank */
    ApexBitWrapper *cand = NULL;
    uint8_t *cpb = NULL;
    uint32_t *cpa = NULL;
    size_t nc = 0, cc = 0;
    for (size_t w = 0; w <= p->banks; w++) {
        const LabelSet *ls = w < p->banks ? &p->bank_labels[w] : &p->system_labels;
        uint8_t bank = w < p->banks ? bank_id_for_index(p->banks, (int)w) : 0xffu;
        for (size_t i = 0; i < ls->count; i++) {
            const Label *l = &ls->items[i];
            ApexBitWrapper wr;
            uint8_t pb;
            uint32_t pa;
            memset(&wr, 0, sizeof(wr));
            if (!l->is_code || !wrapper_shape(p, bank, l->addr, &wr.code, &wr.inline_arg, &pb, &pa))
                continue;
            if (nc == cc) {
                cc = cc ? cc * 2 : 64;
                cand = realloc(cand, cc * sizeof(*cand));
                cpb = realloc(cpb, cc);
                cpa = realloc(cpa, cc * sizeof(*cpa));
            }
            wr.bank = bank;
            wr.addr = l->addr;
            cpb[nc] = pb;
            cpa[nc] = pa;
            cand[nc++] = wr;
        }
    }
    /* 2. bit paths: called by two or more inline wrappers */
    for (size_t i = 0; i < nc; i++) {
        if (!cand[i].inline_arg)
            continue;
        size_t k;
        for (k = 0; k < m->path_count; k++)
            if (m->paths[k].bank == cpb[i] && m->paths[k].addr == cpa[i])
                break;
        if (k == m->path_count) {
            m->paths = realloc(m->paths, (m->path_count + 1) * sizeof(*m->paths));
            m->paths[k] = (ApexBitPath){cpb[i], cpa[i], 0, 0};
            m->path_count++;
        }
        m->paths[k].wrappers++;
    }
    size_t keep = 0;
    for (size_t k = 0; k < m->path_count; k++)
        if (m->paths[k].wrappers >= 2)
            m->paths[keep++] = m->paths[k];
    m->path_count = keep;
    /* 3. the wrappers of those paths */
    for (size_t i = 0; i < nc; i++) {
        for (size_t k = 0; k < m->path_count; k++) {
            if (m->paths[k].bank != cpb[i] || m->paths[k].addr != cpa[i])
                continue;
            m->wrappers = realloc(m->wrappers, (m->wrapper_count + 1) * sizeof(*m->wrappers));
            cand[i].path = (int)k;
            cand[i].type_name = wrapper_type(p, &cand[i]);
            m->wrappers[m->wrapper_count++] = cand[i];
        }
    }
    free(cand);
    free(cpb);
    free(cpa);
    if (!m->path_count)
        return 0;
    /* 4. the calls of wrappers and paths with a certain bit number */
    ApexCallSites calls;
    WantCtx wc = {m};
    if (apex_collect_calls(p, want_target, &wc, &calls) != 0)
        return -1;
    size_t cap = 0;
    for (size_t i = 0; i < calls.count; i++) {
        const ApexCallSite *c = &calls.items[i];
        ApexBitSite s;
        memset(&s, 0, sizeof(s));
        s.bank = c->bank;
        s.call_addr = c->call_addr;
        s.via_bank = c->target_bank;
        s.via_addr = c->target_addr;
        int ok = 0;
        for (size_t k = 0; k < m->wrapper_count && !ok; k++) {
            const ApexBitWrapper *w = &m->wrappers[k];
            if (w->bank != c->target_bank || w->addr != c->target_addr)
                continue;
            /* calls from inside a wrapper / to the next wrapper are plumbing */
            s.matrix = w->code;
            s.path = w->path;
            s.type_name = w->type_name;
            if (w->inline_arg && c->has_after && !c->is_tail) {
                s.bit = c->after;
                s.value_addr = c->call_addr + 3u; /* JSR ext; BSR is 2 bytes */
                ok = 1;
            } else if (!w->inline_arg && c->known[APEX_ARG_REG_A]) {
                s.bit = c->value[APEX_ARG_REG_A];
                s.value_addr = c->value_addr[APEX_ARG_REG_A];
                ok = 1;
            }
        }
        for (size_t k = 0; k < m->path_count && !ok; k++) {
            const ApexBitPath *pa = &m->paths[k];
            if (pa->bank != c->target_bank || pa->addr != c->target_addr ||
                !c->known[APEX_ARG_REG_A] || !c->known[APEX_ARG_REG_B])
                continue;
            s.matrix = (uint8_t)c->value[APEX_ARG_REG_B];
            s.bit = c->value[APEX_ARG_REG_A];
            s.value_addr = c->value_addr[APEX_ARG_REG_A];
            s.path = (int)k;
            ok = 1;
        }
        if (!ok)
            continue;
        if (m->site_count == cap) {
            cap = cap ? cap * 2 : 256;
            m->sites = realloc(m->sites, cap * sizeof(*m->sites));
        }
        m->sites[m->site_count++] = s;
    }
    apex_free_calls(&calls);
    /* `@bits 0xNN:TYPE` in a bit routine's doc names the bits of matrix NN
       (for sites whose wrapper declares no type) */
    for (size_t k = 0; k < m->path_count; k++) {
        const char *doc = config_doc_at(&p->docs, m->paths[k].bank, m->paths[k].addr);
        for (const char *d = doc ? strstr(doc, "@bits") : NULL; d; d = strstr(d + 5, "@bits")) {
            const char *q = d + 5;
            while (*q == ' ')
                q++;
            char *end;
            unsigned long code = strtoul(q, &end, 0);
            if (end == q || *end != ':' || code > 0xff)
                continue;
            char tn[96];
            size_t n = 0;
            for (q = end + 1; *q && *q != ' ' && *q != ',' && *q != ';' && n + 1 < sizeof(tn); q++)
                tn[n++] = *q;
            tn[n] = '\0';
            const ConfigType *t = find_config_type(&p->config_types, tn);
            if (!t)
                continue;
            for (size_t i = 0; i < m->site_count; i++)
                if (m->sites[i].matrix == code && !m->sites[i].type_name)
                    m->sites[i].type_name = t->name;
            m->matrix_type[code] = t->name;
        }
    }
    /* the inline byte follows the call instruction: fix its address for BSR */
    for (size_t i = 0; i < m->site_count; i++) {
        ApexBitSite *s = &m->sites[i];
        const uint8_t *data;
        size_t size;
        uint32_t org;
        window(p, s->bank, &data, &size, &org);
        if (data && s->value_addr == s->call_addr + 3u && s->call_addr - org < size) {
            char t[64];
            Cpu6809InstrInfo in =
                cpu6809_disassemble_info(data + (s->call_addr - org), size - (s->call_addr - org),
                                         s->call_addr, t, sizeof(t));
            s->value_addr = s->call_addr + (uint32_t)in.size;
        }
    }
    /* a test: the code after most of its calls branches on the result */
    for (size_t k = 0; k < m->path_count; k++) {
        int n = 0, cond = 0;
        for (size_t i = 0; i < m->site_count; i++) {
            const ApexBitSite *s = &m->sites[i];
            if (s->path != (int)k)
                continue;
            const uint8_t *data;
            size_t size;
            uint32_t org;
            window(p, s->bank, &data, &size, &org);
            if (!data || s->call_addr - org >= size)
                continue;
            char t[64];
            Cpu6809InstrInfo in =
                cpu6809_disassemble_info(data + (s->call_addr - org), size - (s->call_addr - org),
                                         s->call_addr, t, sizeof(t));
            size_t after = s->call_addr - org + in.size;
            int inl = 0;
            for (size_t w = 0; w < m->wrapper_count; w++)
                if (m->wrappers[w].bank == s->via_bank && m->wrappers[w].addr == s->via_addr)
                    inl = m->wrappers[w].inline_arg;
            after += (size_t)inl;
            if (after + 1 >= size)
                continue;
            n++;
            uint8_t op = data[after];
            cond += (op >= 0x22u && op <= 0x2fu) ||
                    (op == 0x10u && data[after + 1] >= 0x22u && data[after + 1] <= 0x2fu);
        }
        m->paths[k].is_test = n >= 2 && cond * 2 > n;
    }
    qsort(m->sites, m->site_count, sizeof(*m->sites), site_cmp);
    return 0;
}

void apex_bit_map_free(ApexBitMap *m) {
    free(m->paths);
    free(m->wrappers);
    free(m->sites);
    memset(m, 0, sizeof(*m));
}

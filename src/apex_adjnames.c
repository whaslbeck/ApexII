#include "apex_adjnames.h"

#include "apex.h"
#include "apex_analysis.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The printable string at window offset `o` of a bank (NULL if it is not one). */
static char *bank_string(const uint8_t *bank, uint32_t a) {
    if (a < APEX_PAGED_ORG || a >= APEX_PAGED_ORG + APEX_BANK_SIZE)
        return NULL;
    size_t o = a - APEX_PAGED_ORG, n = 0;
    while (o + n < APEX_BANK_SIZE && bank[o + n]) {
        uint8_t c = bank[o + n];
        if (c < 0x20u || c > 0x7eu || n > 40)
            return NULL;
        n++;
    }
    if (!n || o + n >= APEX_BANK_SIZE)
        return NULL;
    char *s = xmalloc(n + 1);
    memcpy(s, bank + o, n);
    s[n] = '\0';
    return s;
}

static char *dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = xmalloc(n);
    memcpy(d, s, n);
    return d;
}

static void add_table(ApexAdjTables *t, uint8_t bank, uint32_t addr, char **names, size_t n) {
    t->tables = realloc(t->tables, (t->count + 1) * sizeof(*t->tables));
    t->tables[t->count++] = (ApexNameTable){bank, addr, n, names};
}

/* More than 30 % of the names end in a number: a test list (FLASHER 1). */
static int numbered(const ApexNameTable *x) {
    size_t n = 0;
    for (size_t i = 1; i < x->count; i++) {
        const char *s = x->names[i];
        size_t l = strlen(s);
        while (l && s[l - 1] == ')')
            l--;
        n += l && isdigit((unsigned char)s[l - 1]);
    }
    return n * 10 > (x->count - 1) * 3;
}

/* Share of names with a word typical of a setting. */
static double settings_score(const ApexNameTable *x) {
    static const char *const kw[] = {"PERCENT", "%",       "MEMORY", "DIFF",    "TIME",   "MAX",
                                     "LEVEL",   "LIMIT",   "START",  "BUY",     "EX.",    "EXTRA",
                                     "SAVE",    "SPEED",   "MODE",   "AWARD",   "LIT",    "HITS",
                                     "COUNT",   "ADJ",     "ENABLE", "DISABLE", "RANDOM", "SPECIAL",
                                     "REPLAY",  "JACKPOT", "LOCK",   "BALL"};
    size_t n = 0;
    for (size_t i = 1; i < x->count; i++) {
        char up[48];
        size_t k;
        for (k = 0; x->names[i][k] && k + 1 < sizeof(up); k++)
            up[k] = (char)toupper((unsigned char)x->names[i][k]);
        up[k] = '\0';
        for (size_t w = 0; w < sizeof(kw) / sizeof(kw[0]); w++) {
            if (strstr(up, kw[w])) {
                n++;
                break;
            }
        }
    }
    return x->count > 1 ? (double)n / (double)(x->count - 1) : 0.0;
}

int apex_adj_find_tables(const ApexProject *p, ApexAdjTables *t) {
    memset(t, 0, sizeof(*t));
    t->std_adj = t->std_aud = t->feat_adj = t->feat_aud = -1;
    if (!p || p->rom.size < APEX_SYSTEM_SIZE)
        return 0;
    for (size_t w = 0; w < p->banks; w++) {
        const uint8_t *bank = p->rom.data + w * APEX_BANK_SIZE;
        uint8_t id = bank_id_for_index(p->banks, (int)w);
        for (size_t k = 2; k + 2 <= APEX_BANK_SIZE;) {
            uint32_t a = ((uint32_t)bank[k] << 8) | bank[k + 1];
            char *s0 = bank_string(bank, a);
            int is_null = s0 && !strcmp(s0, "NULL");
            free(s0);
            if (!is_null) {
                k++;
                continue;
            }
            char **names = NULL;
            size_t n = 0, q = k;
            while (q + 2 <= APEX_BANK_SIZE) {
                char *s = bank_string(bank, ((uint32_t)bank[q] << 8) | bank[q + 1]);
                if (!s || (n && !strcmp(s, "NULL"))) {
                    free(s);
                    break;
                }
                names = realloc(names, (n + 1) * sizeof(*names));
                names[n++] = s;
                q += 2;
            }
            if (n >= 10) {
                add_table(t, id, APEX_PAGED_ORG + (uint32_t)k, names, n);
                k = q;
            } else {
                for (size_t i = 0; i < n; i++)
                    free(names[i]);
                free(names);
                k++;
            }
        }
    }
    /* the standard tables: English is first */
    for (size_t i = 0; i < t->count && t->std_adj < 0; i++) {
        if (t->tables[i].count > 1 && !strcmp(t->tables[i].names[1], "BALLS PER GAME")) {
            t->std_adj = (int)i;
            if (i + 1 < t->count && t->tables[i + 1].bank == t->tables[i].bank &&
                t->tables[i + 1].count > 1 && strstr(t->tables[i + 1].names[1], "EARNINGS"))
                t->std_aud = (int)i + 1;
        }
    }
    /* the feature tables: an (adjustments, audits) pair in one bank outside
       the standard tables' bank, the audits the longer list, not a numbered
       test list (FLASHER 1, LAMP 11 ...); the adjustment names that read most
       like settings win, the earlier (English) on a tie */
    double best = -1.0;
    for (size_t i = 0; i + 1 < t->count; i++) {
        const ApexNameTable *x = t->tables + i;
        if (x[1].bank != x->bank || x->count >= x[1].count)
            continue;
        if (t->std_adj >= 0 && x->bank == t->tables[t->std_adj].bank)
            continue;
        if (numbered(x) || numbered(x + 1))
            continue;
        double sc = settings_score(x);
        if (sc > best) {
            best = sc;
            t->feat_adj = (int)i;
            t->feat_aud = (int)i + 1;
        }
    }
    if (best < 0.3)
        t->feat_adj = t->feat_aud = -1; /* nothing that looks like settings */
    return 0;
}

void apex_adj_tables_free(ApexAdjTables *t) {
    for (size_t i = 0; i < t->count; i++) {
        for (size_t k = 0; k < t->tables[i].count; k++)
            free(t->tables[i].names[k]);
        free(t->tables[i].names);
    }
    free(t->tables);
    memset(t, 0, sizeof(*t));
}

int apex_adj_table_at(const ApexAdjTables *t, uint8_t bank, uint32_t addr) {
    for (size_t i = 0; i < t->count; i++)
        if (t->tables[i].bank == bank && t->tables[i].addr == addr)
            return (int)i;
    return -1;
}

/* NAME from a display text: letters / digits, the rest '_' (collapsed). */
static void ident(const char *s, char *out, size_t n) {
    size_t o = 0;
    int us = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c)) {
            if (o == 0 && isdigit(c))
                out[o++] = 'N', out[o++] = '_';
            out[o++] = (char)toupper(c);
            us = 0;
        } else if (c == '%' && o + 4 < n) {
            if (o && !us)
                out[o++] = '_';
            memcpy(out + o, "PCT", 3);
            o += 3;
            us = 0;
        } else if (o && !us) {
            out[o++] = '_';
            us = 1;
        }
    }
    while (o && out[o - 1] == '_')
        o--;
    out[o] = '\0';
    if (!o)
        snprintf(out, n, "UNNAMED");
}

typedef struct {
    char *buf;
    size_t n, cap;
    char **used;
    size_t used_count;
} Values;

static void val_add(Values *v, unsigned value, const char *text) {
    char name[64], uniq[72];
    ident(text, name, sizeof(name));
    snprintf(uniq, sizeof(uniq), "%s", name);
    for (int k = 2;; k++) {
        int dup = 0;
        for (size_t i = 0; i < v->used_count && !dup; i++)
            dup = !strcmp(v->used[i], uniq);
        if (!dup)
            break;
        snprintf(uniq, sizeof(uniq), "%s_%d", name, k);
    }
    v->used = realloc(v->used, (v->used_count + 1) * sizeof(*v->used));
    v->used[v->used_count++] = dup(uniq);
    char item[96];
    int len = snprintf(item, sizeof(item), "%s0x%02x:%s", v->n ? ", " : "", value, uniq);
    if (v->n + (size_t)len + 1 > v->cap) {
        v->cap = (v->n + (size_t)len + 1) * 2;
        v->buf = realloc(v->buf, v->cap);
    }
    memcpy(v->buf + v->n, item, (size_t)len + 1);
    v->n += (size_t)len;
}

char *apex_adj_type_values(const ApexAdjTables *t, int adjustments, int table) {
    Values v;
    memset(&v, 0, sizeof(v));
    if (adjustments) {
        if (t->feat_adj >= 0)
            for (size_t i = 1; i < t->tables[t->feat_adj].count && i < 0x80; i++)
                val_add(&v, (unsigned)i, t->tables[t->feat_adj].names[i]);
        if (t->std_adj >= 0)
            for (size_t i = 1; i < t->tables[t->std_adj].count && i < 0x80; i++)
                val_add(&v, 0x80u + (unsigned)i, t->tables[t->std_adj].names[i]);
    } else if (table >= 0 && (size_t)table < t->count) {
        for (size_t i = 1; i < t->tables[table].count && i < 0x100; i++)
            val_add(&v, (unsigned)i, t->tables[table].names[i]);
    }
    for (size_t i = 0; i < v.used_count; i++)
        free(v.used[i]);
    free(v.used);
    return v.buf ? v.buf : dup("");
}

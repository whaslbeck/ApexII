#include "apex_wpc_hooks.h"

#include <stdlib.h>
#include <string.h>

/* A helper must be followed by a slot address at this many call sites before
   its calls are trusted (a single JSR followed by a slot-looking word is a
   coincidence). */
#define HOOK_HELPER_MIN_SITES 3u
/* Shorter runs of far pointers at 0x8001 are not a hook table. */
#define HOOK_TABLE_MIN_SLOTS 8u
#define HOOK_TABLE_MAX_SLOTS 1024u

typedef struct {
    uint32_t target;
    unsigned sites;
} HelperCount;

/* One CPU window to scan: the paged banks at 0x4000 and the system bank. */
typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t org;
    uint8_t bank;
} HookWindow;

static int hook_slot_index(const ApexWpcHooks *h, uint32_t w, size_t *slot) {
    uint32_t end = APEX_WPC_HOOK_TABLE_ADDR + 3u * (uint32_t)h->count;
    if (w < APEX_WPC_HOOK_TABLE_ADDR || w >= end || (w - APEX_WPC_HOOK_TABLE_ADDR) % 3u != 0) {
        return 0;
    }
    *slot = (w - APEX_WPC_HOOK_TABLE_ADDR) / 3u;
    return 1;
}

static const uint8_t *hook_target_bytes(const ApexProject *p, uint8_t bank, uint32_t addr,
                                        size_t *avail) {
    if (bank == 0xffu) {
        if (addr < APEX_SYSTEM_ORG || addr > 0xffffu) {
            return NULL;
        }
        *avail = 0x10000u - addr;
        return p->rom.data + p->paged_size + (addr - APEX_SYSTEM_ORG);
    }
    int bi = bank_index_for_id(p->rom.data, p->banks, bank);
    if (bi < 0 || addr < APEX_PAGED_ORG || addr >= APEX_SYSTEM_ORG) {
        return NULL;
    }
    *avail = APEX_SYSTEM_ORG - addr;
    return p->rom.data + (size_t)bi * APEX_BANK_SIZE + (addr - APEX_PAGED_ORG);
}

static int caller_cmp(const void *a, const void *b) {
    const ApexWpcHookCaller *x = a, *y = b;
    if (x->slot != y->slot)
        return x->slot < y->slot ? -1 : 1;
    if (x->bank != y->bank)
        return x->bank < y->bank ? -1 : 1;
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1;
    return 0;
}

static int add_caller(ApexWpcHooks *h, size_t *cap, uint8_t bank, uint32_t addr, int kind,
                      size_t slot) {
    if (h->caller_count == *cap) {
        size_t ncap = *cap ? *cap * 2u : 256u;
        ApexWpcHookCaller *n = realloc(h->callers, ncap * sizeof(*n));
        if (!n)
            return -1;
        h->callers = n;
        *cap = ncap;
    }
    ApexWpcHookCaller *c = &h->callers[h->caller_count++];
    c->bank = bank;
    c->addr = addr;
    c->kind = kind;
    c->slot = slot;
    return 0;
}

int apex_wpc_detect_hooks(const ApexProject *p, ApexWpcHooks *out) {
    memset(out, 0, sizeof(*out));
    if (!p || p->rom.size < APEX_SYSTEM_SIZE || p->paged_size + APEX_SYSTEM_SIZE > p->rom.size) {
        return 0;
    }
    const uint8_t *sys = p->rom.data + p->paged_size;

    /* 1. The table: the run of valid far pointers starting at 0x8001. */
    size_t n = 0;
    while (n < HOOK_TABLE_MAX_SLOTS) {
        size_t o = 1u + 3u * n;
        if (o + 3u > APEX_SYSTEM_SIZE)
            break;
        uint16_t addr = (uint16_t)((sys[o] << 8) | sys[o + 1]);
        if (!valid_far_code_target(addr, sys[o + 2], p->rom.data, p->banks))
            break;
        n++;
    }
    if (n < HOOK_TABLE_MIN_SLOTS) {
        return 0;
    }
    out->items = calloc(n, sizeof(*out->items));
    if (!out->items)
        return -1;
    out->count = n;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *e = sys + 1u + 3u * i;
        ApexWpcHook *hk = &out->items[i];
        size_t avail = 0;
        hk->slot_addr = APEX_WPC_HOOK_TABLE_ADDR + 3u * (uint32_t)i;
        hk->target_addr = (uint32_t)((e[0] << 8) | e[1]);
        hk->target_bank = e[2];
        const uint8_t *t = hook_target_bytes(p, hk->target_bank, hk->target_addr, &avail);
        hk->is_stub = t && avail > 0 && t[0] == 0x39u; /* RTS */
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            if (out->items[j].target_bank == out->items[i].target_bank &&
                out->items[j].target_addr == out->items[i].target_addr)
                out->items[i].shared++;
        }
    }

    /* 2. Windows to scan for callers. */
    size_t nwin = p->banks + 1u;
    HookWindow *win = calloc(nwin, sizeof(*win));
    if (!win)
        return -1;
    for (size_t b = 0; b < p->banks; b++) {
        win[b].data = p->rom.data + b * APEX_BANK_SIZE;
        win[b].size = APEX_BANK_SIZE;
        win[b].org = APEX_PAGED_ORG;
        win[b].bank = bank_id_for_index(p->banks, (int)b);
    }
    win[p->banks].data = sys;
    win[p->banks].size = APEX_SYSTEM_SIZE;
    win[p->banks].org = APEX_SYSTEM_ORG;
    win[p->banks].bank = 0xffu;
    uint32_t table_end = APEX_WPC_HOOK_TABLE_ADDR + 3u * (uint32_t)n;

    /* 3. Helpers: system-bank JSR/JMP targets followed by a slot address. */
    HelperCount counts[64];
    size_t ncounts = 0;
    for (size_t w = 0; w < nwin; w++) {
        const HookWindow *hw = &win[w];
        for (size_t k = 0; k + 5u <= hw->size; k++) {
            uint8_t op = hw->data[k];
            size_t slot;
            if (op != 0xbdu && op != 0x7eu)
                continue;
            uint32_t at = hw->org + (uint32_t)k;
            if (hw->bank == 0xffu && at < table_end)
                continue;
            uint32_t t = (uint32_t)((hw->data[k + 1] << 8) | hw->data[k + 2]);
            uint32_t s = (uint32_t)((hw->data[k + 3] << 8) | hw->data[k + 4]);
            if (t < APEX_SYSTEM_ORG || !hook_slot_index(out, s, &slot))
                continue;
            size_t c;
            for (c = 0; c < ncounts && counts[c].target != t; c++)
                ;
            if (c == ncounts) {
                if (ncounts == sizeof(counts) / sizeof(counts[0]))
                    continue;
                counts[ncounts].target = t;
                counts[ncounts].sites = 0;
                ncounts++;
            }
            counts[c].sites++;
        }
    }
    /* The real helper dominates (~100 sites); other inline-word routines only
       hit a slot-looking word now and then (<10 sites), so keep the candidates
       within half of the best. */
    unsigned best = 0;
    for (size_t c = 0; c < ncounts; c++) {
        if (counts[c].sites > best)
            best = counts[c].sites;
    }
    for (size_t c = 0; c < ncounts; c++) {
        if (counts[c].sites >= HOOK_HELPER_MIN_SITES && counts[c].sites * 2u >= best &&
            out->helper_count < sizeof(out->helpers) / sizeof(out->helpers[0]))
            out->helpers[out->helper_count++] = counts[c].target;
    }

    /* 4. Callers: helper calls with an inline slot, and LDX #slot. */
    size_t cap = 0;
    for (size_t w = 0; w < nwin; w++) {
        const HookWindow *hw = &win[w];
        for (size_t k = 0; k + 3u <= hw->size; k++) {
            uint8_t op = hw->data[k];
            uint32_t at = hw->org + (uint32_t)k;
            size_t slot;
            if (hw->bank == 0xffu && at < table_end)
                continue;
            if ((op == 0xbdu || op == 0x7eu) && k + 5u <= hw->size) {
                uint32_t t = (uint32_t)((hw->data[k + 1] << 8) | hw->data[k + 2]);
                uint32_t s = (uint32_t)((hw->data[k + 3] << 8) | hw->data[k + 4]);
                size_t h;
                for (h = 0; h < out->helper_count && out->helpers[h] != t; h++)
                    ;
                if (h < out->helper_count && hook_slot_index(out, s, &slot)) {
                    if (add_caller(out, &cap, hw->bank, at, APEX_HOOK_CALL_HELPER, slot) != 0)
                        goto oom;
                    k += 4u;
                }
            } else if (op == 0x8eu) {
                uint32_t s = (uint32_t)((hw->data[k + 1] << 8) | hw->data[k + 2]);
                if (hook_slot_index(out, s, &slot)) {
                    if (add_caller(out, &cap, hw->bank, at, APEX_HOOK_CALL_LDX, slot) != 0)
                        goto oom;
                    k += 2u;
                }
            }
        }
    }
    free(win);
    if (out->caller_count > 1)
        qsort(out->callers, out->caller_count, sizeof(*out->callers), caller_cmp);
    for (size_t c = 0; c < out->caller_count; c++)
        out->items[out->callers[c].slot].caller_count++;
    return 0;

oom:
    free(win);
    apex_wpc_hooks_free(out);
    return -1;
}

void apex_wpc_hooks_free(ApexWpcHooks *hooks) {
    if (!hooks)
        return;
    free(hooks->items);
    free(hooks->callers);
    memset(hooks, 0, sizeof(*hooks));
}

const char *apex_wpc_routine_at(const ApexProject *p, uint8_t bank, uint32_t addr,
                                uint32_t *start) {
    const LabelSet *set = NULL;
    if (bank == 0xffu) {
        set = &p->system_labels;
    } else {
        int bi = bank_index_for_id(p->rom.data, p->banks, bank);
        if (bi < 0 || !p->bank_labels)
            return NULL;
        set = &p->bank_labels[bi];
    }
    const Label *best = NULL;
    for (size_t i = 0; i < set->count; i++) {
        const Label *l = &set->items[i];
        if (l->is_code && l->addr <= addr && (!best || l->addr > best->addr))
            best = l;
    }
    if (!best)
        return NULL;
    if (start)
        *start = best->addr;
    /* Several labels can share the address: use the one the listing shows. */
    const char *shown = label_set_name_at((LabelSet *)set, best->addr);
    return shown ? shown : best->name;
}

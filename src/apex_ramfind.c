#include "apex_ramfind.h"

#include "apex.h"
#include "apex_project.h"
#include "apex_render.h"
#include "cpu6809.h"

#include <stdlib.h>
#include <string.h>

void apex_ram_map_mark(ApexRamMap *map, uint32_t addr, uint32_t len, int flag) {
    for (uint32_t a = addr; a < addr + len && a < APEX_RAM_SIZE; a++)
        map->use[a] |= (uint8_t)flag;
}

/* 16-bit register operations access two bytes. */
static int wide_op(const char *mn) {
    static const char *const w[] = {"LDD",  "STD",  "LDX",  "STX",  "LDY",  "STY",
                                    "LDU",  "STU",  "LDS",  "STS",  "ADDD", "SUBD",
                                    "CMPD", "CMPX", "CMPY", "CMPU", "CMPS", NULL};
    for (int i = 0; w[i]; i++)
        if (!strcmp(mn, w[i]))
            return 1;
    return 0;
}

static int pointer_op(const char *mn) {
    return !strcmp(mn, "LDX") || !strcmp(mn, "LDY") || !strcmp(mn, "LDU") || !strcmp(mn, "LDD") ||
           !strcmp(mn, "CMPX") || !strcmp(mn, "CMPY") || !strcmp(mn, "CMPU");
}

void apex_ram_map_static(const ApexProject *p, const ApexRenderedDocument *d, ApexRamMap *map,
                         unsigned pointer_window, unsigned stack_bytes) {
    if (!pointer_window)
        pointer_window = 32;
    if (!stack_bytes)
        stack_bytes = 256;
    for (size_t i = 0; i < p->symbols.count; i++) {
        const ConfigSymbol *s = &p->symbols.items[i];
        if (s->value < APEX_RAM_SIZE)
            apex_ram_map_mark(map, s->value, s->length ? s->length : 1u, APEX_RAM_SYMBOL);
    }
    if (!d)
        return;
    uint32_t *bases = NULL;
    size_t nb = 0, cb = 0;
    for (size_t i = 0; i < d->line_count; i++) {
        const ApexRenderedLine *l = &d->lines[i];
        if (l->kind != APEX_RENDER_LINE_INSTRUCTION || !l->has_location ||
            l->rom_addr >= p->rom.size)
            continue;
        char txt[96], mn[16];
        Cpu6809InstrInfo in = cpu6809_disassemble_info(
            p->rom.data + l->rom_addr, p->rom.size - l->rom_addr, l->cpu_addr, txt, sizeof(txt));
        if (!in.size)
            continue;
        size_t k = 0;
        while (txt[k] && txt[k] != ' ' && k + 1 < sizeof(mn)) {
            mn[k] = txt[k];
            k++;
        }
        mn[k] = '\0';
        const char *imm = strchr(txt, '#');
        const char *dp = strstr(txt, "<0x");
        uint32_t width = wide_op(mn) ? 2u : 1u;
        if (imm && in.has_addr_ref) { /* a 16-bit immediate */
            uint32_t v = in.addr_ref;
            if (!strcmp(mn, "LDS") && v <= APEX_RAM_SIZE) { /* every stack set up */
                uint32_t lo = v > stack_bytes ? v - stack_bytes : 0;
                apex_ram_map_mark(map, lo, v - lo, APEX_RAM_STACK);
                if (v > map->stack_top)
                    map->stack_top = v;
            } else if (pointer_op(mn) && v < APEX_RAM_SIZE && v >= 0x0010u) {
                if (nb == cb) {
                    cb = cb ? cb * 2 : 64;
                    bases = realloc(bases, cb * sizeof(*bases));
                }
                bases[nb++] = v;
            }
        } else if (in.has_addr_ref && !imm) { /* extended */
            if (in.addr_ref < APEX_RAM_SIZE)
                apex_ram_map_mark(map, in.addr_ref, width, APEX_RAM_STATIC);
        } else if (dp) { /* direct page (DP = 0 on WPC) */
            uint32_t a = (uint32_t)strtoul(dp + 1, NULL, 16);
            apex_ram_map_mark(map, a, width, APEX_RAM_STATIC);
        }
    }
    /* pointer bases: a window after each, up to the next statically used byte */
    for (size_t i = 0; i < nb; i++) {
        uint32_t a = bases[i];
        apex_ram_map_mark(map, a, 1, APEX_RAM_POINTER);
        for (uint32_t k = 1; k < pointer_window && a + k < APEX_RAM_SIZE; k++) {
            if (map->use[a + k] & (APEX_RAM_STATIC | APEX_RAM_SYMBOL))
                break;
            map->use[a + k] |= APEX_RAM_POINTER;
        }
    }
    free(bases);
}

void apex_ram_map_sample(ApexRamMap *map, const uint8_t *ram) {
    for (uint32_t a = 0; a < APEX_RAM_SIZE; a++)
        if (ram[a])
            map->use[a] |= APEX_RAM_DYNAMIC;
    map->samples++;
}

static int cmp_run(const void *a, const void *b) {
    const ApexRamRun *x = a, *y = b;
    if (x->nvram != y->nvram)
        return x->nvram - y->nvram;
    if (x->len != y->len)
        return x->len < y->len ? 1 : -1;
    return x->addr < y->addr ? -1 : 1;
}

void apex_ram_map_runs(ApexRamMap *map, unsigned min_len) {
    free(map->runs);
    map->runs = NULL;
    map->count = 0;
    size_t cap = 0;
    if (!min_len)
        min_len = 1;
    /* 0x0000-0x000f: scratch registers of many games; never offered */
    for (uint32_t a = 0x10; a < APEX_RAM_SIZE;) {
        if (map->use[a]) {
            a++;
            continue;
        }
        uint32_t s = a;
        while (a < APEX_RAM_SIZE && !map->use[a])
            a++;
        if (s < APEX_RAM_NVRAM_START && a > APEX_RAM_NVRAM_START) { /* split at the NVRAM */
            a = APEX_RAM_NVRAM_START;
        }
        if (a - s < min_len)
            continue;
        if (map->count == cap) {
            cap = cap ? cap * 2 : 32;
            map->runs = realloc(map->runs, cap * sizeof(*map->runs));
        }
        map->runs[map->count++] =
            (ApexRamRun){s, a - s, map->samples > 0, s >= APEX_RAM_NVRAM_START};
    }
    qsort(map->runs, map->count, sizeof(*map->runs), cmp_run);
}

void apex_ram_map_free(ApexRamMap *map) {
    free(map->runs);
    map->runs = NULL;
    map->count = 0;
}

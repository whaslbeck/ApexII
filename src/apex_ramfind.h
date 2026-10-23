#ifndef APEX_RAMFIND_H
#define APEX_RAMFIND_H

/* Free-RAM finder: which bytes of the WPC RAM (0x0000-0x1FFF) the game uses,
   and the runs it seems to leave alone -- candidates for a patch's `ram` op.

   Static evidence (from the analysed code): every direct / extended operand
   that lands in RAM (16-bit accesses count two bytes), [symbols] with their
   length, pointer bases loaded as immediates (LDX #0x0123 ...: a window after
   them is taken as used, since indexed accesses are not visible), the stack
   below every LDS #top.  Dynamic evidence (optional): RAM
   snapshots from a running game; a byte that is ever non-zero is used (WPC
   clears its RAM at boot).  Static-only candidates are unverified -- computed
   and indexed accesses are invisible to the analysis. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ApexProject;

#define APEX_RAM_SIZE 0x2000u
/* Usually the battery-backed (write-protected) part: audits and settings,
   reached through tables, and zero until counted -- risky candidates. */
#define APEX_RAM_NVRAM_START 0x1800u

enum {
    APEX_RAM_STATIC = 1,   /* a code operand */
    APEX_RAM_POINTER = 2,  /* window after an immediate pointer base */
    APEX_RAM_SYMBOL = 4,   /* [symbols] */
    APEX_RAM_STACK = 8,    /* below the initial stack pointer */
    APEX_RAM_DYNAMIC = 16, /* non-zero in a snapshot */
    APEX_RAM_PATCH = 32    /* claimed by a patch (ram op) */
};

typedef struct {
    uint32_t addr, len;
    int verified; /* zero in every snapshot (dynamic data was given) */
    int nvram;    /* starts at or above APEX_RAM_NVRAM_START */
} ApexRamRun;

typedef struct {
    uint8_t use[APEX_RAM_SIZE]; /* APEX_RAM_* flags per byte */
    uint32_t stack_top;         /* highest LDS #imm, 0 = none (each one reserves
                                   stack_bytes below it) */
    unsigned samples;           /* snapshots merged */
    ApexRamRun *runs;
    size_t count;
} ApexRamMap;

struct ApexRenderedDocument;

/* Static evidence from the analysed project and its rendered listing (the
   instruction lines say what is code; the bytes come from project->rom).
   pointer_window: bytes taken as used after a pointer base (0 = 32);
   stack_bytes: bytes below the stack top (0 = 256). */
void apex_ram_map_static(const struct ApexProject *project, const struct ApexRenderedDocument *doc,
                         ApexRamMap *map, unsigned pointer_window, unsigned stack_bytes);
/* Merge one RAM snapshot (APEX_RAM_SIZE bytes from address 0). */
void apex_ram_map_sample(ApexRamMap *map, const uint8_t *ram);
/* Mark [addr, addr+len) with flag (e.g. APEX_RAM_PATCH). */
void apex_ram_map_mark(ApexRamMap *map, uint32_t addr, uint32_t len, int flag);
/* Collect the free runs of at least min_len bytes: below the NVRAM area
   first, then largest first. */
void apex_ram_map_runs(ApexRamMap *map, unsigned min_len);
void apex_ram_map_free(ApexRamMap *map);

#ifdef __cplusplus
}
#endif

#endif /* APEX_RAMFIND_H */

#ifndef APEX_BITS_H
#define APEX_BITS_H

/* Bit map: where the game sets, clears, toggles and tests its lamps and flags.
 *
 * The WPC OS keeps lamps and game flags in bit matrices, all handled by a few
 * "bit path" routines (on / off / toggle / test ...) that take the bit number
 * in A and the matrix in B.  Small wrappers load B with a matrix code and call
 * a path, with the bit number either in A or as an inline byte after the call
 * (PSHS ; LDX n,S ; LDA ,X+ ; STX n,S ; LDB #code ; JSR path).
 *
 * Detection needs no configuration: a routine called by at least two such
 * inline wrappers is a bit path; every wrapper of a bit path (inline or A) is
 * found by its shape.  Each call of a wrapper or a path whose bit number is
 * certain (the inline byte, or an immediate load of A in the same block)
 * becomes a site.  The bit's name comes from a type the analysis already
 * declares: the wrapper's typed [inline] field or an `@args A:TYPE` doc, else
 * the type a bit routine's doc gives the whole matrix: `@bits 0x48:TYPE_FLAG`.
 */

#include "apex_project.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t bank;
    uint32_t addr;
    int path;     /* index into ApexBitMap.paths */
    uint8_t code; /* B: the matrix */
    int inline_arg;
    const char *type_name; /* naming type of the bit number, or NULL */
} ApexBitWrapper;

typedef struct {
    uint8_t bank;
    uint32_t addr;
    int wrappers; /* how many wrappers call it */
    int is_test;  /* most of its sites are followed by a conditional branch */
} ApexBitPath;

typedef struct {
    uint8_t matrix; /* B code */
    uint32_t bit;   /* bit number (A / inline byte) */
    int path;       /* the operation: index into paths */
    uint8_t bank;   /* the calling instruction */
    uint32_t call_addr;
    uint8_t via_bank; /* the wrapper / path it calls */
    uint32_t via_addr;
    uint32_t value_addr;   /* where the bit number is (load or inline byte) */
    const char *type_name; /* see ApexBitWrapper */
} ApexBitSite;

typedef struct {
    ApexBitPath *paths;
    size_t path_count;
    ApexBitWrapper *wrappers;
    size_t wrapper_count;
    ApexBitSite *sites; /* sorted by matrix, bit, path, bank, call_addr */
    size_t site_count;
    const char *matrix_type[256]; /* `@bits 0xNN:TYPE` of a bit routine's doc */
} ApexBitMap;

int apex_bit_map(const ApexProject *project, ApexBitMap *out);
void apex_bit_map_free(ApexBitMap *map);

#endif /* APEX_BITS_H */

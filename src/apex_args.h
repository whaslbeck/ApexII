#ifndef APEX_ARGS_H
#define APEX_ARGS_H

/* Typed call-site arguments.
 *
 * A routine declares which registers carry a typed argument with an `@args`
 * line in its [docs] entry:
 *
 *     Bff_A9123 = "Tests a switch. @args A:TYPE_SWITCH"
 *     Bff_A9200 = "@args B:TYPE_LAMP, X:TYPE_SCRIPT_ID"
 *
 * (registers A, B, D, X, Y, U; the type is a [types] name).  Each call (or
 * tail jump) to such a routine is traced back through its basic block to the
 * immediate load of the register (LDA/LDB/LDD/LDX/LDY/LDU #imm, CLRA, CLRB),
 * so `LDA #0x2c / JSR Switch_Test` can render as `LDA #TYPE_SWITCH_LEFT_RAMP`.
 * A call through a far-call helper (an [inline] signature with a far_code
 * field) counts as a call of its far target.  The trace stops at any label
 * (branch target), any other call and any instruction that changes the
 * register, so a site is only reported when the value is certain.
 *
 * Typed [inline] fields (named [types] in an [inline] signature) are reported
 * as sites too, so one list answers "where is switch / lamp / adjustment N
 * used", whichever way the value is passed.
 */

#include "apex_project.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    APEX_ARG_REG_A = 0,
    APEX_ARG_REG_B,
    APEX_ARG_REG_D,
    APEX_ARG_REG_X,
    APEX_ARG_REG_Y,
    APEX_ARG_REG_U,
    APEX_ARG_INLINE /* an [inline] field after the call */
} ApexArgReg;

typedef struct {
    uint8_t bank;          /* bank of the call site (0xff = system) */
    uint32_t value_addr;   /* the load instruction / the inline field */
    uint32_t call_addr;    /* the JSR/BSR/JMP */
    uint8_t target_bank;   /* routine that takes the argument */
    uint32_t target_addr;  /* (the far target for a helper call) */
    int reg;               /* ApexArgReg */
    const char *type_name; /* borrowed from the project's [types] */
    uint32_t value;
} ApexArgSite;

typedef struct {
    ApexArgSite *items; /* sorted by bank, value_addr */
    size_t count;
    size_t cap;
} ApexArgSites;

/* Collect every typed argument site of the analysed project.  include_inline
   adds the typed [inline] fields.  Returns 0 on success. */
int apex_collect_arg_sites(const ApexProject *project, int include_inline, ApexArgSites *out);
void apex_free_arg_sites(ApexArgSites *sites);

/* First register-argument site whose load instruction is at (bank, addr), or
   NULL.  `sites` must come from apex_collect_arg_sites. */
const ApexArgSite *apex_arg_site_at(const ApexArgSites *sites, uint8_t bank, uint32_t addr,
                                    size_t *count);

/* Parse the `@args` declaration of a doc text into per-register type names
   (types[reg] = NULL when the register is not declared; the strings are
   written into buf).  Returns the number of declared registers. */
int apex_parse_args_doc(const char *doc, char *buf, size_t bufsz, const char *types[6]);

const char *apex_arg_reg_name(int reg);

/* Calls (and tail jumps) with the register values the block trace knows at
   the call -- the machinery of the typed arguments, for other analyses (the
   bit map).  known/value/value_addr per ApexArgReg A..U (D is A:B); `after`
   is the byte right after a call (its first inline byte, if it has one). */
typedef struct {
    uint8_t bank;
    uint32_t call_addr;
    uint8_t target_bank;
    uint32_t target_addr;
    int is_tail;
    int known[6];
    uint32_t value[6];
    uint32_t value_addr[6];
    int has_after;
    uint8_t after;
} ApexCallSite;

typedef struct {
    ApexCallSite *items;
    size_t count;
    size_t cap;
} ApexCallSites;

/* Which call targets to collect. */
typedef int (*ApexCallWant)(void *ctx, uint8_t bank, uint32_t addr);

int apex_collect_calls(const ApexProject *project, ApexCallWant want, void *ctx,
                       ApexCallSites *out);
void apex_free_calls(ApexCallSites *calls);

#endif /* APEX_ARGS_H */

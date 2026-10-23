#ifndef APEX_WPC_HOOKS_H
#define APEX_WPC_HOOKS_H

/* Detection of the WPC game-hook table.
 *
 * The system bank starts with its bank byte at 0x8000, followed at 0x8001 by a
 * table of 3-byte far-code descriptors (addr_hi, addr_lo, bank).  The OS never
 * calls game code directly: at every point where a game may act (ball start,
 * end of ball, replay, match, ...) it calls a far-call helper with the slot
 * address as an inline word,
 *
 *     JSR FarCallHelper
 *     .DW 0x805b          ; -> slot -> game routine
 *
 * or loads the slot with LDX #slot.  The table length depends on the OS
 * version (143..175 slots seen), the slot order is fixed per OS version.
 * Slots a game does not use point at a shared RTS.  Redirecting a slot is the
 * least invasive way to hook game behaviour, and the OS call site of a slot
 * tells what the hook means.
 *
 * Detection reads raw ROM bytes only (no config needed): the table is the run
 * of valid far pointers at 0x8001, the helpers are the JSR/JMP targets that are
 * followed by a slot address at several places.
 */

#include "apex_project.h"

#include <stddef.h>
#include <stdint.h>

#define APEX_WPC_HOOK_TABLE_ADDR 0x8001u

typedef enum {
    APEX_HOOK_CALL_HELPER = 0, /* JSR/JMP helper; .DW slot */
    APEX_HOOK_CALL_LDX = 1     /* LDX #slot */
} ApexHookCallKind;

typedef struct {
    uint8_t bank;  /* caller bank (0xff = system) */
    uint32_t addr; /* CPU address of the JSR/JMP/LDX instruction */
    int kind;      /* ApexHookCallKind */
    size_t slot;   /* index into ApexWpcHooks.items */
} ApexWpcHookCaller;

typedef struct {
    uint32_t slot_addr;   /* CPU address of the descriptor (system bank) */
    uint8_t target_bank;  /* 0xff = system */
    uint32_t target_addr; /* CPU address of the game routine */
    int is_stub;          /* target is a bare RTS (hook unused by the game) */
    int shared;           /* number of slots with the same target (incl. this) */
    size_t caller_count;  /* callers of this slot in ApexWpcHooks.callers */
} ApexWpcHook;

typedef struct {
    ApexWpcHook *items;
    size_t count;
    ApexWpcHookCaller *callers; /* sorted by slot, then caller bank/address */
    size_t caller_count;
    uint32_t helpers[8]; /* system-bank far-call helpers that take a slot inline */
    size_t helper_count;
} ApexWpcHooks;

/* Detect the hook table of the project's ROM.  Returns 0 on success (out->count
   may be 0 when no table is found). */
int apex_wpc_detect_hooks(const ApexProject *project, ApexWpcHooks *out);
void apex_wpc_hooks_free(ApexWpcHooks *hooks);

/* Name of the code label at or before addr in the same bank (the routine that
   contains addr), or NULL.  *start (may be NULL) receives its address. */
const char *apex_wpc_routine_at(const ApexProject *project, uint8_t bank, uint32_t addr,
                                uint32_t *start);

#endif /* APEX_WPC_HOOKS_H */

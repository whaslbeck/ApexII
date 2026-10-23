#ifndef APEX_CARD_H
#define APEX_CARD_H

/* Routine card: what one routine does, at a glance -- its extent (followed
 * through its branches up to the next routine), who calls it, what it calls
 * (far calls through an [inline] far-code field included), the RAM it reads
 * and writes, and the data it refers to (strings, tables, frames).  A routine
 * start is a call target, an [entries] address or the given address. */

#include "apex_project.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t bank;
    uint32_t addr;
} ApexCardLoc;

typedef struct {
    uint8_t bank; /* the caller's instruction / table row */
    uint32_t addr;
    int from_table; /* a dispatch-table row */
} ApexCardCaller;

typedef struct {
    uint8_t bank; /* the callee */
    uint32_t addr;
    int far;  /* through a far-call helper's inline target */
    int tail; /* a jump, not a call */
    int count;
} ApexCardCallee;

typedef struct {
    uint32_t addr; /* RAM address */
    int reads, writes;
} ApexCardRam;

typedef struct {
    uint8_t bank; /* the data */
    uint32_t addr;
    uint32_t from; /* the referencing instruction */
    int is_string;
} ApexCardData;

typedef struct {
    uint8_t bank;
    uint32_t addr;
    uint32_t low, high; /* extent: lowest instruction, end of the highest */
    size_t insns;
    int returns; /* RTS / PULS ...PC / RTI */
    ApexCardCaller *callers;
    size_t caller_count;
    ApexCardCallee *callees;
    size_t callee_count;
    ApexCardRam *ram;
    size_t ram_count;
    ApexCardData *data;
    size_t data_count;
    ApexCardLoc *insn; /* every instruction of the routine (sorted) */
    size_t insn_count;
} ApexRoutineCard;

int apex_routine_card(const ApexProject *project, uint8_t bank, uint32_t addr,
                      ApexRoutineCard *out);
void apex_routine_card_free(ApexRoutineCard *card);
/* 1 if (bank, addr) is an instruction of the card's routine. */
int apex_routine_card_has(const ApexRoutineCard *card, uint8_t bank, uint32_t addr);

#endif /* APEX_CARD_H */

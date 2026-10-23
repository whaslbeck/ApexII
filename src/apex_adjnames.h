#ifndef APEX_ADJNAMES_H
#define APEX_ADJNAMES_H

/* Adjustment and audit names from the ROM's text tables.
 *
 * WPC games keep the names of their adjustments and audits in tables of
 * 16-bit string pointers whose entry 0 points at "NULL", one table per
 * language: the OS's standard adjustments and audits (English first; entry 1
 * of the adjustments is "BALLS PER GAME", of the audits "TOTAL EARNINGS"),
 * and the game's feature adjustments and audits as (adjustments, audits) pairs
 * per language.  The OS does not store the tables' addresses, so they are
 * recognised by content: the standard ones by their English entry 1; the
 * feature pair outside the standard bank, its audits the longer list, not a
 * numbered test list, its names the most setting-like (PERCENT, MEMORY,
 * TIME, MAX, ...).  The feature guess can be overridden.  The game code passes an
 * adjustment as an index byte: 0x80 + n for standard adjustment n, n for
 * feature adjustment n -- so one type can name both.
 */

#include "apex_project.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t bank;
    uint32_t addr;
    size_t count; /* entries, the "NULL" entry 0 included */
    char **names; /* the strings (entry 0 = "NULL") */
} ApexNameTable;

typedef struct {
    ApexNameTable *tables; /* every NULL-led table of 10+ strings, in ROM order */
    size_t count;
    int std_adj, std_aud;   /* indices into tables, -1 = not found */
    int feat_adj, feat_aud; /* -1 = not found / ambiguous */
} ApexAdjTables;

int apex_adj_find_tables(const ApexProject *project, ApexAdjTables *out);
void apex_adj_tables_free(ApexAdjTables *t);
/* The table at (bank, addr), or -1. */
int apex_adj_table_at(const ApexAdjTables *t, uint8_t bank, uint32_t addr);

/* Values for a [types] byte type, "0xNN:NAME, ..." (malloc'd): with
   adjustments = 1, standard adjustment n as 0x80 + n and feature adjustment n
   as n; else table `table` as plain indices.  Names become identifiers
   (A-Z, 0-9, _), duplicates get a suffix. */
char *apex_adj_type_values(const ApexAdjTables *t, int adjustments, int table);

#endif /* APEX_ADJNAMES_H */

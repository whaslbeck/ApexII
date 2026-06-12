#ifndef APEX_COMPARE_H
#define APEX_COMPARE_H

#include <stddef.h>
#include <stdint.h>

#include "apex_match.h"

struct ApexProject;

/*
 * ROM version comparison.
 *
 * Compares two fully-analysed projects (ROM A = the reference / current
 * project, ROM B = the other version) and classifies every code routine,
 * string and configured table into one of five states.  Correspondence is
 * established structurally so it survives address shifts between versions:
 *
 *   identical  - present in both, byte/fingerprint-identical, same address
 *   moved      - identical body, different address (relocated)
 *   changed    - same address (anchor), body differs (patched in place)
 *   removed    - only in A
 *   added      - only in B
 *
 * Code uses the apex_match fingerprint database (L1 mnemonic + L2 immediate
 * hashes).  Strings compare by exact byte content with an address anchor.
 * Tables compare the raw byte span of each table configured in A against B at
 * the same address (B table discovery needs B to carry its own config).
 */

typedef enum {
    APEX_CMP_CODE = 0,
    APEX_CMP_STRING,
    APEX_CMP_TABLE
} ApexCompareKind;

typedef enum {
    APEX_CMP_IDENTICAL = 0,
    APEX_CMP_MOVED,
    APEX_CMP_CHANGED,
    APEX_CMP_REMOVED,
    APEX_CMP_ADDED
} ApexCompareStatus;

#define APEX_CMP_LABEL_MAX  64
#define APEX_CMP_DETAIL_MAX 80

typedef struct {
    ApexCompareKind   kind;
    ApexCompareStatus status;
    int      has_a;             /* a_bank/a_addr valid (false for ADDED) */
    int      has_b;             /* b_bank/b_addr valid (false for REMOVED) */
    uint8_t  a_bank;
    uint32_t a_addr;
    uint8_t  b_bank;
    uint32_t b_addr;
    int      confidence;        /* 0-100; 100 = exact body match */
    char     label[APEX_CMP_LABEL_MAX];
    char     detail[APEX_CMP_DETAIL_MAX];
} ApexCompareEntry;

typedef struct {
    ApexCompareEntry *items;
    size_t            count;
    size_t            cap;
    /* summary counts across all kinds */
    size_t n_identical;
    size_t n_moved;
    size_t n_changed;
    size_t n_removed;
    size_t n_added;
} ApexCompareReport;

typedef struct {
    int include_code;
    int include_strings;
    int include_tables;
    int include_identical;  /* emit identical entries (false keeps the report short) */
    int min_instrs;         /* skip code routines shorter than this (default 5) */
} ApexCompareOptions;

void apex_compare_default_options(ApexCompareOptions *o);

/* Build a comparison report.  Both projects must already be analysed.
   Returns 0 on success.  Caller frees with apex_compare_report_free. */
int apex_compare_run(const struct ApexProject *a, const struct ApexProject *b,
                     const ApexCompareOptions *opt, ApexCompareReport *out);

void apex_compare_report_free(ApexCompareReport *r);

const char *apex_compare_status_name(ApexCompareStatus s);
const char *apex_compare_kind_name(ApexCompareKind k);

/* ---- cursor sidebar: top-N candidate matches in B for one A routine ---- */

typedef struct {
    uint8_t  bank;
    uint32_t addr;
    int      confidence;        /* 0-100 */
    int      exact;             /* L1+L2 exact body match */
} ApexCompareCandidate;

/* Rank up to max_out candidate routines in b_db for the routine at
   (a_bank, a_addr) in a_db, best first.  Returns the number written. */
size_t apex_compare_candidates(const ApexFingerprintDB *a_db,
                               const ApexFingerprintDB *b_db,
                               uint8_t a_bank, uint32_t a_addr,
                               ApexCompareCandidate *out, size_t max_out);

#endif /* APEX_COMPARE_H */

#ifndef APEX_MATCH_H
#define APEX_MATCH_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct ApexProject;

/*
 * Per-function fingerprint.  Three independent hashes at increasing
 * specificity:
 *
 *   l1_hash  - mnemonic sequence only (address-independent)
 *   l2_hash  - mnemonic sequence + immediate operand values
 *   callee_hash - sequence of callee l1_hashes (structural)
 *
 * All three are FNV-32 over their respective data streams.
 */
typedef struct {
    uint32_t  addr;
    uint8_t   bank;
    uint32_t  l1_hash;
    uint32_t  l2_hash;
    uint32_t  callee_hash;
    uint16_t  instr_count;
    const char *label_name;  /* NULL for auto-generated labels */
} ApexFunctionFingerprint;

/* Opaque fingerprint database for one ROM/config pair. */
typedef struct ApexFingerprintDB ApexFingerprintDB;

/*
 * Build a fingerprint database from a fully-analysed project.
 * The project must have been analysed before calling this.
 * Returns NULL on allocation failure.
 */
ApexFingerprintDB *apex_fingerprint_build(const struct ApexProject *p);
void               apex_fingerprint_free(ApexFingerprintDB *db);
size_t             apex_fingerprint_count(const ApexFingerprintDB *db);
const ApexFunctionFingerprint *apex_fingerprint_get(const ApexFingerprintDB *db, size_t i);

/* Retrieve by (bank, addr).  Returns NULL if not found. */
const ApexFunctionFingerprint *apex_fingerprint_at(const ApexFingerprintDB *db,
                                                    uint8_t bank, uint32_t addr);

/*
 * Fingerprint raw ROM bytes at (bank, addr) for up to max_instrs instructions
 * (0 = internal default), without stopping at labels.  Used by ROM compare to
 * re-fingerprint version B at version A's address and instruction count, so an
 * unchanged routine hashes identically regardless of B's own config/analysis.
 * Writes the L1/L2 hashes and returns the instruction count actually decoded.
 */
uint16_t apex_fingerprint_raw(const struct ApexProject *p, uint8_t bank, uint32_t addr,
                              uint16_t max_instrs, uint32_t *l1_out, uint32_t *l2_out);

/* ---------- matching ---------- */

#define APEX_MATCH_CONF_EXACT   90  /* l1 + l2 match */
#define APEX_MATCH_CONF_HIGH    75  /* l1 + callee structure */
#define APEX_MATCH_CONF_MEDIUM  55  /* l1 only */

typedef struct {
    const char *label_name;   /* from source — never NULL */
    uint32_t    src_addr;
    uint8_t     src_bank;
    uint32_t    dst_addr;
    uint8_t     dst_bank;
    int         confidence;   /* 0-100 */
} ApexMatchResult;

/*
 * Inject source explicit-entry addresses into a target project so that the
 * subsequent apex_project_analyze() discovers more code.
 * system_only: only inject system-bank (0xff) entries — safe even for
 *              cross-game matching where paged-bank addresses differ.
 */
void apex_match_inject_entries(struct ApexProject       *dst,
                               const struct ApexProject *src,
                               int                       system_only);

/*
 * Match named source functions against all code labels discovered in target.
 * Writes result count to *out_count.  Caller must free with apex_match_results_free.
 * min_confidence: skip results below this threshold (0 = all).
 * min_instrs_for_medium: medium-confidence matches require at least this many
 *   instructions (reduces false positives for short routines; 0 = no filter).
 */
ApexMatchResult *apex_match_roms(const ApexFingerprintDB *src_db,
                                 const ApexFingerprintDB *dst_db,
                                 int                      min_confidence,
                                 int                      min_instrs_for_medium,
                                 size_t                  *out_count);

void apex_match_results_free(ApexMatchResult *results, size_t count);

/*
 * Second-phase scan: for each named source function that was NOT matched by
 * apex_match_roms, scan every byte position in the target's system bank for
 * an exact L1+L2 fingerprint match.
 *
 * Addresses not reachable from interrupt vectors (e.g. utility routines only
 * called from game code) are invisible to the code-flow phase; this catch-all
 * scan finds them.  Only EXACT matches (L1 + L2) are emitted to keep the
 * false-positive rate negligible.  Ambiguous positions (same fingerprint at
 * multiple offsets) are silently skipped.
 *
 * prior/prior_count: the results already found by apex_match_roms — used to
 *   skip source functions that have already been matched.
 * min_instrs: functions shorter than this are excluded (default: 4).
 *
 * Returns a freshly allocated result array; caller frees with
 * apex_match_results_free.  Writes new count to *out_count.
 */
ApexMatchResult *apex_match_scan_system_bank(
    const ApexFingerprintDB  *src_db,
    const ApexFingerprintDB  *dst_db,   /* for fast callee L1 lookup (L3) */
    const struct ApexProject *dst,
    const ApexMatchResult    *prior,
    size_t                    prior_count,
    int                       min_instrs,
    size_t                   *out_count);

/*
 * Write match results as a .ini overlay (labels + inline sigs from source).
 * src_project is used to transfer inline signatures and routine docs.
 */
void apex_match_write_ini(FILE *out,
                          const ApexMatchResult       *results,
                          size_t                       count,
                          const struct ApexProject    *src_project);

#endif /* APEX_MATCH_H */

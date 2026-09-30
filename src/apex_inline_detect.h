#ifndef APEX_INLINE_DETECT_H
#define APEX_INLINE_DETECT_H

/* Automatic detection of inline-argument routines in the system ("prime")
 * bank.  Two independent detectors:
 *
 *  - Marker: the OS places the two bytes 0xCF, 0x73+N directly before a
 *    routine that takes N inline bytes (0x74 = 1, 0x75 = 2, ...).
 *  - Walker: a routine that loads its return address from the stack
 *    (LDX/LDU/LDY n,S after pushing n bytes), consumes the inline bytes through
 *    that pointer (,R+ / ,R++ / PULU / offset loads) and writes the advanced
 *    pointer back (STX n,S ...), or pops it and returns via JMP ,R.  The
 *    accesses give the byte / word layout.
 *
 * Findings are compared with the configured [inline] signatures; applying
 * them never silently overwrites a hand-made signature.
 */

#include "apex_project.h"

#include <stddef.h>
#include <stdint.h>

#define APEX_INL_SRC_MARKER 1u
#define APEX_INL_SRC_WALKER 2u

typedef enum { APEX_INL_LOW = 1, APEX_INL_MEDIUM = 2, APEX_INL_HIGH = 3 } ApexInlineConfidence;

typedef enum {
    APEX_INL_NEW = 0,       /* no [inline] signature yet */
    APEX_INL_CONFIRMED = 1, /* configured signature has the detected length */
    APEX_INL_CONFLICT = 2   /* configured signature has a different length */
} ApexInlineStatus;

typedef struct {
    uint32_t addr;       /* routine start (system bank) */
    unsigned sources;    /* APEX_INL_SRC_* */
    unsigned length;     /* inline bytes */
    unsigned marker_len; /* length stated by the marker (0 = no marker) */
    unsigned walker_len; /* length found by the walker (0 = walker failed) */
    char spec[128];      /* proposed [inline] value, e.g. "word, byte" */
    int confidence;      /* ApexInlineConfidence */
    int status;          /* ApexInlineStatus */
    char config_spec[128];
    int callsites;    /* calls found by the analysis */
    int callsites_ok; /* calls after which code resumes plausibly */
    int is_code;      /* already known as a code routine */
    char note[96];
} ApexInlineFinding;

typedef struct {
    ApexInlineFinding *items;
    size_t count;
    size_t cap;
} ApexInlineFindings;

/* Detect inline routines in the analysed project's system bank (sorted by
   address).  Returns 0 on success. */
int apex_detect_inline_routines(const ApexProject *project, ApexInlineFindings *out);
void apex_free_inline_findings(ApexInlineFindings *findings);

/* Apply findings as one undo step: [inline] signature, a bytes[2] data range
   over a marker and a code entry for routines not yet known as code.  Only
   status NEW with confidence >= min_confidence is applied (plus CONFLICT ones
   when replace_conflicts).  index >= 0 applies just that finding regardless of
   confidence/status.  Returns the number of findings applied. */
int apex_apply_inline_findings(ApexProject *project, const ApexInlineFindings *findings,
                               int min_confidence, int replace_conflicts, long index);

const char *apex_inline_confidence_name(int confidence);
const char *apex_inline_status_name(int status);

#endif /* APEX_INLINE_DETECT_H */

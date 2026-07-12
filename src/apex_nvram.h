#ifndef APEX_NVRAM_H
#define APEX_NVRAM_H

/* Import/export of RAM maps in the PinMAME NVRAM-maps JSON format
   (https://github.com/tomlogic/pinmame-nvram-maps).  A "location" is one
   documented RAM address: a name (from a descriptor's short_label/label) and a
   doc string (its human-readable label), at a CPU address (the descriptor's
   `start`, treated as a CPU address per fileformat v0.7+). */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    *name;   /* sanitized, valid symbol name (owned) */
    uint32_t addr;   /* CPU address */
    char    *doc;    /* human-readable description (owned; may be empty) */
} ApexNvramLoc;

typedef struct {
    ApexNvramLoc *items;
    size_t        count;
    size_t        cap;
} ApexNvramLocs;

/* Parse NVRAM-maps JSON text into a list of RAM locations.  Returns 0 on
   success; on failure returns non-zero and writes a message into err (if
   non-NULL).  `base` is added to every descriptor's `start` (0 for v0.7+ maps
   whose `start` is already a CPU address). */
int apex_nvram_parse_json(const char *text, size_t len, uint32_t base,
                          ApexNvramLocs *out, char *err, size_t errsz);

/* Write a minimal valid NVRAM-maps JSON document for the given locations.
   `rom_name` (may be NULL) is listed under _metadata.roms. Returns 0 on success. */
int apex_nvram_write_json(FILE *out, const ApexNvramLoc *items, size_t count,
                          const char *rom_name);

/* Zero-loss export: rewrite the template JSON, overriding each descriptor's
   short_label/label with the matching (by `start` address) location's name/doc,
   and appending any locations not present in the template.  Every other field of
   every descriptor is preserved verbatim.  Returns 0 on success. */
int apex_nvram_export_merged(FILE *out, const char *template_text, size_t template_len,
                             const ApexNvramLoc *items, size_t count,
                             char *err, size_t errsz);

void apex_nvram_locs_free(ApexNvramLocs *locs);

#ifdef __cplusplus
}
#endif

#endif /* APEX_NVRAM_H */

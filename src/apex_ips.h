#ifndef APEX_IPS_H
#define APEX_IPS_H

/* Distributable patch formats: IPS (offset/length records) and BPS (beat:
   source/target reads with CRC-32 checks of source, target and patch).  Both
   are made from the original and the patched image (same size: patches never
   grow the ROM) and can be applied to the original. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Make a patch; returns a malloc'd buffer (*len bytes), NULL on failure. */
uint8_t *apex_ips_make(const uint8_t *orig, const uint8_t *patched, size_t size, size_t *len);
uint8_t *apex_bps_make(const uint8_t *orig, const uint8_t *patched, size_t size,
                       const char *metadata, size_t *len);

/* Apply an IPS or BPS patch (detected by its magic) to src.  Returns a
   malloc'd image (*out_size bytes), or NULL with a message. */
uint8_t *apex_patchfile_apply(const uint8_t *src, size_t src_size, const uint8_t *patch,
                              size_t patch_len, size_t *out_size, char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif /* APEX_IPS_H */

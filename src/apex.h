#ifndef APEX_H
#define APEX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define APEX_BANK_SIZE 0x4000u
#define APEX_SYSTEM_SIZE 0x8000u
#define APEX_PAGED_ORG 0x4000u
#define APEX_SYSTEM_ORG 0x8000u
/* Addresses below the paged window are RAM/ASIC (not in the ROM image). */
#define APEX_RAM_LIMIT 0x4000u

typedef struct {
    uint8_t *data;
    size_t size;
} Buffer;

#if defined(__GNUC__) || defined(__clang__)
#define APEX_THREAD_LOCAL __thread
#else
#define APEX_THREAD_LOCAL
#endif

extern void (*apex_die_hook)(const char *msg);
void die(const char *fmt, ...);
/* Run fn(ctx); a die() inside it returns 1 (message copied to err) instead of
   exiting.  Memory fn allocated before the die() is leaked, so only use it
   around parsers that have not yet mutated caller state. */
int apex_try(void (*fn)(void *), void *ctx, char *err, size_t errsz);
/* Source position prefixed to die() messages as "path:line: " (thread-local;
   path NULL clears it).  Parsers set it per line and restore the previous one
   when done, so nested includes report the innermost file. */
void apex_set_die_context(const char *path, unsigned long line);
void apex_get_die_context(const char **path, unsigned long *line);
void *xmalloc(size_t size);
Buffer read_file(const char *path);
void write_file(const char *path, const uint8_t *data, size_t size);
int parse_u32(const char *text, uint32_t *out);
/* Strict integer argument (decimal or 0x hex, whole string, within [lo, hi]).
   Returns 1 on success; atoi() would silently turn "foo" into 0. */
int apex_parse_int_arg(const char *text, long lo, long hi, long *out);
/* Path helpers that accept both '/' and '\\' (and a Windows drive prefix). */
const char *apex_path_basename(const char *path);
int apex_path_is_absolute(const char *path);
char *trim(char *s);

#endif

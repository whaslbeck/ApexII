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

extern void (*apex_die_hook)(const char *msg);
void die(const char *fmt, ...);
void *xmalloc(size_t size);
Buffer read_file(const char *path);
void write_file(const char *path, const uint8_t *data, size_t size);
int parse_u32(const char *text, uint32_t *out);
char *trim(char *s);

#endif

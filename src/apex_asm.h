#ifndef APEX_ASM_H
#define APEX_ASM_H

/* The ApexII assembler as a library.
 *
 * Whole-program mode assembles .asm files (with .ROM_SIZE/.BANK/.ORG) into a
 * ROM image, as the apexasm tool does.  Snippet mode assembles a piece of
 * source at a fixed bank/address into a byte buffer -- the building block of
 * ROM patches: symbols the snippet does not define are looked up through a
 * caller-supplied resolver (e.g. the labels of an analysed ROM).
 *
 * Both modes report errors through the return value and err (no exit()).
 */

#include <stddef.h>
#include <stdint.h>

/* Resolve a symbol the source does not define: returns 1 and sets the value;
   has_bank and bank give the far bank of a ROM label (0xff = system). */
typedef int (*ApexAsmResolveFn)(void *ctx, const char *name, uint32_t *value, int *has_bank,
                                uint8_t *bank);

/* Whole-program mode: assemble the files (later files may overwrite earlier
   bytes) into a ROM image.  On success *rom (malloc'd) and *rom_size are set.
   Returns 0 on success, non-zero with a message in err. */
int apex_asm_files(const char *const *paths, size_t count, uint8_t **rom, size_t *rom_size,
                   char *err, size_t errsz);

typedef struct {
    char *name;
    uint32_t addr;
} ApexAsmLabel;

typedef struct {
    uint8_t *bytes; /* emitted bytes, starting at the snippet's org */
    size_t size;
    ApexAsmLabel *labels; /* labels the snippet defines */
    size_t label_count;
} ApexAsmSnippet;

/* Snippet mode: assemble text at (bank, org) of a ROM of rom_size bytes; bank
   is a bank id (0xff = system bank, org 0x8000-0xffff; otherwise a paged bank,
   org 0x4000-0x7fff).  The snippet may not use .ROM_SIZE/.BANK/.ORG/BANK_ID/
   FILL_TO_BANK_END.  With sizing set, symbols that do not resolve are taken as
   0 (to learn the size and labels before other snippets are placed).  Returns 0
   on success, non-zero with a message in err ("line N: ..."). */
int apex_asm_snippet(const char *text, uint8_t bank, uint32_t org, size_t rom_size,
                     ApexAsmResolveFn resolve, void *ctx, int sizing, ApexAsmSnippet *out,
                     char *err, size_t errsz);
void apex_asm_snippet_free(ApexAsmSnippet *snippet);

#endif /* APEX_ASM_H */

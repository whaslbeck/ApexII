#ifndef APEX_PATCH_H
#define APEX_PATCH_H

/* ROM patches.
 *
 * A patch file (<game>.patch.ini) describes changes to an original ROM as an
 * ordered list of operations, separate from the analysis config:
 *
 *     [patch]
 *     base_rom   = afm_113b.bin        ; paths relative to the patch file
 *     base_crc32 = 0x1234abcd          ; refuses to build on another ROM
 *     analysis   = afm.ini             ; labels for addresses and asm symbols
 *     checksum   = fix                 ; fix | disable | keep
 *     output     = afm_patched.bin
 *
 *     [op:longer_ballsave]
 *     type   = bytes
 *     at     = BallSave_Start+0x0c     ; label[+/-offset], Bxx_Ayyyy or 0x8xxx
 *     expect = 86 0a                   ; original bytes (guard)
 *     bytes  = 86 14
 *
 *     [op:skip_check]
 *     type   = nop
 *     at     = B38_A4a96
 *     count  = 2                       ; instructions (or length = bytes)
 *
 *     [op:new_logic]
 *     type   = asm
 *     at     = B38_A48e3
 *     length = 6                       ; bytes replaced (default: the code's
 *     code   =                         ; size, rounded up to the next original
 *         | LDA #0x14                  ; instruction boundary); NOP padded
 *         | JSR Lamp_On
 *     ; or: file = new_logic.asm
 *
 * Further op types (keys below, examples in README.md "apexpatch"): hook
 * (detour), ptr, ram, free, reserve, value, string, move, blob, dmd, sprite,
 * branch (always / never / invert), redirect (a new jump / call target).
 *
 * Code ops (nop, asm, hook, branch, redirect) must start on an instruction
 * boundary of the analysed code (outside analysed code they only warn); one
 * that ends inside an instruction is NOP-padded or warned.  `enabled = no`
 * skips an op.  Lines starting
 * with '|' continue a value verbatim (asm comments with ';' are kept);
 * comments in front of a section are preserved when the file is rewritten.
 */

#include "apex_project.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    APEX_PATCH_BYTES = 0,
    APEX_PATCH_NOP,
    APEX_PATCH_ASM,
    APEX_PATCH_HOOK,     /* detour: JMP at `at` to code + displaced instructions + JMP back */
    APEX_PATCH_PTR,      /* write a pointer to `target` (width 2 or 3 = far) at `at` */
    APEX_PATCH_RAM,      /* RAM variable: `label` at RAM address `at`, `length` bytes */
    APEX_PATCH_FREE,     /* declare `length` bytes at `at` free for allocations */
    APEX_PATCH_RESERVE,  /* keep `length` bytes at `at` out of the free space */
    APEX_PATCH_VALUE,    /* set a `width`-byte field at `at` to `value` (number, enum, label) */
    APEX_PATCH_STRING,   /* replace the string at `at` with `text` (see `mode`) */
    APEX_PATCH_MOVE,     /* move the table / data / string at `at` to free space */
    APEX_PATCH_BLOB,     /* the bytes of `file`, at `at` or allocated (`alloc`, `label`) */
    APEX_PATCH_DMD,      /* replace the DMD fullframe at `at` with the image `file` */
    APEX_PATCH_SPRITE,   /* replace the sprite (VSI image) at `at` with the image `file` */
    APEX_PATCH_BRANCH,   /* the conditional branch at `at`: mode = always | never | invert */
    APEX_PATCH_REDIRECT, /* the jump / call / branch at `at` goes to `target` instead */
    APEX_PATCH_TYPE_COUNT
} ApexPatchOpType;

typedef enum {
    APEX_PATCH_CHECKSUM_FIX = 0,
    APEX_PATCH_CHECKSUM_DISABLE,
    APEX_PATCH_CHECKSUM_KEEP
} ApexPatchChecksum;

typedef struct {
    char *name;
    int type; /* ApexPatchOpType */
    int enabled;
    char *at;        /* address expression */
    char *expect;    /* original bytes as hex text, NULL = no guard */
    char *bytes;     /* bytes op: hex text */
    char *code;      /* asm / hook: inline source */
    char *file;      /* asm / hook: source file; blob: the bytes; dmd / sprite: the
                        image (PNG / PBM / PGM); relative to the patch file */
    unsigned count;  /* nop: instructions */
    unsigned length; /* nop / asm: bytes replaced (0 = derived); ram / free / reserve:
                        size; move: the block's size (0 = from the analysis) */
    char *alloc;     /* asm / blob / string / move / dmd / sprite: where in free space
                        ("same" = the site's bank, then system -- the default;
                        "system", "bank:NN", "any"); hook: where the detour goes */
    char *placed;    /* the allocation, pinned after the first build (Bxx_Ayyyy) */
    char *target;    /* ptr / redirect: the label pointed / jumped to */
    char *label;     /* ram: the variable's name; blob: a label for the bytes */
    unsigned width;  /* ptr: 2 (word) or 3 (far, default); value: 1, 2 or 3 */
    char *value;     /* value: number, TYPE enum value name, equate or label */
    char *vtype;     /* value: the [types] type whose enum names `value` may use */
    char *text;      /* string: the new text (\n, \t, \a escapes) */
    char *mode;      /* string: auto (default) | inplace | shift | relocate;
                        dmd / sprite: auto | inplace | relocate;
                        branch: always (default) | never | invert */
    int accept;      /* string / move / dmd / sprite: build despite references that
                        cannot be rewritten automatically (accept = yes) */
    unsigned planes; /* dmd: bit-planes 1-4 (0 = from the analysis, else 1) */
    char *encoding;  /* dmd: auto (smallest of raw / run-length, default) | raw */
    int bright;      /* dmd / sprite: the plane lit longer (level 2) of a 2-plane
                        image: 0 or 1, -1 = default (1) */
    char *note;
    char *comment; /* comment lines in front of the section (kept on save) */
} ApexPatchOp;

typedef struct {
    char *path; /* the patch file */
    char *base_rom;
    char *analysis;
    char *output;
    uint32_t base_crc32;
    int has_crc;
    int checksum;  /* ApexPatchChecksum */
    char *comment; /* comment lines in front of [patch] */
    ApexPatchOp *ops;
    size_t count;
    size_t cap;
} ApexPatch;

/* Parse a patch file.  Returns 0 on success, else non-zero with "line N: ..."
   in err. */
int apex_patch_load(const char *path, ApexPatch *out, char *err, size_t errsz);
/* Write the patch in canonical form.  Returns 0 on success. */
int apex_patch_save(const ApexPatch *patch, const char *path);
/* The same as text (in memory, e.g. for undo snapshots), and back: path is
   the file the text belongs to (relative paths resolve against it). */
void apex_patch_write(const ApexPatch *patch, FILE *f);
char *apex_patch_to_text(const ApexPatch *patch); /* malloc'd, NULL on failure */
int apex_patch_load_text(const char *path, const char *text, ApexPatch *out, char *err,
                         size_t errsz);
void apex_patch_free(ApexPatch *patch);
/* `path` as a patch file at patch_path should record it: relative when it lies
   in the patch file's directory (or below), else absolute. */
void apex_patch_relpath(const char *patch_path, const char *path, char *out, size_t outsz);
/* base_rom / analysis / output resolved against the patch file's directory. */
void apex_patch_path(const ApexPatch *patch, const char *rel, char *out, size_t outsz);

typedef enum {
    APEX_PATCH_OK = 0,
    APEX_PATCH_WARN,    /* applied, see msg */
    APEX_PATCH_ERROR,   /* not applied */
    APEX_PATCH_DISABLED /* enabled = no */
} ApexPatchStatus;

typedef struct {
    int status; /* ApexPatchStatus */
    char msg[256];
    uint8_t bank;
    uint32_t addr;    /* CPU address of the first byte replaced */
    size_t rom_off;   /* file offset */
    size_t length;    /* bytes replaced */
    uint8_t *orig;    /* original bytes (length) */
    uint8_t *patched; /* new bytes (length) */
    /* References rewritten (string shift/relocate, move): see fixups. */
    size_t fixup_first; /* index into ApexPatchBuild.fixups */
    size_t fixup_count;
    char mode_used[16]; /* string: inplace / shift / relocate */
    uint8_t *guard;     /* string / move: the original bytes `expect` guards (the
                           old string, the block's first bytes) */
    size_t guard_len;
    size_t old_length; /* string / move / dmd: bytes of the original data */
    /* Allocated region (alloc asm: the code; hook: the detour code;
       string relocate / move: the new place). */
    int has_alloc;
    uint8_t alloc_bank;
    uint32_t alloc_addr;
    size_t alloc_rom_off;
    size_t alloc_length;
    uint8_t *alloc_bytes;
} ApexPatchOpResult;

/* One reference to moved data, and how the build handles it. */
typedef enum {
    APEX_FIXUP_OK = 0,     /* rewritten */
    APEX_FIXUP_MANUAL,     /* the old address is not uniquely in the referencing bytes */
    APEX_FIXUP_UNREACHABLE /* a 16-bit reference cannot reach the new bank */
} ApexFixupStatus;

typedef struct {
    uint8_t src_bank; /* the referencing instruction / table row */
    uint32_t src_addr;
    size_t rom_off; /* the bytes rewritten (2 or 3), when status is OK */
    int far;        /* 3-byte (address + bank) reference */
    uint8_t old_bank, new_bank;
    uint32_t old_addr, new_addr;
    int status;    /* ApexFixupStatus */
    char what[48]; /* e.g. "code", "table row 3" */
} ApexPatchFixup;

/* A symbol the patch defines: a label of its code, or a RAM variable. */
typedef struct {
    char *name;
    uint8_t bank; /* 0xff = system bank; ignored for RAM */
    uint32_t addr;
    int is_code; /* label inside patch code (a code entry when it starts code) */
    int is_ram;
    int starts_code; /* first byte of an allocated / detour code block */
} ApexPatchSym;

/* Free space: [addr, addr+len) in a bank (0xff = system). */
typedef struct {
    uint8_t bank;
    uint32_t addr;
    uint32_t len;
    int kind; /* APEX_FREE_TRAILING / _INTERIOR / _DECLARED */
} ApexFreeBlock;

enum { APEX_FREE_TRAILING = 0, APEX_FREE_INTERIOR, APEX_FREE_DECLARED };

typedef struct {
    ApexFreeBlock *items;
    size_t count;
    size_t cap;
} ApexFreeSpace;

/* Detect free space in the original image: runs of 0xFF at the end of a bank
   (trailing) and long runs inside one (interior, >= 64 bytes, without labels,
   data ranges or tables; the first byte is kept as a possible terminator).
   The system bank's vector area (0xFFE0-) is never free. */
void apex_patch_detect_free(const ApexProject *project, const uint8_t *base, ApexFreeSpace *out);
void apex_patch_free_space_free(ApexFreeSpace *fs);

typedef struct {
    ApexPatchOpResult *ops; /* one per patch op, same order */
    size_t count;
    int errors;
    int warnings;
    char msg[256]; /* patch-level error (e.g. CRC mismatch), "" if none */
    uint8_t *rom;  /* patched image (NULL if a patch-level error stopped it) */
    size_t rom_size;
    ApexPatchSym *syms; /* labels and RAM variables the patch defines */
    size_t sym_count;
    ApexPatchFixup *fixups; /* all ops' reference rewrites (see op results) */
    size_t fixup_count;
    ApexFreeSpace free_total; /* detected + declared - reserved */
    ApexFreeSpace free_left;  /* what is still free after this build */
} ApexPatchBuild;

/* Apply the patch to the project's ROM (the original image is not changed).
   Ops with errors are skipped and counted; the image is still produced so all
   problems are reported at once.  The checksum is handled per patch->checksum.
   Returns 0 when the build has no errors. */
int apex_patch_build(const ApexPatch *patch, const ApexProject *project, ApexPatchBuild *out);
/* Same, against an explicit original image (base, base_size) instead of the
   project's ROM -- for a frontend whose project shows the patched bytes.  The
   project still provides labels, [inline] signatures and analysed code. */
int apex_patch_build_base(const ApexPatch *patch, const ApexProject *project, const uint8_t *base,
                          size_t base_size, ApexPatchBuild *out);
/* Same, with the free space detected beforehand (apex_patch_detect_free on the
   original's analysis) -- a patched view's analysis sees labels inside the
   patch code and would no longer find that space free.  NULL = detect now. */
int apex_patch_build_free_hint(const ApexPatch *patch, const ApexProject *project,
                               const uint8_t *base, size_t base_size, const ApexFreeSpace *detected,
                               ApexPatchBuild *out);
void apex_patch_build_free(ApexPatchBuild *build);

/* Append a new op (type, unique name derived from base, at = the address
   expression); returns its index, or -1. */
long apex_patch_add_op(ApexPatch *patch, int type, const char *base_name, const char *at);
void apex_patch_remove_op(ApexPatch *patch, size_t index);
/* Swap ops index and index+dir (dir = -1 / +1); returns 1 if moved. */
int apex_patch_move_op(ApexPatch *patch, size_t index, int dir);
/* Set a string field of an op (op->at, op->code, ...; NULL or "" clears). */
void apex_patch_set_field(char **field, const char *value);

/* Readable, portable address expression for (bank, addr): the nearest code
   label at or before it plus an offset ("Check+0x3"), else Bxx_Ayyyy. */
void apex_patch_format_at(const ApexProject *project, uint8_t bank, uint32_t addr, char *out,
                          size_t outsz);

/* Name of the member of a zip archive whose CRC-32 is
   crc -- the game ROM of a PinMAME romset.  Reads the zip's central directory
   only.  Returns 1 and fills name. */
int apex_patch_romset_member(const char *zip, uint32_t crc, char *name, size_t namesz);

/* Fill in the missing expect guards of ops that built cleanly from the
   original bytes, and pin every allocation (placed = ...).  Returns the
   number of ops changed. */
int apex_patch_pin(ApexPatch *patch, const ApexPatchBuild *build);

/* Resolve an address expression (label[+/-n], Bxx_Ayyyy[+/-n], 0x8000-0xffff)
   against the project.  Returns 1 and sets bank/addr, else 0 with err. */
int apex_patch_resolve_addr(const ApexProject *project, const char *expr, uint8_t *bank,
                            uint32_t *addr, char *err, size_t errsz);

const char *apex_patch_type_name(int type);
const char *apex_patch_status_word(int status); /* ok / warn / ERROR / off */

/* Rebase: how one op's site was found in the target ROM. */
typedef struct {
    int status;        /* ApexPatchStatus: ok, warn (review), error (not found) */
    int moved;         /* the site's address changed */
    int confidence;    /* routine match confidence (apex_match), 0 otherwise */
    int guard_changed; /* found by a code pattern whose address bytes differ */
    char method[64];   /* "same address", "routine X+0x12", "context 48 bytes", ... */
    char msg[160];
    uint8_t old_bank, new_bank;
    uint32_t old_addr, new_addr;
} ApexRebaseOp;

/* Make `out`, the patch for another version of the ROM (dp, analysed; its
   files dst_rom / dst_ini, ini may be NULL): every op's site is looked up in
   the target -- the same address with the same bytes around it, the same
   offset in the routine apex_match pairs with the source routine, or the
   bytes around the site found once -- and must hold the op's original bytes.
   Allocations are unpinned, file paths made relative to new_path.  `sb` is
   the source build, ops[] gets one entry per op.  Returns the number of ops
   not found (-1 on failure). */
int apex_patch_rebase(const ApexPatch *src, const ApexProject *sp, const ApexPatchBuild *sb,
                      const ApexProject *dp, const char *new_path, const char *dst_rom,
                      const char *dst_ini, ApexPatch *out, ApexRebaseOp *ops, char *err,
                      size_t errsz);
/* Find the source's paged-bank routines (entries and named code labels) in
   the target and add them as code entries of dp (before its analysis): code
   of the same version at the same place, or moved code by its pattern with
   the addresses masked.  Returns the number found. */
int apex_patch_rebase_paged_entries(const ApexProject *sp, ApexProject *dp);

/* The whole job on files: open and analyse the target ROM (with dst_ini, or
   else with the source's entry points, and then write <out>.labels.ini with
   the source labels apex_match finds in it -- `labels` gets its name), rebase,
   write `out`.  *ops_out: malloc'd, one per op.  Returns the number of ops
   not found, -1 on failure. */
int apex_patch_rebase_files(const ApexPatch *src, const ApexProject *sp, const ApexPatchBuild *sb,
                            const char *trom, const char *tini, const char *out, ApexPatch *np,
                            ApexRebaseOp **ops_out, char *labels, size_t labelsz, char *err,
                            size_t errsz);

/* One op's original vs new bytes / code / text (apexpatch diff). */
void apex_patch_write_diff_op(FILE *f, const ApexPatch *patch, const ApexPatchBuild *build,
                              size_t index);
/* Markdown report: ROMs, ops, per-op changes and references, the symbols the
   patch defines, free space. */
/* `orig` is the original image (the project may hold the patched bytes). */
void apex_patch_write_report(FILE *f, const char *title, const ApexPatch *patch,
                             const ApexProject *project, const uint8_t *orig,
                             const ApexPatchBuild *build);

#endif /* APEX_PATCH_H */

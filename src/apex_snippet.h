#ifndef APEX_SNIPPET_H
#define APEX_SNIPPET_H

/* Snippet library: reusable patch code with parameters.  A snippet is a .asm
   file whose leading comment lines describe it:

     ; snippet: Count executions
     ; type: hook                       asm | hook (default asm)
     ; about: Increments a RAM byte each time the hooked code runs.
     ; param: VAR  RAM byte to count in
     ; param: STEP increment = 1        "= value" gives a default
         LDA {VAR}
         ADDA #{STEP}
         STA {VAR}

   Inserting a snippet expands {NAME} with the values and makes an ordinary op
   (code = the expanded text, note = "snippet <name>"), so a patch file never
   depends on the library. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *name; /* NAME in {NAME} */
    char *desc;
    char *def; /* default value, NULL = required */
} ApexSnippetParam;

typedef struct {
    char *id; /* file name without .asm */
    char *path;
    char *title; /* "snippet:" line, else the id */
    char *about;
    char *type; /* "asm" or "hook" */
    ApexSnippetParam *params;
    size_t param_count;
    char *body; /* the code lines after the header */
} ApexSnippet;

typedef struct {
    ApexSnippet *items;
    size_t count;
} ApexSnippetList;

/* Load one snippet file; 0 on success. */
int apex_snippet_load(const char *path, ApexSnippet *out, char *err, size_t errsz);
/* Add every *.asm of dir (sorted by id; ids already in the list are skipped,
   so earlier directories win).  Returns the number added. */
int apex_snippet_load_dir(const char *dir, ApexSnippetList *list);
/* The library: <patch dir>/snippets, $APEX_SNIPPETS, ./snippets, and the
   bundled set (<exe_dir>/../snippets, <exe_dir>/snippets); patch_path and
   exe_dir may be NULL.  Returns the number of snippets. */
size_t apex_snippet_load_default(ApexSnippetList *list, const char *patch_path,
                                 const char *exe_dir);
/* The snippet with this id, or NULL. */
const ApexSnippet *apex_snippet_find(const ApexSnippetList *list, const char *id);
void apex_snippet_free(ApexSnippet *s);
void apex_snippet_list_free(ApexSnippetList *list);

/* Expand the body: values[i] belongs to params[i] (NULL or "" = the default).
   Returns malloc'd code, or NULL with a message (a missing value, an unknown
   {NAME}). */
char *apex_snippet_expand(const ApexSnippet *s, const char *const *values, char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif /* APEX_SNIPPET_H */

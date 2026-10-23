#include "apex_snippet.h"

#include "apex.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void snip_err(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    if (!err || !errsz)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static char *dup_trim(const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\t')) {
        s++;
        n--;
    }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        n--;
    char *d = xmalloc(n + 1u);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* "key: value" in a header comment line (after ';'), else 0. */
static int header_kv(const char *line, size_t len, const char *key, const char **val,
                     size_t *vlen) {
    size_t k = strlen(key), i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= len || line[i] != ';')
        return 0;
    i++;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i + k + 1 > len || strncmp(line + i, key, k) || line[i + k] != ':')
        return 0;
    *val = line + i + k + 1;
    *vlen = len - (i + k + 1);
    return 1;
}

int apex_snippet_load(const char *path, ApexSnippet *out, char *err, size_t errsz) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) {
        snip_err(err, errsz, "cannot open %s", path);
        return 1;
    }
    fclose(f);
    Buffer b = read_file(path);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t bl = strlen(base);
    if (bl > 4 && !strcmp(base + bl - 4, ".asm"))
        bl -= 4;
    out->id = dup_trim(base, bl);
    out->path = dup_trim(path, strlen(path));
    const char *t = (const char *)b.data, *end = t + b.size;
    size_t cap = 0;
    while (t < end) {
        const char *nl = memchr(t, '\n', (size_t)(end - t));
        size_t len = nl ? (size_t)(nl - t) : (size_t)(end - t);
        const char *v;
        size_t vl;
        if (header_kv(t, len, "snippet", &v, &vl)) {
            free(out->title);
            out->title = dup_trim(v, vl);
        } else if (header_kv(t, len, "type", &v, &vl)) {
            /* the first word: "type: hook ; a comment" is a hook */
            while (vl && (*v == ' ' || *v == '\t')) {
                v++;
                vl--;
            }
            size_t w = 0;
            while (w < vl && v[w] != ' ' && v[w] != '\t' && v[w] != ';' && v[w] != '\r')
                w++;
            free(out->type);
            out->type = dup_trim(v, w);
        } else if (header_kv(t, len, "about", &v, &vl)) {
            char *a = dup_trim(v, vl);
            if (out->about) { /* several about: lines form one text */
                char *j = xmalloc(strlen(out->about) + strlen(a) + 2u);
                sprintf(j, "%s %s", out->about, a);
                free(out->about);
                free(a);
                out->about = j;
            } else {
                out->about = a;
            }
        } else if (header_kv(t, len, "param", &v, &vl)) {
            char *pl = dup_trim(v, vl);
            if (out->param_count == cap) {
                cap = cap ? cap * 2u : 4u;
                out->params = realloc(out->params, cap * sizeof(*out->params));
            }
            ApexSnippetParam *pa = &out->params[out->param_count++];
            memset(pa, 0, sizeof(*pa));
            size_t n = strcspn(pl, " \t");
            pa->name = dup_trim(pl, n);
            const char *rest = pl + n, *eq = strrchr(rest, '=');
            if (eq) {
                pa->desc = dup_trim(rest, (size_t)(eq - rest));
                pa->def = dup_trim(eq + 1, strlen(eq + 1));
            } else {
                pa->desc = dup_trim(rest, strlen(rest));
            }
            free(pl);
        } else {
            size_t i = 0;
            while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r'))
                i++;
            if (i < len && t[i] != ';') /* the first code line ends the header */
                break;
        }
        t = nl ? nl + 1 : end;
    }
    /* body: from the first code line, without trailing blank lines */
    size_t bodylen = (size_t)(end - t);
    while (bodylen && (t[bodylen - 1] == '\n' || t[bodylen - 1] == '\r' || t[bodylen - 1] == ' '))
        bodylen--;
    out->body = xmalloc(bodylen + 2u);
    memcpy(out->body, t, bodylen);
    out->body[bodylen] = '\n';
    out->body[bodylen + 1] = '\0';
    free(b.data);
    if (!out->title)
        out->title = dup_trim(out->id, strlen(out->id));
    if (!out->type)
        out->type = dup_trim("asm", 3);
    if (strcmp(out->type, "asm") && strcmp(out->type, "hook")) {
        snip_err(err, errsz, "%s: type must be asm or hook", path);
        apex_snippet_free(out);
        return 1;
    }
    if (!bodylen) {
        snip_err(err, errsz, "%s: no code", path);
        apex_snippet_free(out);
        return 1;
    }
    return 0;
}

static int cmp_snip(const void *a, const void *b) {
    return strcmp(((const ApexSnippet *)a)->id, ((const ApexSnippet *)b)->id);
}

int apex_snippet_load_dir(const char *dir, ApexSnippetList *list) {
    DIR *d = opendir(dir);
    int added = 0;
    if (!d)
        return 0;
    size_t first = list->count;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".asm"))
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        ApexSnippet s;
        if (apex_snippet_load(path, &s, NULL, 0) != 0)
            continue;
        int dup = 0;
        for (size_t i = 0; i < list->count; i++)
            dup = dup || !strcmp(list->items[i].id, s.id);
        if (dup) {
            apex_snippet_free(&s);
            continue;
        }
        list->items = realloc(list->items, (list->count + 1u) * sizeof(*list->items));
        list->items[list->count++] = s;
        added++;
    }
    closedir(d);
    qsort(list->items + first, list->count - first, sizeof(*list->items), cmp_snip);
    return added;
}

size_t apex_snippet_load_default(ApexSnippetList *list, const char *patch_path,
                                 const char *exe_dir) {
    char d[1024];
    if (patch_path) {
        const char *sl = strrchr(patch_path, '/');
        if (sl)
            snprintf(d, sizeof(d), "%.*s/snippets", (int)(sl - patch_path), patch_path);
        else
            snprintf(d, sizeof(d), "snippets");
        apex_snippet_load_dir(d, list);
    }
    const char *env = getenv("APEX_SNIPPETS");
    if (env && *env)
        apex_snippet_load_dir(env, list);
    apex_snippet_load_dir("snippets", list);
    if (exe_dir && *exe_dir) {
        size_t n = strlen(exe_dir);
        int sep = exe_dir[n - 1] == '/' || exe_dir[n - 1] == '\\';
        snprintf(d, sizeof(d), "%s%s../snippets", exe_dir, sep ? "" : "/");
        apex_snippet_load_dir(d, list);
        snprintf(d, sizeof(d), "%s%ssnippets", exe_dir, sep ? "" : "/");
        apex_snippet_load_dir(d, list);
    }
    return list->count;
}

const ApexSnippet *apex_snippet_find(const ApexSnippetList *list, const char *id) {
    for (size_t i = 0; i < list->count; i++) {
        if (!strcmp(list->items[i].id, id))
            return &list->items[i];
    }
    return NULL;
}

void apex_snippet_free(ApexSnippet *s) {
    free(s->id);
    free(s->path);
    free(s->title);
    free(s->about);
    free(s->type);
    for (size_t i = 0; i < s->param_count; i++) {
        free(s->params[i].name);
        free(s->params[i].desc);
        free(s->params[i].def);
    }
    free(s->params);
    free(s->body);
    memset(s, 0, sizeof(*s));
}

void apex_snippet_list_free(ApexSnippetList *list) {
    for (size_t i = 0; i < list->count; i++)
        apex_snippet_free(&list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

char *apex_snippet_expand(const ApexSnippet *s, const char *const *values, char *err,
                          size_t errsz) {
    size_t cap = strlen(s->body) * 2u + 64u, n = 0;
    char *out = xmalloc(cap);
    for (const char *p = s->body; *p;) {
        const char *rep = NULL;
        size_t skip = 1;
        if (*p == '{') {
            const char *close = strchr(p, '}');
            size_t nl = close ? (size_t)(close - p - 1) : 0;
            size_t i;
            for (i = 0; close && i < s->param_count; i++) {
                if (strlen(s->params[i].name) == nl && !strncmp(p + 1, s->params[i].name, nl))
                    break;
            }
            if (close && i < s->param_count) {
                rep = values && values[i] && values[i][0] ? values[i] : s->params[i].def;
                if (!rep) {
                    snip_err(err, errsz, "snippet %s: %s needs a value (%s)", s->id,
                             s->params[i].name, s->params[i].desc);
                    free(out);
                    return NULL;
                }
                skip = nl + 2u;
            } else if (close && nl && nl < 32) {
                int ident = 1;
                for (size_t k = 1; k <= nl; k++) {
                    char c = p[k];
                    ident = ident && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
                }
                if (ident) {
                    snip_err(err, errsz, "snippet %s: unknown parameter {%.*s}", s->id, (int)nl,
                             p + 1);
                    free(out);
                    return NULL;
                }
            }
        }
        size_t rl = rep ? strlen(rep) : 1u;
        if (n + rl + 1u > cap) {
            cap = (n + rl + 1u) * 2u;
            out = realloc(out, cap);
        }
        if (rep) {
            memcpy(out + n, rep, rl);
        } else {
            out[n] = *p;
        }
        n += rl;
        p += skip;
    }
    out[n] = '\0';
    return out;
}

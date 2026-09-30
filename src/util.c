#include "apex.h"

#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void (*apex_die_hook)(const char *msg) = NULL;

/* apex_try() state; thread-local so a die() on the GUI's render thread never
   jumps into a guard set up on the UI thread. */
static APEX_THREAD_LOCAL jmp_buf *s_try_jmp;
static APEX_THREAD_LOCAL char *s_try_err;
static APEX_THREAD_LOCAL size_t s_try_errsz;
static APEX_THREAD_LOCAL const char *s_die_path;
static APEX_THREAD_LOCAL unsigned long s_die_line;

void apex_set_die_context(const char *path, unsigned long line) {
    s_die_path = path;
    s_die_line = line;
}

void apex_get_die_context(const char **path, unsigned long *line) {
    *path = s_die_path;
    *line = s_die_line;
}

int apex_try(void (*fn)(void *), void *ctx, char *err, size_t errsz) {
    jmp_buf jb;
    jmp_buf *prev_jmp = s_try_jmp;
    char *prev_err = s_try_err;
    size_t prev_errsz = s_try_errsz;
    const char *prev_path = s_die_path;
    unsigned long prev_line = s_die_line;

    if (err && errsz) {
        err[0] = '\0';
    }
    if (setjmp(jb)) {
        s_try_jmp = prev_jmp;
        s_try_err = prev_err;
        s_try_errsz = prev_errsz;
        s_die_path = prev_path; /* a parser that died did not restore it */
        s_die_line = prev_line;
        return 1;
    }
    s_try_jmp = &jb;
    s_try_err = err;
    s_try_errsz = errsz;
    fn(ctx);
    s_try_jmp = prev_jmp;
    s_try_err = prev_err;
    s_try_errsz = prev_errsz;
    return 0;
}

void die(const char *fmt, ...) {
    char text[512];
    char msg[640];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (s_die_path) {
        snprintf(msg, sizeof(msg), "%s:%lu: %s", s_die_path, s_die_line, text);
    } else {
        snprintf(msg, sizeof(msg), "%s", text);
    }
    if (s_try_jmp) {
        if (s_try_err && s_try_errsz) {
            snprintf(s_try_err, s_try_errsz, "%s", msg);
        }
        longjmp(*s_try_jmp, 1);
    }
    if (apex_die_hook) {
        apex_die_hook(msg);
        exit(1); /* hook should longjmp; this is a safety fallback */
    }
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}

int apex_parse_int_arg(const char *text, long lo, long hi, long *out) {
    char *end;
    long v;

    if (!text || !*text) {
        return 0;
    }
    errno = 0;
    v = strtol(text, &end, 0);
    if (errno != 0 || *end != '\0' || v < lo || v > hi) {
        return 0;
    }
    *out = v;
    return 1;
}

const char *apex_path_basename(const char *path) {
    const char *base = path;
    const char *p;

    if (!path) {
        return "";
    }
    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\' || (*p == ':' && p == path + 1)) {
            base = p + 1;
        }
    }
    return base;
}

int apex_path_is_absolute(const char *path) {
    if (!path || !*path) {
        return 0;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return 1;
    }
    /* "C:\..." / "C:/..." */
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

void *xmalloc(size_t size) {
    void *p = malloc(size == 0 ? 1 : size);
    if (!p) {
        die("out of memory");
    }
    return p;
}

Buffer read_file(const char *path) {
    FILE *f;
    long len;
    Buffer b;

    f = fopen(path, "rb");
    if (!f) {
        die("failed to open %s: %s", path, strerror(errno));
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        die("failed to seek %s", path);
    }
    len = ftell(f);
    if (len < 0) {
        die("failed to size %s", path);
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        die("failed to rewind %s", path);
    }

    b.size = (size_t)len;
    b.data = xmalloc(b.size);
    if (b.size && fread(b.data, 1, b.size, f) != b.size) {
        die("failed to read %s", path);
    }
    fclose(f);
    return b;
}

void write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        die("failed to open %s for writing: %s", path, strerror(errno));
    }
    if (size && fwrite(data, 1, size, f) != size) {
        die("failed to write %s", path);
    }
    if (fclose(f) != 0) {
        die("failed to close %s: %s", path, strerror(errno));
    }
}

int parse_u32(const char *text, uint32_t *out) {
    char *end;
    unsigned long value;
    int base = 10;

    errno = 0;
    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (text[0] == '$') {
        text++;
        base = 16;
    } else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
    }
    value = strtoul(text, &end, base);
    if (errno != 0 || end == text) {
        return 0;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return 0;
    }
    if (value > UINT32_MAX) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

char *trim(char *s) {
    char *end;

    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    return s;
}

#include "apex.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *xmalloc(size_t size)
{
    void *p = malloc(size == 0 ? 1 : size);
    if (!p) {
        die("out of memory");
    }
    return p;
}

Buffer read_file(const char *path)
{
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

void write_file(const char *path, const uint8_t *data, size_t size)
{
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

int parse_u32(const char *text, uint32_t *out)
{
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

char *trim(char *s)
{
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

#include "apex_ips.h"

#include "apex_rominfo.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *b;
    size_t n, cap;
} Out;

static void out_put(Out *o, const void *src, size_t n) {
    if (o->n + n > o->cap) {
        size_t nc = o->cap ? o->cap : 1024u;
        while (nc < o->n + n)
            nc *= 2u;
        uint8_t *nb = realloc(o->b, nc);
        if (!nb)
            abort();
        o->b = nb;
        o->cap = nc;
    }
    memcpy(o->b + o->n, src, n);
    o->n += n;
}

static void out_byte(Out *o, uint8_t v) {
    out_put(o, &v, 1);
}

static void set_err(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    if (!err || !errsz)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* Changed runs [start, end), joining runs separated by fewer than `gap`
   unchanged bytes (a new record costs more than repeating them). */
static int next_run(const uint8_t *a, const uint8_t *b, size_t size, size_t from, size_t gap,
                    size_t *start, size_t *end) {
    size_t i = from;
    while (i < size && a[i] == b[i])
        i++;
    if (i >= size)
        return 0;
    *start = i;
    size_t last = i;
    for (; i < size; i++) {
        if (a[i] != b[i])
            last = i;
        else if (i - last >= gap)
            break;
    }
    *end = last + 1u;
    return 1;
}

/* ── IPS ───────────────────────────────────────────────────────────────── */

uint8_t *apex_ips_make(const uint8_t *orig, const uint8_t *patched, size_t size, size_t *len) {
    Out o = {0};
    size_t s, e, pos = 0;
    if (size > 0xffffffu)
        return NULL; /* 24-bit offsets */
    out_put(&o, "PATCH", 5);
    while (next_run(orig, patched, size, pos, 6u, &s, &e)) {
        pos = e;
        while (s < e) {
            /* an offset that spells "EOF" would end the patch: start a byte
               earlier (rewriting an unchanged byte is harmless) */
            if (s == 0x454f46u)
                s--;
            size_t n = e - s > 0xffffu ? 0xffffu : e - s;
            uint8_t h[5] = {(uint8_t)(s >> 16), (uint8_t)(s >> 8), (uint8_t)s, (uint8_t)(n >> 8),
                            (uint8_t)n};
            out_put(&o, h, 5);
            out_put(&o, patched + s, n);
            s += n;
        }
    }
    out_put(&o, "EOF", 3);
    *len = o.n;
    return o.b;
}

static uint8_t *ips_apply(const uint8_t *src, size_t src_size, const uint8_t *p, size_t n,
                          size_t *out_size, char *err, size_t errsz) {
    size_t size = src_size, pos = 5;
    uint8_t *img = malloc(size ? size : 1u);
    if (!img)
        return NULL;
    memcpy(img, src, src_size);
    for (;;) {
        if (pos + 3 > n) {
            set_err(err, errsz, "IPS: no EOF marker");
            free(img);
            return NULL;
        }
        if (!memcmp(p + pos, "EOF", 3))
            break;
        if (pos + 5 > n) {
            set_err(err, errsz, "IPS: truncated record");
            free(img);
            return NULL;
        }
        size_t off = ((size_t)p[pos] << 16) | ((size_t)p[pos + 1] << 8) | p[pos + 2];
        size_t len = ((size_t)p[pos + 3] << 8) | p[pos + 4];
        pos += 5;
        size_t rle = 0;
        uint8_t fill = 0;
        if (!len) { /* RLE record: 2-byte count, 1 byte value */
            if (pos + 3 > n) {
                set_err(err, errsz, "IPS: truncated RLE record");
                free(img);
                return NULL;
            }
            rle = ((size_t)p[pos] << 8) | p[pos + 1];
            fill = p[pos + 2];
            pos += 3;
            len = rle;
        } else if (pos + len > n) {
            set_err(err, errsz, "IPS: truncated record data");
            free(img);
            return NULL;
        }
        if (off + len > size) {
            uint8_t *ni = realloc(img, off + len);
            if (!ni) {
                free(img);
                return NULL;
            }
            memset(ni + size, 0, off + len - size);
            img = ni;
            size = off + len;
        }
        if (rle) {
            memset(img + off, fill, len);
        } else {
            memcpy(img + off, p + pos, len);
            pos += len;
        }
    }
    *out_size = size;
    return img;
}

/* ── BPS ───────────────────────────────────────────────────────────────── */

static void bps_num(Out *o, uint64_t v) {
    for (;;) {
        uint8_t x = (uint8_t)(v & 0x7fu);
        v >>= 7;
        if (!v) {
            out_byte(o, (uint8_t)(0x80u | x));
            return;
        }
        out_byte(o, x);
        v--;
    }
}

static void put_le32(Out *o, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    out_put(o, b, 4);
}

enum { BPS_SOURCE_READ = 0, BPS_TARGET_READ, BPS_SOURCE_COPY, BPS_TARGET_COPY };

uint8_t *apex_bps_make(const uint8_t *orig, const uint8_t *patched, size_t size,
                       const char *metadata, size_t *len) {
    Out o = {0};
    size_t s, e, pos = 0, ml = metadata ? strlen(metadata) : 0;
    out_put(&o, "BPS1", 4);
    bps_num(&o, size);
    bps_num(&o, size);
    bps_num(&o, ml);
    if (ml)
        out_put(&o, metadata, ml);
    while (next_run(orig, patched, size, pos, 3u, &s, &e)) {
        if (s > pos)
            bps_num(&o, ((uint64_t)(s - pos - 1u) << 2) | BPS_SOURCE_READ);
        bps_num(&o, ((uint64_t)(e - s - 1u) << 2) | BPS_TARGET_READ);
        out_put(&o, patched + s, e - s);
        pos = e;
    }
    if (pos < size)
        bps_num(&o, ((uint64_t)(size - pos - 1u) << 2) | BPS_SOURCE_READ);
    put_le32(&o, ari_crc32(orig, size));
    put_le32(&o, ari_crc32(patched, size));
    put_le32(&o, ari_crc32(o.b, o.n));
    *len = o.n;
    return o.b;
}

static int bps_read(const uint8_t *p, size_t n, size_t *pos, uint64_t *v) {
    uint64_t data = 0, shift = 1;
    for (;;) {
        if (*pos >= n || shift > (1ull << 56))
            return 0;
        uint8_t x = p[(*pos)++];
        data += (x & 0x7fu) * shift;
        if (x & 0x80u)
            break;
        shift <<= 7;
        data += shift;
    }
    *v = data;
    return 1;
}

static uint32_t le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint8_t *bps_apply(const uint8_t *src, size_t src_size, const uint8_t *p, size_t n,
                          size_t *out_size, char *err, size_t errsz) {
    size_t pos = 4;
    uint64_t ssize, tsize, msize;
    if (n < 4 + 12 || !bps_read(p, n, &pos, &ssize) || !bps_read(p, n, &pos, &tsize) ||
        !bps_read(p, n, &pos, &msize) || msize > n - pos) {
        set_err(err, errsz, "BPS: bad header");
        return NULL;
    }
    size_t end = n - 12;
    if (le32(p + end + 8) != ari_crc32(p, n - 4)) {
        set_err(err, errsz, "BPS: the patch file is damaged (CRC mismatch)");
        return NULL;
    }
    if (ssize != src_size || le32(p + end) != ari_crc32(src, src_size)) {
        set_err(err, errsz, "BPS: this patch is for another ROM (source CRC-32 %08X)",
                (unsigned)le32(p + end));
        return NULL;
    }
    if (tsize > (64u << 20)) {
        set_err(err, errsz, "BPS: target too large");
        return NULL;
    }
    pos += (size_t)msize;
    uint8_t *t = calloc((size_t)tsize ? (size_t)tsize : 1u, 1);
    if (!t)
        return NULL;
    size_t out = 0, src_rel = 0, tgt_rel = 0;
    while (pos < end) {
        uint64_t a;
        if (!bps_read(p, end, &pos, &a))
            goto bad;
        size_t len = (size_t)(a >> 2) + 1u;
        if (out + len > tsize)
            goto bad;
        switch ((int)(a & 3u)) {
        case BPS_SOURCE_READ:
            if (out + len > src_size)
                goto bad;
            memcpy(t + out, src + out, len);
            break;
        case BPS_TARGET_READ:
            if (pos + len > end)
                goto bad;
            memcpy(t + out, p + pos, len);
            pos += len;
            break;
        case BPS_SOURCE_COPY:
        case BPS_TARGET_COPY: {
            uint64_t d;
            if (!bps_read(p, end, &pos, &d))
                goto bad;
            long long delta = (long long)(d >> 1) * ((d & 1u) ? -1 : 1);
            size_t *rel = (a & 3u) == BPS_SOURCE_COPY ? &src_rel : &tgt_rel;
            long long np = (long long)*rel + delta;
            if (np < 0)
                goto bad;
            *rel = (size_t)np;
            if ((a & 3u) == BPS_SOURCE_COPY) {
                if (*rel + len > src_size)
                    goto bad;
                memcpy(t + out, src + *rel, len);
            } else {
                if (*rel >= out)
                    goto bad;
                for (size_t k = 0; k < len; k++) /* may overlap: byte by byte */
                    t[out + k] = t[*rel + k];
            }
            *rel += len;
            break;
        }
        }
        out += len;
    }
    if (out != tsize || le32(p + end + 4) != ari_crc32(t, (size_t)tsize)) {
        set_err(err, errsz, "BPS: the result does not match the target CRC-32");
        free(t);
        return NULL;
    }
    *out_size = (size_t)tsize;
    return t;
bad:
    set_err(err, errsz, "BPS: corrupt action stream");
    free(t);
    return NULL;
}

uint8_t *apex_patchfile_apply(const uint8_t *src, size_t src_size, const uint8_t *patch,
                              size_t patch_len, size_t *out_size, char *err, size_t errsz) {
    if (patch_len >= 8 && !memcmp(patch, "PATCH", 5))
        return ips_apply(src, src_size, patch, patch_len, out_size, err, errsz);
    if (patch_len >= 4 && !memcmp(patch, "BPS1", 4))
        return bps_apply(src, src_size, patch, patch_len, out_size, err, errsz);
    set_err(err, errsz, "not an IPS or BPS patch");
    return NULL;
}

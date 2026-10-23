#include "apex_png.h"

#include "apex.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void img_err(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    if (!err || !errsz)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* ── inflate (RFC 1951) ────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *src;
    size_t len, pos;
    unsigned bitbuf, bitcnt;
    int bad;
    uint8_t *out;
    size_t out_len, out_cap;
} Inflate;

static unsigned getbits(Inflate *s, unsigned n) {
    unsigned v = 0;
    while (s->bitcnt < n) {
        if (s->pos >= s->len) {
            s->bad = 1;
            return 0;
        }
        s->bitbuf |= (unsigned)s->src[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    v = s->bitbuf & ((1u << n) - 1u);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static void put(Inflate *s, uint8_t b) {
    if (s->out_len == s->out_cap) {
        size_t nc = s->out_cap ? s->out_cap * 2u : 65536u;
        uint8_t *n = realloc(s->out, nc);
        if (!n) {
            s->bad = 1;
            return;
        }
        s->out = n;
        s->out_cap = nc;
    }
    s->out[s->out_len++] = b;
}

/* Canonical Huffman code: counts per length and symbols in code order. */
typedef struct {
    uint16_t count[16];
    uint16_t symbol[320];
} Huff;

static int huff_build(Huff *h, const uint8_t *lengths, unsigned n) {
    uint16_t offs[16];
    unsigned i;
    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (i = 1; i < 15; i++)
        offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++) {
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    }
    return 1;
}

static int huff_decode(Inflate *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)getbits(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->bad)
            return -1;
    }
    s->bad = 1;
    return -1;
}

static const uint16_t k_len_base[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                        15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                        67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t k_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t k_dist_base[30] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void inflate_codes(Inflate *s, const Huff *lit, const Huff *dist) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (s->bad || sym < 0)
            return;
        if (sym < 256) {
            put(s, (uint8_t)sym);
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) {
                s->bad = 1;
                return;
            }
            unsigned len = k_len_base[sym] + getbits(s, k_len_extra[sym]);
            int ds = huff_decode(s, dist);
            if (s->bad || ds < 0 || ds >= 30) {
                s->bad = 1;
                return;
            }
            size_t d = k_dist_base[ds] + getbits(s, k_dist_extra[ds]);
            if (d > s->out_len) {
                s->bad = 1;
                return;
            }
            while (len--)
                put(s, s->out[s->out_len - d]);
        }
    }
}

int apex_inflate_zlib(const uint8_t *src, size_t len, uint8_t **out, size_t *out_len) {
    Inflate s;
    memset(&s, 0, sizeof(s));
    if (len < 2 || (src[0] & 0x0f) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20))
        return 0;
    s.src = src;
    s.len = len;
    s.pos = 2;
    int last;
    do {
        last = (int)getbits(&s, 1);
        unsigned type = getbits(&s, 2);
        if (s.bad)
            break;
        if (type == 0) { /* stored */
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.pos + 4 > s.len) {
                s.bad = 1;
                break;
            }
            unsigned n = s.src[s.pos] | (s.src[s.pos + 1] << 8);
            s.pos += 4;
            if (s.pos + n > s.len) {
                s.bad = 1;
                break;
            }
            while (n--)
                put(&s, s.src[s.pos++]);
        } else if (type == 1) { /* fixed codes */
            uint8_t l[288 + 30];
            Huff lit, dist;
            unsigned i;
            for (i = 0; i < 144; i++)
                l[i] = 8;
            for (; i < 256; i++)
                l[i] = 9;
            for (; i < 280; i++)
                l[i] = 7;
            for (; i < 288; i++)
                l[i] = 8;
            huff_build(&lit, l, 288);
            for (i = 0; i < 30; i++)
                l[i] = 5;
            huff_build(&dist, l, 30);
            inflate_codes(&s, &lit, &dist);
        } else if (type == 2) { /* dynamic codes */
            static const uint8_t order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                              11, 4,  12, 3, 13, 2, 14, 1, 15};
            unsigned nlen = getbits(&s, 5) + 257, ndist = getbits(&s, 5) + 1,
                     ncode = getbits(&s, 4) + 4, i;
            uint8_t l[320];
            Huff lencode, lit, dist;
            memset(l, 0, sizeof(l));
            for (i = 0; i < ncode; i++)
                l[order[i]] = (uint8_t)getbits(&s, 3);
            huff_build(&lencode, l, 19);
            memset(l, 0, sizeof(l));
            for (i = 0; i < nlen + ndist && !s.bad;) {
                int sym = huff_decode(&s, &lencode);
                if (sym < 0)
                    break;
                if (sym < 16) {
                    l[i++] = (uint8_t)sym;
                } else {
                    unsigned rep = 0;
                    uint8_t v = 0;
                    if (sym == 16) {
                        if (i == 0) {
                            s.bad = 1;
                            break;
                        }
                        v = l[i - 1];
                        rep = 3 + getbits(&s, 2);
                    } else if (sym == 17) {
                        rep = 3 + getbits(&s, 3);
                    } else {
                        rep = 11 + getbits(&s, 7);
                    }
                    if (i + rep > nlen + ndist) {
                        s.bad = 1;
                        break;
                    }
                    while (rep--)
                        l[i++] = v;
                }
            }
            if (s.bad)
                break;
            huff_build(&lit, l, nlen);
            huff_build(&dist, l + nlen, ndist);
            inflate_codes(&s, &lit, &dist);
        } else {
            s.bad = 1;
        }
    } while (!last && !s.bad);
    if (s.bad) {
        free(s.out);
        return 0;
    }
    *out = s.out;
    *out_len = s.out_len;
    return 1;
}

/* ── PNG ───────────────────────────────────────────────────────────────── */

static uint32_t be32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

static int read_png(const uint8_t *d, size_t n, uint8_t **pixels, unsigned *w, unsigned *h,
                    char *err, size_t errsz) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    uint8_t pal[256][4];
    unsigned npal = 0, width = 0, height = 0, depth = 0, ctype = 0;
    uint8_t *idat = NULL;
    size_t idat_len = 0, pos = 8;
    memset(pal, 0xff, sizeof(pal));
    if (n < 8 || memcmp(d, sig, 8)) {
        img_err(err, errsz, "not a PNG file");
        return 0;
    }
    while (pos + 12 <= n) {
        uint32_t len = be32(d + pos);
        const uint8_t *type = d + pos + 4, *body = d + pos + 8;
        if (pos + 12 + len > n)
            break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            width = be32(body);
            height = be32(body + 4);
            depth = body[8];
            ctype = body[9];
            if (body[12]) {
                img_err(err, errsz, "interlaced PNGs are not supported");
                free(idat);
                return 0;
            }
        } else if (!memcmp(type, "PLTE", 4)) {
            for (npal = 0; npal < 256 && npal * 3 + 2 < len; npal++) {
                pal[npal][0] = body[npal * 3];
                pal[npal][1] = body[npal * 3 + 1];
                pal[npal][2] = body[npal * 3 + 2];
            }
        } else if (!memcmp(type, "tRNS", 4) && ctype == 3) {
            for (uint32_t k = 0; k < len && k < 256; k++)
                pal[k][3] = body[k];
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *ni = realloc(idat, idat_len + len + 1);
            if (!ni) {
                free(idat);
                return 0;
            }
            idat = ni;
            memcpy(idat + idat_len, body, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    static const unsigned chans_of[7] = {1, 0, 3, 1, 2, 0, 4};
    unsigned chans = ctype <= 6 ? chans_of[ctype] : 0;
    if (!width || !height || !chans || !idat ||
        (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) {
        img_err(err, errsz, "unsupported PNG (colour type %u, depth %u)", ctype, depth);
        free(idat);
        return 0;
    }
    uint8_t *raw;
    size_t raw_len;
    int ok = apex_inflate_zlib(idat, idat_len, &raw, &raw_len);
    free(idat);
    size_t bpp_bits = (size_t)chans * depth, stride = (width * bpp_bits + 7) / 8;
    size_t bpp = (bpp_bits + 7) / 8;
    if (!ok || raw_len < (stride + 1) * height) {
        img_err(err, errsz, "corrupt PNG image data");
        if (ok)
            free(raw);
        return 0;
    }
    /* undo the per-row filters in place */
    for (unsigned y = 0; y < height; y++) {
        uint8_t *row = raw + y * (stride + 1) + 1;
        const uint8_t *up = y ? raw + (y - 1) * (stride + 1) + 1 : NULL;
        uint8_t f = row[-1];
        for (size_t x = 0; x < stride; x++) {
            int a = x >= bpp ? row[x - bpp] : 0, b = up ? up[x] : 0,
                c = (up && x >= bpp) ? up[x - bpp] : 0;
            switch (f) {
            case 1:
                row[x] = (uint8_t)(row[x] + a);
                break;
            case 2:
                row[x] = (uint8_t)(row[x] + b);
                break;
            case 3:
                row[x] = (uint8_t)(row[x] + ((a + b) >> 1));
                break;
            case 4:
                row[x] = (uint8_t)(row[x] + paeth(a, b, c));
                break;
            default:
                break;
            }
        }
    }
    uint8_t *px = xmalloc((size_t)width * height);
    unsigned maxv = (1u << (depth > 8 ? 8 : depth)) - 1u;
    for (unsigned y = 0; y < height; y++) {
        const uint8_t *row = raw + y * (stride + 1) + 1;
        for (unsigned x = 0; x < width; x++) {
            unsigned s[4] = {0, 0, 0, 255};
            for (unsigned ch = 0; ch < chans; ch++) {
                size_t bit = ((size_t)x * chans + ch) * depth;
                unsigned v;
                if (depth == 16)
                    v = row[bit / 8];
                else if (depth == 8)
                    v = row[bit / 8];
                else
                    v = (row[bit / 8] >> (8 - depth - bit % 8)) & maxv;
                s[ch] = v;
            }
            unsigned r, g, b, a = 255;
            if (ctype == 3) {
                unsigned i = s[0] < 256 ? s[0] : 0;
                r = pal[i][0];
                g = pal[i][1];
                b = pal[i][2];
                a = pal[i][3];
            } else if (ctype == 0 || ctype == 4) {
                r = g = b = depth < 8 ? s[0] * 255u / maxv : s[0];
                if (ctype == 4)
                    a = s[1];
            } else {
                r = s[0];
                g = s[1];
                b = s[2];
                if (ctype == 6)
                    a = s[3];
            }
            unsigned y8 = (r * 299u + g * 587u + b * 114u) / 1000u;
            px[(size_t)y * width + x] = (uint8_t)(y8 * a / 255u);
        }
    }
    free(raw);
    *pixels = px;
    *w = width;
    *h = height;
    return 1;
}

/* ── netpbm (P4 / P5) ──────────────────────────────────────────────────── */

static int pnm_int(const uint8_t *d, size_t n, size_t *pos, unsigned *v) {
    while (*pos < n && (d[*pos] == ' ' || d[*pos] == '\n' || d[*pos] == '\r' || d[*pos] == '\t' ||
                        d[*pos] == '#')) {
        if (d[*pos] == '#') {
            while (*pos < n && d[*pos] != '\n')
                (*pos)++;
        } else {
            (*pos)++;
        }
    }
    if (*pos >= n || d[*pos] < '0' || d[*pos] > '9')
        return 0;
    *v = 0;
    while (*pos < n && d[*pos] >= '0' && d[*pos] <= '9')
        *v = *v * 10u + (unsigned)(d[(*pos)++] - '0');
    return 1;
}

static int read_pnm(const uint8_t *d, size_t n, uint8_t **pixels, unsigned *w, unsigned *h,
                    char *err, size_t errsz) {
    size_t pos = 2;
    unsigned width, height, maxv = 1;
    int pbm = d[1] == '4';
    if (!pnm_int(d, n, &pos, &width) || !pnm_int(d, n, &pos, &height) ||
        (!pbm && !pnm_int(d, n, &pos, &maxv)) || !width || !height || !maxv || maxv > 255) {
        img_err(err, errsz, "bad PBM/PGM header");
        return 0;
    }
    pos++; /* the single whitespace before the data */
    size_t need = pbm ? ((width + 7) / 8) * height : (size_t)width * height;
    if (pos + need > n) {
        img_err(err, errsz, "PBM/PGM data is short");
        return 0;
    }
    uint8_t *px = xmalloc((size_t)width * height);
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            if (pbm) /* 1 = a lit dot, as apexdmd writes DMD planes */
                px[(size_t)y * width + x] =
                    (d[pos + y * ((width + 7) / 8) + x / 8] & (0x80 >> (x % 8))) ? 255 : 0;
            else
                px[(size_t)y * width + x] = (uint8_t)(d[pos + (size_t)y * width + x] * 255u / maxv);
        }
    }
    *pixels = px;
    *w = width;
    *h = height;
    return 1;
}

int apex_image_read_gray(const char *path, uint8_t **pixels, unsigned *w, unsigned *h, char *err,
                         size_t errsz) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        img_err(err, errsz, "cannot open %s", path);
        return 0;
    }
    fclose(f);
    Buffer b = read_file(path);
    int ok = 0;
    if (b.size >= 8 && b.data[0] == 0x89 && b.data[1] == 'P')
        ok = read_png(b.data, b.size, pixels, w, h, err, errsz);
    else if (b.size > 3 && b.data[0] == 'P' && (b.data[1] == '4' || b.data[1] == '5'))
        ok = read_pnm(b.data, b.size, pixels, w, h, err, errsz);
    else
        img_err(err, errsz, "%s: not a PNG, PBM or PGM image", path);
    free(b.data);
    return ok;
}

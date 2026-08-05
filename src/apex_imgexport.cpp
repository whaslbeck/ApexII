#include "apex_imgexport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* ---- checksums ---- */

static uint32_t crc32_bytes(const uint8_t *d, size_t n, uint32_t crc)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static uint32_t adler32_bytes(const uint8_t *d, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + d[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* ---- PNG (8-bit RGB, single uncompressed zlib stored block) ---- */

static void push_be32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

static void png_chunk(std::vector<uint8_t> &out, const char *type,
                      const uint8_t *data, size_t n)
{
    push_be32(out, (uint32_t)n);
    size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    if (n) out.insert(out.end(), data, data + n);
    uint32_t crc = crc32_bytes(out.data() + crc_start, out.size() - crc_start, 0);
    push_be32(out, crc);
}

bool apex_png_write_rgb(const char *path, int w, int h, const uint8_t *rgb)
{
    if (w <= 0 || h <= 0 || !rgb) return false;

    std::vector<uint8_t> out;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    out.insert(out.end(), sig, sig + 8);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;   /* bit depth */
    ihdr[9] = 2;   /* colour type: RGB */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(out, "IHDR", ihdr, sizeof(ihdr));

    /* Raw (filtered) scanlines: filter byte 0 + RGB row. */
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 3));
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3);
    }

    /* zlib stream: header + DEFLATE stored blocks (<=65535 each) + adler32. */
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        size_t chunk = raw.size() - off;
        if (chunk > 65535) chunk = 65535;
        z.push_back((off + chunk >= raw.size()) ? 1 : 0); /* BFINAL, BTYPE=00 */
        z.push_back((uint8_t)(chunk & 0xff)); z.push_back((uint8_t)(chunk >> 8));
        uint16_t nlen = (uint16_t)~chunk;
        z.push_back((uint8_t)(nlen & 0xff)); z.push_back((uint8_t)(nlen >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + chunk);
        off += chunk;
    }
    push_be32(z, adler32_bytes(raw.data(), raw.size()));
    png_chunk(out, "IDAT", z.data(), z.size());
    png_chunk(out, "IEND", nullptr, 0);

    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

/* ---- animated GIF (indexed frames, minimal LZW = literals + clear) ---- */

struct ApexGif {
    FILE *f = nullptr;
    int   w = 0, h = 0;
    /* LZW bit accumulator + current sub-block */
    uint32_t bit_acc = 0;
    int      bit_cnt = 0;
    uint8_t  sub[255];
    int      sub_len = 0;
};

static void gif_le16(FILE *f, int v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }

static void gif_flush_subblock(ApexGif *g)
{
    if (g->sub_len > 0) {
        fputc(g->sub_len, g->f);
        fwrite(g->sub, 1, (size_t)g->sub_len, g->f);
        g->sub_len = 0;
    }
}

static void gif_put_bits(ApexGif *g, int code, int n)
{
    g->bit_acc |= (uint32_t)code << g->bit_cnt;
    g->bit_cnt += n;
    while (g->bit_cnt >= 8) {
        g->sub[g->sub_len++] = (uint8_t)(g->bit_acc & 0xff);
        g->bit_acc >>= 8;
        g->bit_cnt -= 8;
        if (g->sub_len == 255) gif_flush_subblock(g);
    }
}

ApexGif *apex_gif_begin(const char *path, int w, int h, const uint8_t *pal)
{
    if (w <= 0 || h <= 0 || !pal) return nullptr;
    FILE *f = fopen(path, "wb");
    if (!f) return nullptr;
    ApexGif *g = new ApexGif();
    g->f = f; g->w = w; g->h = h;

    fwrite("GIF89a", 1, 6, f);
    gif_le16(f, w); gif_le16(f, h);
    fputc(0xF7, f);          /* GCT present, 8-bit colour, GCT size = 256 */
    fputc(0, f);             /* background colour index */
    fputc(0, f);             /* pixel aspect ratio */
    fwrite(pal, 1, 256 * 3, f);
    /* Netscape loop-forever extension */
    fputc(0x21, f); fputc(0xFF, f); fputc(0x0B, f);
    fwrite("NETSCAPE2.0", 1, 11, f);
    fputc(0x03, f); fputc(0x01, f); gif_le16(f, 0); fputc(0x00, f);
    return g;
}

bool apex_gif_add_frame(ApexGif *g, const uint8_t *idx, int delay_cs)
{
    if (!g || !idx) return false;
    FILE *f = g->f;
    if (delay_cs < 2) delay_cs = 2;

    /* Graphic Control Extension (delay) */
    fputc(0x21, f); fputc(0xF9, f); fputc(0x04, f);
    fputc(0x00, f);                 /* no disposal / transparency */
    gif_le16(f, delay_cs);
    fputc(0x00, f); fputc(0x00, f);

    /* Image Descriptor (full frame, no local colour table) */
    fputc(0x2C, f);
    gif_le16(f, 0); gif_le16(f, 0);
    gif_le16(f, g->w); gif_le16(f, g->h);
    fputc(0x00, f);

    const int min_code = 8;
    const int clear = 1 << min_code;   /* 256 */
    const int eoi = clear + 1;         /* 257 */
    fputc(min_code, f);

    g->bit_acc = 0; g->bit_cnt = 0; g->sub_len = 0;
    int code_size = min_code + 1;      /* 9 */
    int next_code = clear + 2;         /* 258 */
    gif_put_bits(g, clear, code_size);
    bool first = true;                 /* decoder adds NO dict entry for the first
                                          code after a clear — mirror that here, or
                                          the code-size growth desyncs. */
    size_t n = (size_t)g->w * g->h;
    for (size_t i = 0; i < n; i++) {
        gif_put_bits(g, idx[i], code_size);   /* every pixel is a literal */
        if (first) {
            first = false;
        } else {
            next_code++;
            if (next_code == (1 << code_size)) { /* mirror the decoder's growth */
                if (code_size < 12) {
                    code_size++;
                } else {
                    gif_put_bits(g, clear, code_size);
                    code_size = min_code + 1;
                    next_code = clear + 2;
                    first = true;
                }
            }
        }
    }
    gif_put_bits(g, eoi, code_size);
    if (g->bit_cnt > 0) {              /* flush trailing bits */
        g->sub[g->sub_len++] = (uint8_t)(g->bit_acc & 0xff);
        g->bit_acc = 0; g->bit_cnt = 0;
        if (g->sub_len == 255) gif_flush_subblock(g);
    }
    gif_flush_subblock(g);
    fputc(0x00, f);                   /* image data block terminator */
    return true;
}

bool apex_gif_end(ApexGif *g)
{
    if (!g) return false;
    fputc(0x3B, g->f);                /* trailer */
    bool ok = fclose(g->f) == 0;
    delete g;
    return ok;
}

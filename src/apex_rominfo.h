#ifndef APEX_ROMINFO_H
#define APEX_ROMINFO_H

/*
 * WPC ROM metadata: OS version, game version string, checksum, hashes.
 * Shared between apexmeta (CLI) and apeximgui (GUI info panel).
 *
 * Include once from a .c/.cpp file that needs it.  All functions are
 * declared static inline to allow inclusion in multiple translation
 * units without linker conflicts.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── WPC ROM layout constants ───────────────────────────────────────────*/
#define APEX_ROMINFO_CSUM_END   18u   /* 0xFFEE: 16-bit checksum  */
#define APEX_ROMINFO_DELTA_END  20u   /* 0xFFEC: fixup/delta word */
#define APEX_ROMINFO_RESET_END   2u   /* 0xFFFE: 6809 reset vec   */
#define APEX_ROMINFO_DISABLE_DELTA 0x00FFu

/* ── Metadata result struct ─────────────────────────────────────────────*/
typedef struct {
    /* OS version (2 bytes before reset routine) */
    int      os_valid;
    uint8_t  os_major;
    uint8_t  os_minor;
    uint32_t reset_addr;

    /* Game version string ("REV. X-N...") */
    char     game_version[32];
    size_t   game_version_offset;

    /* WPC checksum */
    uint16_t stored_csum;
    uint16_t computed_csum;
    uint16_t stored_delta;
    size_t   csum_file_off;
    size_t   delta_file_off;

    /* File hashes */
    uint32_t crc32_val;
    uint8_t  sha1[20];
    uint8_t  sha256[32];
} ApexRomInfo;

/* ── Internal: read/write helpers ──────────────────────────────────────*/
static inline uint16_t ari_r16(const uint8_t *p, size_t o)
{
    return (uint16_t)(((uint16_t)p[o] << 8) | p[o + 1]);
}
static inline void ari_w16(uint8_t *p, size_t o, uint16_t v)
{
    p[o] = (uint8_t)(v >> 8); p[o + 1] = (uint8_t)(v & 0xFF);
}
#define ARI_RB32(p) ((uint32_t)(p)[0]<<24|(uint32_t)(p)[1]<<16|(uint32_t)(p)[2]<<8|(uint32_t)(p)[3])
#define ARI_WB32(p,v) do{(p)[0]=(uint8_t)((v)>>24);(p)[1]=(uint8_t)((v)>>16); \
                         (p)[2]=(uint8_t)((v)>>8); (p)[3]=(uint8_t)(v);}while(0)

/* ── CRC-32 ─────────────────────────────────────────────────────────────*/
static inline uint32_t ari_crc32(const uint8_t *data, size_t len)
{
    static uint32_t tbl[256]; static int init = 0;
    uint32_t c = 0xFFFFFFFFu; size_t i; unsigned j;
    if (!init) {
        for (i = 0; i < 256; i++) {
            uint32_t x = (uint32_t)i;
            for (j = 0; j < 8; j++) x = (x & 1) ? (0xEDB88320u ^ (x >> 1)) : (x >> 1);
            tbl[i] = x;
        }
        init = 1;
    }
    for (i = 0; i < len; i++) c = tbl[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ── SHA-1 ──────────────────────────────────────────────────────────────*/
#define ARI_ROL(x,n) (((x)<<(n))|((x)>>(32-(n))))
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; unsigned blen; } AriSha1;
static inline void ari_sha1_compress(uint32_t h[5], const uint8_t *b) {
    uint32_t w[80],a,bc,c,d,e,tmp,f,k; int i;
    for(i=0;i<16;i++) w[i]=ARI_RB32(b+i*4);
    for(;i<80;i++) w[i]=ARI_ROL(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    a=h[0];bc=h[1];c=h[2];d=h[3];e=h[4];
    for(i=0;i<80;i++){
        if(i<20){f=(bc&c)|(~bc&d);k=0x5A827999u;}
        else if(i<40){f=bc^c^d;k=0x6ED9EBA1u;}
        else if(i<60){f=(bc&c)|(bc&d)|(c&d);k=0x8F1BBCDCu;}
        else{f=bc^c^d;k=0xCA62C1D6u;}
        tmp=ARI_ROL(a,5)+f+e+k+w[i];
        e=d;d=c;c=ARI_ROL(bc,30);bc=a;a=tmp;
    }
    h[0]+=a;h[1]+=bc;h[2]+=c;h[3]+=d;h[4]+=e;
}
static inline void ari_sha1(const uint8_t *data, size_t n, uint8_t out[20]) {
    AriSha1 s; uint64_t l; int i;
    s.h[0]=0x67452301;s.h[1]=0xEFCDAB89;s.h[2]=0x98BADCFE;s.h[3]=0x10325476;s.h[4]=0xC3D2E1F0;
    s.len=(uint64_t)n*8; s.blen=0;
    while(n--){ s.buf[s.blen++]=*data++; if(s.blen==64){ari_sha1_compress(s.h,s.buf);s.blen=0;} }
    l=s.len; s.buf[s.blen++]=0x80;
    if(s.blen>56){while(s.blen<64)s.buf[s.blen++]=0;ari_sha1_compress(s.h,s.buf);s.blen=0;}
    while(s.blen<56)s.buf[s.blen++]=0;
    ARI_WB32(s.buf+56,(uint32_t)(l>>32)); ARI_WB32(s.buf+60,(uint32_t)l);
    ari_sha1_compress(s.h,s.buf);
    for(i=0;i<5;i++) ARI_WB32(out+i*4,s.h[i]);
}

/* ── SHA-256 ─────────────────────────────────────────────────────────────*/
static const uint32_t ari_K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ARI_ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define ARI_EP0(x) (ARI_ROTR(x,2)^ARI_ROTR(x,13)^ARI_ROTR(x,22))
#define ARI_EP1(x) (ARI_ROTR(x,6)^ARI_ROTR(x,11)^ARI_ROTR(x,25))
#define ARI_SG0(x)(ARI_ROTR(x,7)^ARI_ROTR(x,18)^((x)>>3))
#define ARI_SG1(x)(ARI_ROTR(x,17)^ARI_ROTR(x,19)^((x)>>10))
#define ARI_CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define ARI_MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; unsigned blen; } AriSha256;
static inline void ari_sha256_compress(uint32_t h[8], const uint8_t *b) {
    uint32_t w[64],t1,t2,a,bc,c,d,e,f,g,wh; int i;
    for(i=0;i<16;i++) w[i]=ARI_RB32(b+i*4);
    for(;i<64;i++) w[i]=ARI_SG1(w[i-2])+w[i-7]+ARI_SG0(w[i-15])+w[i-16];
    a=h[0];bc=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];wh=h[7];
    for(i=0;i<64;i++){
        t1=wh+ARI_EP1(e)+ARI_CH(e,f,g)+ari_K256[i]+w[i];
        t2=ARI_EP0(a)+ARI_MAJ(a,bc,c);
        wh=g;g=f;f=e;e=d+t1;d=c;c=bc;bc=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=bc;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=wh;
}
static inline void ari_sha256(const uint8_t *data, size_t n, uint8_t out[32]) {
    AriSha256 s; uint64_t l; int i;
    s.h[0]=0x6a09e667;s.h[1]=0xbb67ae85;s.h[2]=0x3c6ef372;s.h[3]=0xa54ff53a;
    s.h[4]=0x510e527f;s.h[5]=0x9b05688c;s.h[6]=0x1f83d9ab;s.h[7]=0x5be0cd19;
    s.len=(uint64_t)n*8; s.blen=0;
    while(n--){ s.buf[s.blen++]=*data++; if(s.blen==64){ari_sha256_compress(s.h,s.buf);s.blen=0;} }
    l=s.len; s.buf[s.blen++]=0x80;
    if(s.blen>56){while(s.blen<64)s.buf[s.blen++]=0;ari_sha256_compress(s.h,s.buf);s.blen=0;}
    while(s.blen<56)s.buf[s.blen++]=0;
    ARI_WB32(s.buf+56,(uint32_t)(l>>32)); ARI_WB32(s.buf+60,(uint32_t)l);
    ari_sha256_compress(s.h,s.buf);
    for(i=0;i<8;i++) ARI_WB32(out+i*4,s.h[i]);
}

/* ── Metadata reader ────────────────────────────────────────────────────*/
static inline void apex_rominfo_compute(const uint8_t *rom, size_t size, ApexRomInfo *info)
{
    size_t i; uint32_t s = 0;
    memset(info, 0, sizeof(*info));
    if (size < 32768u) return;

    info->csum_file_off  = size - APEX_ROMINFO_CSUM_END;
    info->delta_file_off = size - APEX_ROMINFO_DELTA_END;
    info->stored_csum    = ari_r16(rom, info->csum_file_off);
    info->stored_delta   = ari_r16(rom, info->delta_file_off);

    for (i = 0; i < size; i++) s += rom[i];
    info->computed_csum = (uint16_t)(s & 0xFFFF);

    info->reset_addr = ari_r16(rom, size - APEX_ROMINFO_RESET_END);
    if (info->reset_addr >= 0x8002u && info->reset_addr <= 0xFFFFu) {
        size_t roff = (size - 32768u) + (info->reset_addr - 0x8000u);
        if (roff >= 2 && roff <= size) {
            info->os_major = rom[roff - 2];
            info->os_minor = rom[roff - 1];
            info->os_valid = 1;
        }
    }

    for (i = 0; i + 8 < size; i++) {
        if (rom[i]=='R'&&rom[i+1]=='E'&&rom[i+2]=='V'&&rom[i+3]=='.'&&rom[i+4]==' '&&
            rom[i+5]>='A'&&rom[i+5]<='Z'&&rom[i+6]=='-'&&
            rom[i+7]>='0'&&rom[i+7]<='9') {
            size_t j = 0;
            while (j < 31 && i+j < size && rom[i+j] >= 0x20 && rom[i+j] <= 0x7E) {
                info->game_version[j] = (char)rom[i+j]; j++;
            }
            info->game_version[j] = '\0';
            info->game_version_offset = i;
            break;
        }
    }

    info->crc32_val = ari_crc32(rom, size);
    ari_sha1(rom, size, info->sha1);
    ari_sha256(rom, size, info->sha256);
}

/* ── Checksum fix ───────────────────────────────────────────────────────*/
static inline int apex_rominfo_fix_checksum(uint8_t *rom, size_t size)
{
    uint32_t s = 0, base;
    uint32_t oc = ari_r16(rom, size - APEX_ROMINFO_CSUM_END);
    uint32_t od = ari_r16(rom, size - APEX_ROMINFO_DELTA_END);
    uint32_t td, tc, adj; size_t i;
    for (i = 0; i < size; i++) s += rom[i];
    s &= 0xFFFF;
    base = (s - (oc >> 8) - (oc & 0xFF) - (od >> 8) - (od & 0xFF)) & 0xFFFF;
    for (td = 0; td <= 0xFFFF; td++) {
        if (td == APEX_ROMINFO_DISABLE_DELTA) continue;
        for (tc = 0; tc <= 0xFFFF; tc += 0x100) {
            adj = (base + (tc>>8)+(tc&0xFF)+(td>>8)+(td&0xFF)) & 0xFFFF;
            if (adj == tc) {
                ari_w16(rom, size - APEX_ROMINFO_DELTA_END, (uint16_t)td);
                ari_w16(rom, size - APEX_ROMINFO_CSUM_END,  (uint16_t)tc);
                return 1;
            }
        }
    }
    return 0;
}

#endif /* APEX_ROMINFO_H */

/* MD5 (RFC 1321). Implemented from the specification text:
 * four rounds of 16 operations over 512-bit blocks, little-endian words. */

#include "sip_md5.h"

#include <string.h>

/* T[i] = floor(2^32 * |sin(i + 1)|), RFC 1321 §3.4 */
static const uint32_t T[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

/* per-round rotation amounts */
static const uint8_t S[4][4] = {
    { 7, 12, 17, 22 },
    { 5, 9, 14, 20 },
    { 4, 11, 16, 23 },
    { 6, 10, 15, 21 },
};

static uint32_t rotl(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

static void md5_block(uint32_t st[4], const uint8_t blk[64])
{
    uint32_t x[16];
    for (int i = 0; i < 16; i++) {
        x[i] = (uint32_t)blk[i * 4] | ((uint32_t)blk[i * 4 + 1] << 8) |
               ((uint32_t)blk[i * 4 + 2] << 16) | ((uint32_t)blk[i * 4 + 3] << 24);
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int k;
        int round = i / 16;
        switch (round) {
        case 0:
            f = (b & c) | (~b & d);          /* F */
            k = i;
            break;
        case 1:
            f = (b & d) | (c & ~d);          /* G */
            k = (5 * i + 1) % 16;
            break;
        case 2:
            f = b ^ c ^ d;                   /* H */
            k = (3 * i + 5) % 16;
            break;
        default:
            f = c ^ (b | ~d);                /* I */
            k = (7 * i) % 16;
            break;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl(a + f + T[i] + x[k], S[round][i % 4]);
        a = tmp;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

void sip_md5_init(sip_md5_ctx_t *c)
{
    c->state[0] = 0x67452301;
    c->state[1] = 0xefcdab89;
    c->state[2] = 0x98badcfe;
    c->state[3] = 0x10325476;
    c->bytes = 0;
    c->fill = 0;
}

void sip_md5_update(sip_md5_ctx_t *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->bytes += len;
    while (len > 0) {
        size_t n = 64 - c->fill;
        if (n > len) {
            n = len;
        }
        memcpy(c->block + c->fill, p, n);
        c->fill += n;
        p += n;
        len -= n;
        if (c->fill == 64) {
            md5_block(c->state, c->block);
            c->fill = 0;
        }
    }
}

void sip_md5_final(sip_md5_ctx_t *c, uint8_t digest[16])
{
    /* pad: 0x80, zeros up to 56 mod 64, then the 64-bit LE bit length */
    uint64_t bits = c->bytes * 8;
    uint8_t pad = 0x80;
    sip_md5_update(c, &pad, 1);
    pad = 0;
    while (c->fill != 56) {
        sip_md5_update(c, &pad, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (8 * i));
    }
    sip_md5_update(c, len, 8);
    for (int i = 0; i < 4; i++) {
        digest[i * 4] = (uint8_t)c->state[i];
        digest[i * 4 + 1] = (uint8_t)(c->state[i] >> 8);
        digest[i * 4 + 2] = (uint8_t)(c->state[i] >> 16);
        digest[i * 4 + 3] = (uint8_t)(c->state[i] >> 24);
    }
    memset(c, 0, sizeof(*c));
}

void sip_md5_to_hex(const uint8_t digest[16], char out[33])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[32] = '\0';
}

void sip_md5_hex(const void *data, size_t len, char out[33])
{
    sip_md5_ctx_t c;
    uint8_t d[16];
    sip_md5_init(&c);
    sip_md5_update(&c, data, len);
    sip_md5_final(&c, d);
    sip_md5_to_hex(d, out);
}

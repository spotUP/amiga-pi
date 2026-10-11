/* sha256 -- see sha256.h. */
#include <string.h>
#include "sha256.h"

#define M32(x) ((x) & 0xffffffffUL)
#define ROR(x, n) M32(((x) >> (n)) | ((x) << (32 - (n))))

static const unsigned long K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL, 0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

void pi_sha256_init(pi_sha256 *c)
{
    static const unsigned long h0[8] = {
        0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL, 0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL
    };
    memcpy(c->h, h0, sizeof(h0));
    c->len_lo = c->len_hi = 0;
    c->bn = 0;
}

static void block(pi_sha256 *c, const unsigned char *b)
{
    unsigned long w[64], a, bb, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((unsigned long)b[i * 4] << 24) | ((unsigned long)b[i * 4 + 1] << 16) |
               ((unsigned long)b[i * 4 + 2] << 8) | (unsigned long)b[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        unsigned long s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned long s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = M32(w[i - 16] + s0 + w[i - 7] + s1);
    }
    a = c->h[0];
    bb = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    f = c->h[5];
    g = c->h[6];
    h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = M32(h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (M32(~e) & g)) + K[i] + w[i]);
        t2 = M32((ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & cc) ^ (bb & cc)));
        h = g;
        g = f;
        f = e;
        e = M32(d + t1);
        d = cc;
        cc = bb;
        bb = a;
        a = M32(t1 + t2);
    }
    c->h[0] = M32(c->h[0] + a);
    c->h[1] = M32(c->h[1] + bb);
    c->h[2] = M32(c->h[2] + cc);
    c->h[3] = M32(c->h[3] + d);
    c->h[4] = M32(c->h[4] + e);
    c->h[5] = M32(c->h[5] + f);
    c->h[6] = M32(c->h[6] + g);
    c->h[7] = M32(c->h[7] + h);
}

void pi_sha256_add(pi_sha256 *c, const void *p, long n)
{
    const unsigned char *s = (const unsigned char *)p;
    unsigned long lo = M32(c->len_lo + (unsigned long)n);
    if (lo < c->len_lo)
        c->len_hi++;
    c->len_lo = lo;
    while (n > 0) {
        long k = 64 - c->bn;
        if (k > n)
            k = n;
        memcpy(c->buf + c->bn, s, (size_t)k);
        c->bn += (int)k;
        s += k;
        n -= k;
        if (c->bn == 64) {
            block(c, c->buf);
            c->bn = 0;
        }
    }
}

void pi_sha256_end(pi_sha256 *c, unsigned char out[32])
{
    unsigned long hi = M32((c->len_hi << 3) | (c->len_lo >> 29)), lo = M32(c->len_lo << 3);
    int i;
    c->buf[c->bn++] = 0x80;
    if (c->bn > 56) {
        memset(c->buf + c->bn, 0, (size_t)(64 - c->bn));
        block(c, c->buf);
        c->bn = 0;
    }
    memset(c->buf + c->bn, 0, (size_t)(56 - c->bn));
    for (i = 0; i < 4; i++) {
        c->buf[56 + i] = (unsigned char)(hi >> (24 - 8 * i));
        c->buf[60 + i] = (unsigned char)(lo >> (24 - 8 * i));
    }
    block(c, c->buf);
    for (i = 0; i < 32; i++)
        out[i] = (unsigned char)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void pi_sha256_hex(const void *p, long n, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char d[32];
    pi_sha256 c;
    int i;
    pi_sha256_init(&c);
    pi_sha256_add(&c, p, n);
    pi_sha256_end(&c, d);
    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[d[i] >> 4];
        out[i * 2 + 1] = hex[d[i] & 15];
    }
    out[64] = 0;
}

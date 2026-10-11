/* sha256 -- SHA-256 (FIPS 180-4) for amiga-pi: the hash suffix of an MCP
 * tool name that is too long or collides (pi's createMcpToolName). Nothing
 * else in vtcon or amiga-pi hashes with SHA-256 yet; Bedrock's SigV4 (phase
 * M, in vtcon llm/) is the next user and should move this file there.
 * Portable C89 (32-bit arithmetic in unsigned long, masked). */
#ifndef PI_SHA256_H
#define PI_SHA256_H

typedef struct pi_sha256 {
    unsigned long h[8];
    unsigned long len_lo, len_hi;   /* bytes so far */
    unsigned char buf[64];
    int bn;
} pi_sha256;

void pi_sha256_init(pi_sha256 *c);
void pi_sha256_add(pi_sha256 *c, const void *p, long n);
void pi_sha256_end(pi_sha256 *c, unsigned char out[32]);
/* the digest of p[0..n) as 64 lower-case hex characters, terminated */
void pi_sha256_hex(const void *p, long n, char out[65]);

#endif

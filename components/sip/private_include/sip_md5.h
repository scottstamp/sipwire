#pragma once

/* MD5 message digest (RFC 1321), written from the algorithm description.
 * Used only for SIP digest authentication. */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[4];
    uint64_t bytes;          /* total length processed */
    uint8_t block[64];
    size_t fill;             /* bytes buffered in block */
} sip_md5_ctx_t;

void sip_md5_init(sip_md5_ctx_t *c);
void sip_md5_update(sip_md5_ctx_t *c, const void *data, size_t len);
void sip_md5_final(sip_md5_ctx_t *c, uint8_t digest[16]);

/* One-shot digest as 32 lowercase hex chars + NUL. */
void sip_md5_hex(const void *data, size_t len, char out[33]);
/* Hex-encode a finished digest. */
void sip_md5_to_hex(const uint8_t digest[16], char out[33]);

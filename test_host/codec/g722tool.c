/* g722tool: raw-file G.722 encode/decode with components/codec, used by
 * test_host/sip/integration.sh to cross-check against Asterisk's codec_g722
 * ("file convert"). Not a ctest.
 *
 *   g722tool enc <in.sln16> <out.g722>   16 kHz s16le -> G.722 64k
 *   g722tool dec <in.g722> <out.sln16>
 *   g722tool ka <out.sln16> [seconds]    write the known-answer input */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g722.h"
#include "g722_ka.h"

static void *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n > 0 ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "read %s failed\n", path);
        exit(2);
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void spit(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, len, f) != len || fclose(f) != 0) {
        fprintf(stderr, "write %s failed\n", path);
        exit(2);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "ka")) {
        size_t n = (size_t)(16000 * (argc > 3 ? atof(argv[3]) : 2.0));
        int16_t *x = malloc(n * sizeof(*x));
        g722_ka_input(x, n);
        spit(argv[2], x, n * sizeof(*x));
        free(x);
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr, "usage: g722tool enc|dec <in> <out> | ka <out> [s]\n");
        return 2;
    }
    size_t len;
    void *in = slurp(argv[2], &len);
    if (!strcmp(argv[1], "enc")) {
        size_t n = len / 2;
        uint8_t *out = malloc(n / 2 + 1);
        g722_enc_t e;
        g722_enc_init(&e);
        size_t m = g722_encode(&e, in, n, out);
        spit(argv[3], out, m);
        free(out);
    } else if (!strcmp(argv[1], "dec")) {
        int16_t *out = malloc(len * 2 * sizeof(int16_t) + 1);
        g722_dec_t d;
        g722_dec_init(&d);
        size_t m = g722_decode(&d, in, len, out);
        spit(argv[3], out, m * sizeof(int16_t));
        free(out);
    } else {
        fprintf(stderr, "bad mode %s\n", argv[1]);
        return 2;
    }
    free(in);
    return 0;
}

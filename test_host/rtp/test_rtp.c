/* Host tests for components/rtp: packetization, DTMF, parsing, jitter
 * buffer, PLC, for G.711 (PT 0/8, resampled to/from 16 kHz) and G.722
 * (PT 9). Network behaviour is simulated with a virtual millisecond clock
 * and a deterministic PRNG (no wall clock, no real sockets). Most jitter
 * buffer tests run on PCMU, whose output can be predicted exactly even
 * across losses; G.722 variants cover the codec-specific parts. */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "g711.h"
#include "g722.h"
#include "resample2.h"
#include "rtp.h"
#include "unity.h"

#define HDR 12

void setUp(void) {}
void tearDown(void) {}

/* ---- helpers ---------------------------------------------------------- */

static uint32_t s_rng = 1;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t rnd_range(uint32_t n)
{
    return rnd() % n;
}

/* Deterministic, unique-per-k noise frame (16 kHz). */
static void gen_frame(uint32_t k, int16_t pcm[RTP_FRAME_SAMPLES])
{
    uint32_t x = k * 2654435761u + 12345u;
    for (int i = 0; i < RTP_FRAME_SAMPLES; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        pcm[i] = (int16_t)((int)(x % 20001) - 10000);
    }
}

/* ---- reference model of the codec path ----
 * G.711: payload k = mu/A-law of the 2:1 decimation of frame k. The
 * decimator's history reaches 46 samples (< one frame) back, so a sender that
 * sent frame k-1 just before produces exactly ref_payload(). The receiver's
 * 1:2 interpolator remembers 23 decoded samples: output samples >= 2 * 46
 * depend on the payload of frame k alone, earlier ones also on the previously
 * decoded frame (expected_after() models both exactly).
 * G.722: the ADPCM state depends on the whole history, so the reference is a
 * sender/receiver pair that ran frames 0..k in order (cached). */

#define CMP_FROM        96      /* frame-local samples, see above */
#define XF              64      /* PLC cross-fade length (rtp.c PLC_XFADE) */
#define G722_REF_MAX    2100

static uint8_t s_g722_pl[G722_REF_MAX][RTP_FRAME_BYTES];
static int16_t s_g722_out[G722_REF_MAX][RTP_FRAME_SAMPLES];
static uint32_t s_g722_n;
static g722_enc_t s_g722_enc;
static g722_dec_t s_g722_dec;

static void g722_ref_upto(uint32_t k)
{
    TEST_ASSERT_LESS_THAN_UINT32(G722_REF_MAX, k);
    if (s_g722_n == 0) {
        g722_enc_init(&s_g722_enc);
        g722_dec_init(&s_g722_dec);
    }
    for (; s_g722_n <= k; s_g722_n++) {
        int16_t pcm[RTP_FRAME_SAMPLES];
        gen_frame(s_g722_n, pcm);
        g722_encode(&s_g722_enc, pcm, RTP_FRAME_SAMPLES, s_g722_pl[s_g722_n]);
        g722_decode(&s_g722_dec, s_g722_pl[s_g722_n], RTP_FRAME_BYTES, s_g722_out[s_g722_n]);
    }
}

static void g711_enc(uint8_t pt, const int16_t *nb, uint8_t *out)
{
    if (pt == 8) {
        g711_alaw_encode_buf(nb, out, RTP_FRAME_TS);
    } else {
        g711_ulaw_encode_buf(nb, out, RTP_FRAME_TS);
    }
}

static void g711_dec(uint8_t pt, const uint8_t *in, int16_t *nb)
{
    if (pt == 8) {
        g711_alaw_decode_buf(in, nb, RTP_FRAME_TS);
    } else {
        g711_ulaw_decode_buf(in, nb, RTP_FRAME_TS);
    }
}

/* Payload of frame k from a sender that sent frames 0..k in order. */
static void ref_payload(uint8_t pt, uint32_t k, uint8_t out[RTP_FRAME_BYTES])
{
    if (pt == 9) {
        g722_ref_upto(k);
        memcpy(out, s_g722_pl[k], RTP_FRAME_BYTES);
        return;
    }
    int16_t pcm[RTP_FRAME_SAMPLES], nb[RTP_FRAME_TS];
    resample2_down_t d;
    resample2_down_init(&d);
    if (k > 0) {
        gen_frame(k - 1, pcm);
        resample2_down(&d, pcm, RTP_FRAME_TS, nb);
    }
    gen_frame(k, pcm);
    resample2_down(&d, pcm, RTP_FRAME_TS, nb);
    g711_enc(pt, nb, out);
}

/* Receiver output for frame k when the frame decoded before it was `prev`
 * (-1: none). G.722 is only modelled for in-order playout (prev == k - 1). */
static void expected_after(uint8_t pt, int prev, uint32_t k, int16_t out[RTP_FRAME_SAMPLES])
{
    if (pt == 9) {
        TEST_ASSERT_EQUAL_INT((int)k - 1, prev);
        g722_ref_upto(k);
        memcpy(out, s_g722_out[k], sizeof(s_g722_out[k]));
        return;
    }
    uint8_t pl[RTP_FRAME_BYTES];
    int16_t nb[RTP_FRAME_TS];
    resample2_up_t u;
    resample2_up_init(&u);
    if (prev >= 0) {
        ref_payload(pt, (uint32_t)prev, pl);
        g711_dec(pt, pl, nb);
        resample2_up(&u, nb, RTP_FRAME_TS, out);
    }
    ref_payload(pt, k, pl);
    g711_dec(pt, pl, nb);
    resample2_up(&u, nb, RTP_FRAME_TS, out);
}

/* What the receiver should output for frame k in an in-order stream.
 * Cached: classify() asks for the same frames over and over. */
#define EXP_CACHE 6000
static int16_t (*s_exp[2])[RTP_FRAME_SAMPLES];
static uint8_t *s_exp_ok[2];

static void expected_frame(uint8_t pt, uint32_t k, int16_t out[RTP_FRAME_SAMPLES])
{
    int c = pt == 8 ? 1 : 0;
    if (pt == 9 || k >= EXP_CACHE) {
        expected_after(pt, (int)k - 1, k, out);
        return;
    }
    if (s_exp[c] == NULL) {
        s_exp[c] = calloc(EXP_CACHE, sizeof(*s_exp[c]));
        s_exp_ok[c] = calloc(EXP_CACHE, 1);
        TEST_ASSERT_NOT_NULL(s_exp[c]);
        TEST_ASSERT_NOT_NULL(s_exp_ok[c]);
    }
    if (!s_exp_ok[c][k]) {
        expected_after(pt, (int)k - 1, k, s_exp[c][k]);
        s_exp_ok[c][k] = 1;
    }
    memcpy(out, s_exp[c][k], sizeof(s_exp[c][k]));
}

/* Samples before CMP_FROM depend on filter history and may be cross-faded
 * after a concealed frame: not compared. */
static bool frames_match(const int16_t *a, const int16_t *b)
{
    return memcmp(a + CMP_FROM, b + CMP_FROM, (RTP_FRAME_SAMPLES - CMP_FROM) * sizeof(int16_t)) == 0;
}

static bool is_silence(const int16_t *f)
{
    for (int i = 0; i < RTP_FRAME_SAMPLES; i++) {
        if (f[i] != 0) {
            return false;
        }
    }
    return true;
}

#define FRAME_SILENCE   (-1)
#define FRAME_OTHER     (-2)

/* Identify an output frame as expected frame k in [from, from + 16). */
static int classify(uint8_t pt, const int16_t *f, int from)
{
    if (is_silence(f)) {
        return FRAME_SILENCE;
    }
    int16_t e[RTP_FRAME_SAMPLES];
    for (int k = from < 0 ? 0 : from; k < from + 16; k++) {
        expected_frame(pt, (uint32_t)k, e);
        if (frames_match(f, e)) {
            return k;
        }
    }
    return FRAME_OTHER;
}

static rtp_config_t make_cfg(uint8_t pt)
{
    rtp_config_t c = {
        .payload_type = pt,
        .dtmf_payload_type = 101,
        .ssrc = 0xA1B2C3D4,
        .initial_seq = 1000,
        .initial_ts = 50000,
        .jb_min_ms = 0,
        .jb_max_ms = 0,
    };
    return c;
}

/* ---- network simulator ------------------------------------------------ */

#define SIM_Q 512

typedef struct {
    bool used;
    uint32_t arrive, order;
    size_t len;
    uint8_t data[RTP_MAX_PACKET];
} qpkt_t;

typedef struct {
    int base_ms;            /* fixed one-way delay */
    int jitter_ms;          /* uniform +-jitter added to the delay */
    int loss_pct;           /* random loss, outside the guard zones */
    int swap_every;         /* delay every Nth packet so it is overtaken */
    int dup_every;          /* duplicate every Nth packet (+3 ms) */
    int burst_from, burst_n;
} net_cfg_t;

typedef struct {
    rtp_session_t *tx, *rx;
    net_cfg_t net;
    int pull_ppm;           /* receiver clock error */
    uint32_t now;
    uint32_t k_sent, k_pulled, order;
    qpkt_t q[SIM_Q];
    /* recording */
    int16_t *rec;
    size_t rec_cap;
    /* depth tracking after warm-up */
    bool track;
    int depth_min, depth_max;
    uint32_t dropped;       /* packets removed by the net model */
    uint32_t guard_end;     /* no random loss for k >= this (0 = none) */
} sim_t;

static void sim_enqueue(sim_t *m, const uint8_t *p, size_t len, uint32_t arrive)
{
    for (int i = 0; i < SIM_Q; i++) {
        if (!m->q[i].used) {
            m->q[i].used = true;
            m->q[i].arrive = arrive;
            m->q[i].order = m->order++;
            m->q[i].len = len;
            memcpy(m->q[i].data, p, len);
            return;
        }
    }
    TEST_FAIL_MESSAGE("sim queue overflow");
}

static void sim_init(sim_t *m, const rtp_config_t *tx_cfg, const rtp_config_t *rx_cfg)
{
    memset(m, 0, sizeof(*m));
    m->tx = rtp_session_create(tx_cfg);
    m->rx = rtp_session_create(rx_cfg);
    TEST_ASSERT_NOT_NULL(m->tx);
    TEST_ASSERT_NOT_NULL(m->rx);
    m->net.base_ms = 40;
    m->depth_min = 1 << 30;
}

static void sim_done(sim_t *m)
{
    rtp_session_destroy(m->tx);
    rtp_session_destroy(m->rx);
}

static void sim_send_frame(sim_t *m)
{
    int16_t pcm[RTP_FRAME_SAMPLES];
    uint8_t pkt[RTP_MAX_PACKET];
    uint32_t k = m->k_sent++;
    gen_frame(k, pcm);
    size_t n = rtp_build_audio(m->tx, pcm, pkt, sizeof(pkt));
    TEST_ASSERT_TRUE(n == HDR + 160 || n == 16);          /* audio, or a DTMF event packet */

    const net_cfg_t *c = &m->net;
    bool drop = (c->burst_n > 0 && (int)k >= c->burst_from && (int)k < c->burst_from + c->burst_n);
    if (!drop && c->loss_pct > 0 && k > 10 && (m->guard_end == 0 || k < m->guard_end) &&
        (int)rnd_range(100) < c->loss_pct) {
        drop = true;
    }
    if (drop) {
        m->dropped++;
        return;
    }
    int delay = c->base_ms;
    if (c->jitter_ms > 0) {
        delay += (int)rnd_range((uint32_t)(2 * c->jitter_ms + 1)) - c->jitter_ms;
    }
    if (c->swap_every > 0 && (int)(k % (uint32_t)c->swap_every) == 0 && k > 0) {
        delay += 25;
    }
    if (delay < 1) {
        delay = 1;
    }
    sim_enqueue(m, pkt, n, m->now + (uint32_t)delay);
    if (c->dup_every > 0 && (int)(k % (uint32_t)c->dup_every) == 3) {
        sim_enqueue(m, pkt, n, m->now + (uint32_t)delay + 3);
    }
}

static void sim_deliver(sim_t *m)
{
    for (;;) {
        int best = -1;
        for (int i = 0; i < SIM_Q; i++) {
            if (m->q[i].used && m->q[i].arrive <= m->now &&
                (best < 0 || m->q[i].arrive < m->q[best].arrive ||
                 (m->q[i].arrive == m->q[best].arrive && m->q[i].order < m->q[best].order))) {
                best = i;
            }
        }
        if (best < 0) {
            return;
        }
        rtp_on_packet(m->rx, m->q[best].data, m->q[best].len, m->now);
        m->q[best].used = false;
    }
}

static uint32_t sim_pull_time(const sim_t *m, uint32_t j)
{
    int64_t t = (int64_t)j * 20 * (1000000 + m->pull_ppm) / 1000000;
    return 10 + (uint32_t)t;
}

static void sim_advance(sim_t *m, uint32_t ms)
{
    uint32_t end = m->now + ms;
    for (; m->now < end; m->now++) {
        sim_deliver(m);
        if (m->now % 20 == 0) {
            sim_send_frame(m);
        }
        if (m->now == sim_pull_time(m, m->k_pulled)) {
            int16_t out[RTP_FRAME_SAMPLES];
            rtp_pull_frame(m->rx, out, m->now);
            if (m->rec != NULL && m->k_pulled < m->rec_cap) {
                memcpy(m->rec + (size_t)m->k_pulled * RTP_FRAME_SAMPLES, out, sizeof(out));
            }
            m->k_pulled++;
            if (m->track) {
                rtp_stats_t st;
                rtp_get_stats(m->rx, &st);
                if ((int)st.jb_depth_ms < m->depth_min) {
                    m->depth_min = st.jb_depth_ms;
                }
                if ((int)st.jb_depth_ms > m->depth_max) {
                    m->depth_max = st.jb_depth_ms;
                }
            }
        }
    }
}

static const int16_t *rec_frame(const sim_t *m, size_t i)
{
    return m->rec + i * RTP_FRAME_SAMPLES;
}

/* Walk the recording: leading frames must be silence, then every frame must
 * be the next expected one in order. Returns the number of leading silent
 * frames. Fails on anything else. */
static size_t check_perfect(const sim_t *m, uint8_t pt, size_t nframes, size_t *real_out)
{
    size_t lead = 0, real = 0;
    int last = -1;
    bool seen = false;
    for (size_t i = 0; i < nframes; i++) {
        int k = classify(pt, rec_frame(m, i), last + 1);
        if (k == FRAME_SILENCE) {
            TEST_ASSERT_FALSE_MESSAGE(seen, "silence after audio started");
            lead++;
            continue;
        }
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, k, "unexpected frame content");
        if (seen) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(last + 1, k, "frames out of order or missing");
        } else {
            TEST_ASSERT_EQUAL_INT(0, k);
        }
        seen = true;
        last = k;
        real++;
    }
    if (real_out) {
        *real_out = real;
    }
    return lead;
}

/* ---- packet crafting for parser tests --------------------------------- */

typedef struct {
    int cc;                 /* CSRC count */
    int ext_words;          /* -1: no extension */
    int pad;                /* 0: no padding */
    bool marker;
    uint8_t pt;
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
} craft_t;

static size_t craft(uint8_t *b, const craft_t *c, const uint8_t *payload, size_t plen)
{
    size_t n = 0;
    b[0] = (uint8_t)(0x80 | (c->pad ? 0x20 : 0) | (c->ext_words >= 0 ? 0x10 : 0) | (c->cc & 0x0F));
    b[1] = (uint8_t)((c->marker ? 0x80 : 0) | c->pt);
    b[2] = (uint8_t)(c->seq >> 8);
    b[3] = (uint8_t)c->seq;
    for (int i = 0; i < 4; i++) {
        b[4 + i] = (uint8_t)(c->ts >> (24 - 8 * i));
        b[8 + i] = (uint8_t)(c->ssrc >> (24 - 8 * i));
    }
    n = 12;
    for (int i = 0; i < c->cc; i++) {
        memset(b + n, 0xC0 + i, 4);
        n += 4;
    }
    if (c->ext_words >= 0) {
        b[n] = 0xBE;
        b[n + 1] = 0xDE;
        b[n + 2] = (uint8_t)(c->ext_words >> 8);
        b[n + 3] = (uint8_t)c->ext_words;
        n += 4;
        memset(b + n, 0xEE, (size_t)c->ext_words * 4);
        n += (size_t)c->ext_words * 4;
    }
    memmove(b + n, payload, plen);
    n += plen;
    if (c->pad) {
        memset(b + n, 0, (size_t)c->pad - 1);
        n += (size_t)c->pad - 1;
        b[n++] = (uint8_t)c->pad;
    }
    return n;
}

static void encode_frame(uint32_t k, uint8_t out[RTP_FRAME_BYTES])
{
    ref_payload(0, k, out);
}

/* Feed 4 frames crafted with `base` (seq/ts advanced), pull, and expect the
 * audio back unchanged. */
static void check_craft_variant(const craft_t *base)
{
    rtp_config_t cfg = make_cfg(0);
    rtp_session_t *rx = rtp_session_create(&cfg);
    craft_t c = *base;
    uint8_t buf[RTP_MAX_PACKET + 256];
    uint8_t pl[RTP_FRAME_BYTES];
    int16_t out[RTP_FRAME_SAMPLES], e[RTP_FRAME_SAMPLES];
    for (uint32_t k = 0; k < 4; k++) {
        encode_frame(k, pl);
        c.seq = (uint16_t)(base->seq + k);
        c.ts = base->ts + k * 160;
        size_t n = craft(buf, &c, pl, sizeof(pl));
        rtp_on_packet(rx, buf, n, k * 20);
    }
    rtp_stats_t st;
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_invalid);
    TEST_ASSERT_EQUAL_UINT32(4, st.rx_packets);
    for (uint32_t k = 0; k < 4; k++) {
        rtp_pull_frame(rx, out, 100 + k * 20);
        expected_frame(0, k, e);
        TEST_ASSERT_EQUAL_INT16_ARRAY(e, out, RTP_FRAME_SAMPLES);
    }
    rtp_session_destroy(rx);
}

/* ---- tests: TX / header ----------------------------------------------- */

void test_header_build_and_roundtrip(void)
{
    rtp_config_t cfg = make_cfg(0);
    rtp_session_t *tx = rtp_session_create(&cfg);
    rtp_session_t *rx = rtp_session_create(&cfg);
    uint8_t p[3][RTP_MAX_PACKET];
    int16_t pcm[RTP_FRAME_SAMPLES];
    for (uint32_t k = 0; k < 3; k++) {
        gen_frame(k, pcm);
        TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(tx, pcm, p[k], sizeof(p[k])));
    }
    TEST_ASSERT_EQUAL_HEX8(0x80, p[0][0]);          /* V=2 P=0 X=0 CC=0 */
    TEST_ASSERT_EQUAL_HEX8(0x80, p[0][1]);          /* marker on first packet, PT 0 */
    TEST_ASSERT_EQUAL_HEX8(0x00, p[1][1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, p[2][1]);
    for (int k = 0; k < 3; k++) {
        uint16_t seq = (uint16_t)((p[k][2] << 8) | p[k][3]);
        uint32_t ts = ((uint32_t)p[k][4] << 24) | ((uint32_t)p[k][5] << 16) |
                      ((uint32_t)p[k][6] << 8) | p[k][7];
        uint32_t ssrc = ((uint32_t)p[k][8] << 24) | ((uint32_t)p[k][9] << 16) |
                        ((uint32_t)p[k][10] << 8) | p[k][11];
        TEST_ASSERT_EQUAL_UINT16(1000 + k, seq);
        TEST_ASSERT_EQUAL_UINT32(50000 + 160u * (uint32_t)k, ts);
        TEST_ASSERT_EQUAL_HEX32(0xA1B2C3D4, ssrc);
    }
    uint8_t enc[RTP_FRAME_BYTES];
    ref_payload(0, 1, enc);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(enc, p[1] + HDR, 160);

    /* Round trip through the parser and jitter buffer. */
    for (int k = 0; k < 3; k++) {
        rtp_on_packet(rx, p[k], HDR + 160, (uint32_t)k * 20);
    }
    int16_t out[RTP_FRAME_SAMPLES], e[RTP_FRAME_SAMPLES];
    for (uint32_t k = 0; k < 3; k++) {
        rtp_pull_frame(rx, out, 60 + k * 20);
        expected_frame(0, k, e);
        TEST_ASSERT_EQUAL_INT16_ARRAY(e, out, RTP_FRAME_SAMPLES);
    }
    rtp_stats_t st;
    rtp_get_stats(tx, &st);
    TEST_ASSERT_EQUAL_UINT32(3, st.tx_packets);
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(3, st.rx_packets);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_invalid);
    rtp_session_destroy(tx);
    rtp_session_destroy(rx);
}

void test_pcma_payload_and_pt(void)
{
    rtp_config_t cfg = make_cfg(8);
    rtp_session_t *tx = rtp_session_create(&cfg);
    int16_t pcm[RTP_FRAME_SAMPLES];
    uint8_t p[RTP_MAX_PACKET], enc[RTP_FRAME_BYTES];
    gen_frame(0, pcm);
    TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(tx, pcm, p, sizeof(p)));
    TEST_ASSERT_EQUAL_HEX8(0x88, p[1]);             /* marker | PT 8 */
    ref_payload(8, 0, enc);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(enc, p + HDR, 160);
    rtp_session_destroy(tx);

    /* PCMA end to end */
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    static int16_t rec[100 * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = 100;
    sim_advance(&m, 100 * 20);
    check_perfect(&m, 8, 100, NULL);
    sim_done(&m);
}

void test_build_audio_small_buffer_does_not_advance(void)
{
    rtp_config_t cfg = make_cfg(0);
    rtp_session_t *tx = rtp_session_create(&cfg);
    int16_t pcm[RTP_FRAME_SAMPLES] = {0};
    uint8_t p[RTP_MAX_PACKET];
    TEST_ASSERT_EQUAL_UINT(0, rtp_build_audio(tx, pcm, p, 100));
    TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(tx, pcm, p, sizeof(p)));
    TEST_ASSERT_EQUAL_UINT16(1000, (uint16_t)((p[2] << 8) | p[3]));
    TEST_ASSERT_EQUAL_HEX8(0x80, p[1]);             /* marker still pending */
    rtp_session_destroy(tx);
}

void test_create_validation_and_defaults(void)
{
    rtp_config_t cfg = make_cfg(0);
    TEST_ASSERT_NULL(rtp_session_create(NULL));
    cfg.payload_type = 18;
    TEST_ASSERT_NULL(rtp_session_create(&cfg));
    cfg.payload_type = 9;                           /* G.722 */
    rtp_session_t *g = rtp_session_create(&cfg);
    TEST_ASSERT_NOT_NULL(g);
    rtp_session_destroy(g);
    cfg.dtmf_payload_type = 9;                      /* clashes with the audio PT */
    TEST_ASSERT_NULL(rtp_session_create(&cfg));
    cfg = make_cfg(0);
    cfg.dtmf_payload_type = 0;
    rtp_session_t *s = rtp_session_create(&cfg);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FALSE(rtp_send_dtmf(s, '1', 100));  /* DTMF disabled */
    rtp_stats_t st;
    rtp_get_stats(s, &st);
    TEST_ASSERT_EQUAL_UINT16(40, st.jb_target_ms);  /* default min */
    rtp_session_destroy(s);
    rtp_session_destroy(NULL);
}

/* ---- tests: parser ---------------------------------------------------- */

void test_rx_padding_csrc_extension(void)
{
    craft_t base = {.cc = 0, .ext_words = -1, .pad = 0, .pt = 0, .seq = 77,
                    .ts = 9000, .ssrc = 0x1234};
    check_craft_variant(&base);
    craft_t pad = base;
    pad.pad = 4;
    check_craft_variant(&pad);
    pad.pad = 1;
    check_craft_variant(&pad);
    craft_t csrc = base;
    csrc.cc = 3;
    check_craft_variant(&csrc);
    craft_t ext = base;
    ext.ext_words = 5;
    check_craft_variant(&ext);
    ext.ext_words = 0;
    check_craft_variant(&ext);
    craft_t all = base;
    all.cc = 15;
    all.ext_words = 7;
    all.pad = 9;
    all.marker = true;
    check_craft_variant(&all);
}

void test_rx_malformed_counts_invalid(void)
{
    rtp_config_t cfg = make_cfg(0);
    rtp_session_t *rx = rtp_session_create(&cfg);
    uint8_t buf[1024];
    uint8_t pl[RTP_FRAME_BYTES];
    encode_frame(0, pl);
    craft_t c = {.cc = 0, .ext_words = -1, .pad = 0, .pt = 0, .seq = 1, .ts = 160, .ssrc = 9};
    uint32_t expect_invalid = 0;
    rtp_stats_t st;

    /* too short */
    size_t n = craft(buf, &c, pl, sizeof(pl));
    for (size_t l = 0; l < HDR; l++) {
        rtp_on_packet(rx, buf, l, 0);
        expect_invalid++;
    }
    /* header only: no payload */
    rtp_on_packet(rx, buf, HDR, 0);
    expect_invalid++;
    /* wrong version */
    for (int v = 0; v < 4; v++) {
        if (v == 2) {
            continue;
        }
        n = craft(buf, &c, pl, sizeof(pl));
        buf[0] = (uint8_t)((buf[0] & 0x3F) | (v << 6));
        rtp_on_packet(rx, buf, n, 0);
        expect_invalid++;
    }
    /* CSRC list runs past the end */
    c.cc = 15;
    n = craft(buf, &c, pl, sizeof(pl));
    rtp_on_packet(rx, buf, 12 + 4 * 15 - 1, 0);
    expect_invalid++;
    c.cc = 0;
    /* extension header truncated / extension longer than the packet */
    c.ext_words = 3;
    n = craft(buf, &c, pl, sizeof(pl));
    rtp_on_packet(rx, buf, 14, 0);
    expect_invalid++;
    buf[14] = 0xFF;
    buf[15] = 0xFF;                                 /* 65535 words */
    rtp_on_packet(rx, buf, n, 0);
    expect_invalid++;
    c.ext_words = -1;
    /* padding length 0, longer than payload+header, equal to everything */
    c.pad = 4;
    n = craft(buf, &c, pl, sizeof(pl));
    buf[n - 1] = 0;
    rtp_on_packet(rx, buf, n, 0);
    expect_invalid++;
    buf[n - 1] = 255;
    rtp_on_packet(rx, buf, n, 0);
    expect_invalid++;
    c.pad = 0;
    n = craft(buf, &c, pl, sizeof(pl));
    buf[0] |= 0x20;
    buf[n - 1] = (uint8_t)(n - HDR);                /* pad eats the whole payload */
    rtp_on_packet(rx, buf, n, 0);
    expect_invalid++;
    /* payload not a whole number of frames, or too big */
    n = craft(buf, &c, pl, 159);
    rtp_on_packet(rx, buf, n, 0);
    expect_invalid++;
    uint8_t big[160 * 5];
    memset(big, 0xFF, sizeof(big));
    n = craft(buf, &c, big, sizeof(big));
    rtp_on_packet(rx, buf, n, 0);
    expect_invalid++;
    /* wrong payload type */
    c.pt = 18;
    n = craft(buf, &c, pl, sizeof(pl));
    rtp_on_packet(rx, buf, n, 0);
    expect_invalid++;

    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(expect_invalid, st.rx_invalid);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_packets);

    /* telephone-event is ignored: neither audio nor invalid */
    c.pt = 101;
    uint8_t ev[4] = {5, 0x0A, 0, 160};
    n = craft(buf, &c, ev, sizeof(ev));
    rtp_on_packet(rx, buf, n, 0);
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(expect_invalid, st.rx_invalid);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_packets);

    /* NULL arguments are harmless */
    rtp_on_packet(rx, NULL, 0, 0);
    rtp_on_packet(NULL, buf, 10, 0);
    int16_t out[RTP_FRAME_SAMPLES];
    rtp_pull_frame(rx, out, 0);
    TEST_ASSERT_TRUE(is_silence(out));              /* still no audio */
    rtp_session_destroy(rx);
}

void test_rx_multi_frame_packet(void)
{
    rtp_config_t cfg = make_cfg(0);
    rtp_session_t *rx = rtp_session_create(&cfg);
    uint8_t buf[RTP_MAX_PACKET + 64], pl[RTP_FRAME_BYTES * 2];
    encode_frame(0, pl);
    encode_frame(1, pl + RTP_FRAME_BYTES);
    craft_t c = {.ext_words = -1, .pt = 0, .seq = 5, .ts = 4000, .ssrc = 3};
    size_t n = craft(buf, &c, pl, sizeof(pl));      /* one 40 ms packet */
    rtp_on_packet(rx, buf, n, 0);
    int16_t out[RTP_FRAME_SAMPLES], e[RTP_FRAME_SAMPLES];
    for (uint32_t k = 0; k < 2; k++) {
        rtp_pull_frame(rx, out, 50 + k * 20);
        expected_frame(0, k, e);
        TEST_ASSERT_EQUAL_INT16_ARRAY(e, out, RTP_FRAME_SAMPLES);
    }
    rtp_session_destroy(rx);
}

/* ---- tests: streams --------------------------------------------------- */

void test_perfect_stream(void)
{
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    enum { N = 600 };
    static int16_t rec[N * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = N;
    sim_advance(&m, N * 20);
    size_t real;
    size_t lead = check_perfect(&m, 0, N, &real);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(6, lead);        /* initial delay: a few frames */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(1, lead);
    TEST_ASSERT_EQUAL_UINT(N - lead, real);
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.plc_frames);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_lost);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_late);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_dup);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_invalid);
    TEST_ASSERT_EQUAL_UINT16(40, st.jb_target_ms);
    TEST_ASSERT_EQUAL_UINT32(0, st.jitter_ms);
    TEST_ASSERT_EQUAL_UINT16(40, st.jb_depth_ms);
    sim_done(&m);
}

void test_wraparound_seq_and_ts(void)
{
    rtp_config_t cfg = make_cfg(0);
    cfg.initial_seq = 0xFFF0;
    cfg.initial_ts = 0xFFFFFF00u;                   /* wraps after ~2 frames */
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    enum { N = 120 };
    static int16_t rec[N * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = N;
    sim_advance(&m, N * 20);
    size_t real;
    check_perfect(&m, 0, N, &real);
    TEST_ASSERT_GREATER_THAN_UINT(N - 10, real);
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.plc_frames);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_lost);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_late);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_dup);
    sim_done(&m);
}

void test_wraparound_with_loss_across_boundary(void)
{
    rtp_config_t cfg = make_cfg(0);
    cfg.initial_seq = 0xFFFC;
    cfg.initial_ts = 0xFFFFFA00u;
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.net.burst_from = 3;                           /* seq FFFF, 0, 1, 2 lost */
    m.net.burst_n = 4;
    sim_advance(&m, 60 * 20);
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(4, st.rx_lost);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_dup);
    TEST_ASSERT_EQUAL_UINT32(4, st.plc_frames);
    sim_done(&m);
}

void test_seq_wrap_second_cycle_lost_count(void)
{
    /* Loss accounting over several 16-bit wraps. */
    rtp_config_t cfg = make_cfg(0);
    cfg.initial_seq = 65000;
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.net.loss_pct = 3;
    s_rng = 99;
    sim_advance(&m, 4000 * 20);
    m.net.loss_pct = 0;
    sim_advance(&m, 20 * 20);
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_GREATER_THAN_UINT32(0, m.dropped);
    TEST_ASSERT_EQUAL_UINT32(m.dropped, st.rx_lost);
    sim_done(&m);
}

void test_reordering_adjacent_swap(void)
{
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.net.swap_every = 6;
    enum { N = 400 };
    static int16_t rec[N * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = N;
    sim_advance(&m, N * 20);
    size_t real;
    check_perfect(&m, 0, N, &real);                 /* exact audio, in order */
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.plc_frames);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_lost);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_late);
    sim_done(&m);
}

void test_duplicates_are_counted_and_dropped(void)
{
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.net.dup_every = 7;
    enum { N = 300 };
    static int16_t rec[N * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = N;
    sim_advance(&m, N * 20);
    check_perfect(&m, 0, N, NULL);
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    uint32_t dups = 0;
    for (uint32_t k = 0; k < m.k_sent; k++) {
        if (k % 7 == 3) {
            dups++;
        }
    }
    /* Duplicates still in flight at the end are not counted yet. */
    TEST_ASSERT_UINT32_WITHIN(3, dups, st.rx_dup);
    TEST_ASSERT_EQUAL_UINT32(0, st.plc_frames);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_lost);
    sim_done(&m);
}

void test_duplicate_of_already_played_packet(void)
{
    rtp_config_t cfg = make_cfg(0);
    rtp_session_t *tx = rtp_session_create(&cfg);
    rtp_session_t *rx = rtp_session_create(&cfg);
    uint8_t p[10][RTP_MAX_PACKET];
    int16_t pcm[RTP_FRAME_SAMPLES], out[RTP_FRAME_SAMPLES];
    for (uint32_t k = 0; k < 10; k++) {
        gen_frame(k, pcm);
        rtp_build_audio(tx, pcm, p[k], sizeof(p[k]));
        rtp_on_packet(rx, p[k], HDR + 160, k * 20);
        rtp_pull_frame(rx, out, k * 20 + 10);
    }
    rtp_on_packet(rx, p[0], HDR + 160, 300);        /* long since played */
    rtp_on_packet(rx, p[9], HDR + 160, 301);        /* still buffered */
    rtp_stats_t st;
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(2, st.rx_dup);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_late);
    rtp_session_destroy(tx);
    rtp_session_destroy(rx);
}

void test_late_packet_is_counted_and_dropped(void)
{
    rtp_config_t cfg = make_cfg(0);
    rtp_session_t *tx = rtp_session_create(&cfg);
    rtp_session_t *rx = rtp_session_create(&cfg);
    uint8_t p[12][RTP_MAX_PACKET];
    int16_t pcm[RTP_FRAME_SAMPLES], out[RTP_FRAME_SAMPLES];
    for (uint32_t k = 0; k < 12; k++) {
        gen_frame(k, pcm);
        rtp_build_audio(tx, pcm, p[k], sizeof(p[k]));
    }
    /* Packet 3 is delayed so long it shows up after it has been concealed
     * and played past. */
    for (uint32_t k = 0; k < 12; k++) {
        if (k != 3) {
            rtp_on_packet(rx, p[k], HDR + 160, k * 20);
        }
        rtp_pull_frame(rx, out, k * 20 + 10);
    }
    rtp_on_packet(rx, p[3], HDR + 160, 400);
    rtp_stats_t st;
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(1, st.rx_late);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_dup);
    TEST_ASSERT_EQUAL_UINT32(1, st.plc_frames);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_lost);        /* it did arrive */
    rtp_session_destroy(tx);
    rtp_session_destroy(rx);
}

void test_random_loss_5_percent(void)
{
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.net.loss_pct = 5;
    m.guard_end = 1900;
    s_rng = 12345;
    enum { N = 2000 };
    static int16_t rec[N * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = N;
    sim_advance(&m, N * 20);
    TEST_ASSERT_GREATER_THAN_UINT32(50, m.dropped);
    TEST_ASSERT_LESS_THAN_UINT32(160, m.dropped);

    /* Every real frame appears in order; the rest are concealment. */
    int last = -1;
    size_t concealed = 0, i = 0;
    while (i < N && classify(0, rec_frame(&m, i), 0) == FRAME_SILENCE) {
        i++;
    }
    for (; i < N - 40; i++) {
        int k = classify(0, rec_frame(&m, i), last + 1);
        if (k >= 0) {
            TEST_ASSERT_GREATER_THAN_INT(last, k);
            last = k;
        } else {
            concealed++;
        }
    }
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(m.dropped, st.rx_lost);
    TEST_ASSERT_EQUAL_UINT32(m.dropped, st.plc_frames);
    TEST_ASSERT_UINT32_WITHIN(3, (uint32_t)concealed, m.dropped);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_late);
    sim_done(&m);
}

void test_burst_loss_of_four_plc_then_silence(void)
{
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.net.burst_from = 50;
    m.net.burst_n = 4;
    enum { N = 120 };
    static int16_t rec[N * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = N;
    sim_advance(&m, N * 20);

    size_t i = 0;
    int last = -1;
    while (i < N && last != 49) {
        int k = classify(0, rec_frame(&m, i), last + 1);
        if (k >= 0) {
            last = k;
        }
        i++;
    }
    TEST_ASSERT_EQUAL_INT(49, last);
    int16_t e[RTP_FRAME_SAMPLES];
    const int16_t *real49 = rec_frame(&m, i - 1);   /* as played */
    expected_frame(0, 49, e);
    TEST_ASSERT_EQUAL_INT16_ARRAY(e, real49, RTP_FRAME_SAMPLES);
    static const int32_t gain[3] = {22938, 16056, 11239};
    for (int n = 0; n < 3; n++) {
        const int16_t *f = rec_frame(&m, i + (size_t)n);
        for (int j = 0; j < RTP_FRAME_SAMPLES; j++) {
            e[j] = (int16_t)((real49[j] * gain[n]) >> 15);
        }
        TEST_ASSERT_EQUAL_INT16_ARRAY(e, f, RTP_FRAME_SAMPLES);
    }
    TEST_ASSERT_TRUE(is_silence(rec_frame(&m, i + 3)));     /* 4th loss: silence */

    /* Frame 54 returns, cross-faded in from the (silent) tail; the
     * interpolator still holds the tail of frame 49. */
    const int16_t *f54 = rec_frame(&m, i + 4);
    expected_after(0, 49, 54, e);
    TEST_ASSERT_EQUAL_INT16(0, f54[0]);
    TEST_ASSERT_EQUAL_INT16((int16_t)((e[XF / 2] * (XF / 2)) / XF), f54[XF / 2]);
    TEST_ASSERT_EQUAL_INT16_ARRAY(e + XF, f54 + XF, RTP_FRAME_SAMPLES - XF);

    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(4, st.plc_frames);
    TEST_ASSERT_EQUAL_UINT32(4, st.rx_lost);
    /* and the stream carries on cleanly */
    for (size_t j = i + 4; j < N - 5; j++) {
        int want = 54 + (int)(j - (i + 4));
        TEST_ASSERT_EQUAL_INT(want, classify(0, rec_frame(&m, j), want));
    }
    sim_done(&m);
}

void test_single_loss_crossfades_smoothly(void)
{
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.net.burst_from = 30;
    m.net.burst_n = 1;
    enum { N = 70 };
    static int16_t rec[N * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = N;
    sim_advance(&m, N * 20);
    size_t i = 0;
    while (classify(0, rec_frame(&m, i), 29) != 29) {
        i++;
    }
    int16_t e[RTP_FRAME_SAMPLES], r31[RTP_FRAME_SAMPLES];
    const int16_t *r29 = rec_frame(&m, i);          /* as played */
    expected_after(0, 29, 31, r31);
    /* concealed frame = 0.7 x frame 29 */
    for (int j = 0; j < RTP_FRAME_SAMPLES; j++) {
        e[j] = (int16_t)((r29[j] * 22938) >> 15);
    }
    TEST_ASSERT_EQUAL_INT16_ARRAY(e, rec_frame(&m, i + 1), RTP_FRAME_SAMPLES);
    /* frame 31 starts from the 0.49 continuation and fades to the real signal */
    const int16_t *f31 = rec_frame(&m, i + 2);
    int16_t t0 = (int16_t)((r29[0] * 16056) >> 15);
    TEST_ASSERT_EQUAL_INT16(t0, f31[0]);
    TEST_ASSERT_EQUAL_INT16_ARRAY(r31 + XF, f31 + XF, RTP_FRAME_SAMPLES - XF);
    sim_done(&m);
}

/* ---- tests: adaptive jitter buffer ------------------------------------ */

void test_jitter_grows_then_shrinks(void)
{
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    s_rng = 777;
    rtp_stats_t st;

    m.net.base_ms = 60;
    m.net.jitter_ms = 30;
    sim_advance(&m, 30000);
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_GREATER_THAN_UINT16(40, st.jb_target_ms);   /* grew */
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(160, st.jb_target_ms);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(8, st.jitter_ms);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(40, st.jitter_ms);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(60, st.jb_depth_ms);    /* depth followed it */
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(160, st.jb_depth_ms);

    m.net.jitter_ms = 0;                            /* calm again */
    sim_advance(&m, 30000);
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT16(40, st.jb_target_ms);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(60, st.jb_depth_ms);  /* buffer shrank back to target +-1 */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16(20, st.jb_depth_ms);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1, st.jitter_ms);
    sim_done(&m);
}

void test_jitter_moderate_has_little_concealment(void)
{
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    s_rng = 4242;
    m.net.base_ms = 60;
    m.net.jitter_ms = 30;
    sim_advance(&m, 20000);                         /* settle */
    rtp_stats_t a, b;
    rtp_get_stats(m.rx, &a);
    sim_advance(&m, 60000);
    rtp_get_stats(m.rx, &b);
    /* after adapting, fewer than 3% of frames are concealed or late */
    TEST_ASSERT_LESS_THAN_UINT32(90, b.plc_frames - a.plc_frames);
    TEST_ASSERT_LESS_THAN_UINT32(90, b.rx_late - a.rx_late);
    sim_done(&m);
}

void test_long_run_depth_stays_bounded(void)
{
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    s_rng = 2024;
    m.net.base_ms = 60;
    sim_advance(&m, 1000);
    m.track = true;
    m.depth_min = 1 << 30;
    /* 200 simulated seconds: alternating bursts of jitter and calm. */
    for (int phase = 0; phase < 20; phase++) {
        m.net.jitter_ms = (phase % 2 == 0) ? 30 : 0;
        if (phase % 5 == 4) {
            m.net.jitter_ms = 15;
            m.net.loss_pct = 2;
        } else {
            m.net.loss_pct = 0;
        }
        sim_advance(&m, 10000);
        rtp_stats_t st;
        rtp_get_stats(m.rx, &st);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT16(40, st.jb_target_ms);
        TEST_ASSERT_LESS_OR_EQUAL_UINT16(160, st.jb_target_ms);
    }
    TEST_ASSERT_GREATER_THAN_UINT32(9700, m.k_pulled);
    TEST_ASSERT_LESS_OR_EQUAL_INT(160, m.depth_max);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(20, m.depth_min);
    sim_done(&m);
}

static void run_drift(int ppm)
{
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.pull_ppm = ppm;
    m.net.base_ms = 50;
    sim_advance(&m, 1000);
    m.track = true;
    m.depth_min = 1 << 30;
    sim_advance(&m, 180000);                        /* 3 minutes */
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_LESS_OR_EQUAL_INT(160, m.depth_max);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, m.depth_min);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(160, st.jb_depth_ms);
    /* 600 ppm over 3 min is ~108 ms of drift: a few adjustments */
    TEST_ASSERT_LESS_THAN_UINT32(40, st.plc_frames);
    TEST_ASSERT_LESS_THAN_UINT32(40, st.rx_late);
    sim_done(&m);
}

void test_clock_drift_receiver_fast(void)
{
    run_drift(-600);        /* receiver clock fast: pulls more often */
}

void test_clock_drift_receiver_slow(void)
{
    run_drift(+600);        /* receiver clock slow: buffer fills up */
}

void test_burst_after_stall_resyncs(void)
{
    /* 1 s network stall, then everything arrives at once. */
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    sim_advance(&m, 3000);
    m.net.base_ms = 1040;
    sim_advance(&m, 20);
    m.net.base_ms = 40;
    sim_advance(&m, 2000);
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(160, st.jb_depth_ms);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(160, st.jb_target_ms);
    sim_done(&m);
}

void test_timestamp_gap_silence_suppression(void)
{
    /* Sender pauses for 5 s (ts keeps running), then talks again. */
    rtp_config_t cfg = make_cfg(0);
    rtp_session_t *tx = rtp_session_create(&cfg);
    rtp_session_t *rx = rtp_session_create(&cfg);
    uint8_t p[RTP_MAX_PACKET];
    int16_t pcm[RTP_FRAME_SAMPLES], out[RTP_FRAME_SAMPLES], e[RTP_FRAME_SAMPLES];
    uint32_t t = 0;
    for (uint32_t k = 0; k < 20; k++, t += 20) {
        gen_frame(k, pcm);
        rtp_build_audio(tx, pcm, p, sizeof(p));
        rtp_on_packet(rx, p, HDR + 160, t);
        rtp_pull_frame(rx, out, t + 10);
    }
    /* 250 frames pass: advance the sender clock by building (and dropping). */
    for (uint32_t k = 0; k < 250; k++, t += 20) {
        rtp_build_audio(tx, pcm, p, sizeof(p));
        rtp_pull_frame(rx, out, t + 10);
    }
    int real = 0;
    uint32_t kk = 1000;
    for (uint32_t i = 0; i < 40; i++, t += 20, kk++) {
        gen_frame(kk, pcm);
        rtp_build_audio(tx, pcm, p, sizeof(p));
        rtp_on_packet(rx, p, HDR + 160, t);
        rtp_pull_frame(rx, out, t + 10);
        for (uint32_t d = 0; d < 6; d++) {          /* a few frames of latency */
            expected_frame(0, kk - d, e);
            if (frames_match(out, e)) {
                real++;
                break;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(30, real);         /* playout recovered */
    rtp_stats_t st;
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_late);
    rtp_session_destroy(tx);
    rtp_session_destroy(rx);
}

/* ---- tests: SSRC ------------------------------------------------------ */

static void feed_and_pull(rtp_session_t *rx, const uint8_t *p, size_t n, uint32_t t,
                          int16_t *out)
{
    rtp_on_packet(rx, p, n, t);
    rtp_pull_frame(rx, out, t + 10);
}

void test_ssrc_switch_after_three_packets(void)
{
    rtp_config_t ca = make_cfg(0);
    rtp_config_t cb = make_cfg(0);
    cb.ssrc = 0x55667788;
    cb.initial_seq = 40000;
    cb.initial_ts = 0x12345678;
    rtp_session_t *a = rtp_session_create(&ca);
    rtp_session_t *b = rtp_session_create(&cb);
    rtp_session_t *rx = rtp_session_create(&ca);
    uint8_t p[RTP_MAX_PACKET];
    int16_t pcm[RTP_FRAME_SAMPLES], out[RTP_FRAME_SAMPLES];
    uint32_t t = 0;
    for (uint32_t k = 0; k < 30; k++, t += 20) {
        gen_frame(k, pcm);
        rtp_build_audio(a, pcm, p, sizeof(p));
        feed_and_pull(rx, p, HDR + 160, t, out);
    }
    rtp_stats_t st;
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(30, st.rx_packets);

    static int16_t rec[40][RTP_FRAME_SAMPLES];
    for (uint32_t k = 0; k < 40; k++, t += 20) {
        gen_frame(1000 + k, pcm);
        rtp_build_audio(b, pcm, p, sizeof(p));
        feed_and_pull(rx, p, HDR + 160, t, out);
        memcpy(rec[k], out, sizeof(out));
        if (k == 1) {                               /* two packets: not yet switched */
            rtp_get_stats(rx, &st);
            TEST_ASSERT_EQUAL_UINT32(30, st.rx_packets);
        }
    }
    /* Packets 3.. of the new stream (k=2..39) play out, in order. */
    int last = 1001;
    int real = 0;
    for (int i = 0; i < 40; i++) {
        int k = classify(0, rec[i], last + 1);
        if (k >= 0) {
            TEST_ASSERT_EQUAL_INT(last + 1, k);
            last = k;
            real++;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1030, last);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(30, real);
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(30 + 38, st.rx_packets);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_invalid);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_late);
    rtp_session_destroy(a);
    rtp_session_destroy(b);
    rtp_session_destroy(rx);
}

void test_ssrc_stray_packets_are_ignored(void)
{
    rtp_config_t ca = make_cfg(0);
    rtp_config_t cb = make_cfg(0);
    cb.ssrc = 0xDEADBEEF;
    rtp_session_t *a = rtp_session_create(&ca);
    rtp_session_t *b = rtp_session_create(&cb);
    rtp_session_t *rx = rtp_session_create(&ca);
    uint8_t p[RTP_MAX_PACKET];
    int16_t pcm[RTP_FRAME_SAMPLES], out[RTP_FRAME_SAMPLES];
    uint32_t t = 0;
    /* Strays at k = 20 (alone), 30,31 and 33,34 (pairs split by an A packet). */
    for (uint32_t k = 0; k < 60; k++, t += 20) {
        if (k == 20 || k == 30 || k == 31 || k == 33 || k == 34) {
            gen_frame(5000 + k, pcm);
            rtp_build_audio(b, pcm, p, sizeof(p));
            rtp_on_packet(rx, p, HDR + 160, t);
        }
        gen_frame(k, pcm);
        rtp_build_audio(a, pcm, p, sizeof(p));
        if (k != 30 && k != 31 && k != 33 && k != 34) {
            feed_and_pull(rx, p, HDR + 160, t, out);
        } else {
            rtp_pull_frame(rx, out, t + 10);        /* A packet withheld */
        }
    }
    rtp_stats_t st;
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_invalid);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_dup);
    TEST_ASSERT_EQUAL_UINT32(60 - 4, st.rx_packets);    /* B never took over */
    rtp_session_destroy(a);
    rtp_session_destroy(b);
    rtp_session_destroy(rx);
}

void test_ssrc_stray_does_not_disturb_stream(void)
{
    rtp_config_t ca = make_cfg(0);
    rtp_config_t cb = make_cfg(0);
    cb.ssrc = 0xDEADBEEF;
    cb.initial_seq = 5;
    cb.initial_ts = 77;
    sim_t m;
    sim_init(&m, &ca, &ca);
    rtp_session_t *b = rtp_session_create(&cb);
    enum { N = 100 };
    static int16_t rec[N * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = N;
    sim_advance(&m, 40 * 20);
    int16_t pcm[RTP_FRAME_SAMPLES] = {1000};
    uint8_t p[RTP_MAX_PACKET];
    rtp_build_audio(b, pcm, p, sizeof(p));
    rtp_on_packet(m.rx, p, HDR + 160, m.now);
    sim_advance(&m, 60 * 20);
    check_perfect(&m, 0, N, NULL);
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.plc_frames);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_lost);
    rtp_session_destroy(b);
    sim_done(&m);
}

/* ---- tests: DTMF ------------------------------------------------------ */

static uint16_t pkt_seq(const uint8_t *p)
{
    return (uint16_t)((p[2] << 8) | p[3]);
}

static uint32_t pkt_ts(const uint8_t *p)
{
    return ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
}

static uint16_t pkt_dur(const uint8_t *p)
{
    return (uint16_t)((p[14] << 8) | p[15]);
}

void test_dtmf_sequence_100ms(void)
{
    rtp_config_t cfg = make_cfg(0);
    rtp_session_t *s = rtp_session_create(&cfg);
    int16_t pcm[RTP_FRAME_SAMPLES];
    gen_frame(1, pcm);
    uint8_t p[RTP_MAX_PACKET];

    /* two audio packets first */
    TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(s, pcm, p, sizeof(p)));
    TEST_ASSERT_EQUAL_HEX8(0x80, p[1]);
    TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(s, pcm, p, sizeof(p)));
    TEST_ASSERT_EQUAL_HEX8(0x00, p[1]);
    uint16_t seq0 = pkt_seq(p);
    uint32_t ts_audio = pkt_ts(p);

    TEST_ASSERT_TRUE(rtp_send_dtmf(s, '5', 100));
    TEST_ASSERT_FALSE(rtp_send_dtmf(s, '6', 100));          /* busy */

    uint32_t start_ts = ts_audio + 160;
    for (int i = 0; i < 8; i++) {
        size_t n = rtp_build_audio(s, pcm, p, sizeof(p));   /* pcm is ignored */
        TEST_ASSERT_EQUAL_UINT(16, n);
        TEST_ASSERT_EQUAL_HEX8(0x80, p[0]);
        TEST_ASSERT_EQUAL_HEX8(i == 0 ? (0x80 | 101) : 101, p[1]);     /* M on first */
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(seq0 + 1 + i), pkt_seq(p));
        TEST_ASSERT_EQUAL_UINT32(start_ts, pkt_ts(p));              /* constant ts */
        TEST_ASSERT_EQUAL_UINT8(5, p[12]);                          /* event '5' */
        TEST_ASSERT_EQUAL_UINT8(10, p[13] & 0x3F);                  /* volume */
        TEST_ASSERT_EQUAL_UINT8(0, p[13] & 0x40);                   /* R */
        if (i < 5) {
            TEST_ASSERT_EQUAL_UINT8(0, p[13] & 0x80);               /* no E yet */
            TEST_ASSERT_EQUAL_UINT16((uint16_t)(160 * (i + 1)), pkt_dur(p));
        } else {
            TEST_ASSERT_EQUAL_UINT8(0x80, p[13] & 0x80);            /* 3 end packets */
            TEST_ASSERT_EQUAL_UINT16(800, pkt_dur(p));
        }
    }
    /* Back to audio: seq continues, ts is past the event, marker set once. */
    TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(s, pcm, p, sizeof(p)));
    TEST_ASSERT_EQUAL_HEX8(0x80, p[1]);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(seq0 + 9), pkt_seq(p));
    TEST_ASSERT_EQUAL_UINT32(start_ts + 8 * 160, pkt_ts(p));
    TEST_ASSERT_GREATER_THAN_UINT32(800, pkt_ts(p) - start_ts);
    TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(s, pcm, p, sizeof(p)));
    TEST_ASSERT_EQUAL_HEX8(0x00, p[1]);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(seq0 + 10), pkt_seq(p));
    TEST_ASSERT_EQUAL_UINT32(start_ts + 9 * 160, pkt_ts(p));

    rtp_stats_t st;
    rtp_get_stats(s, &st);
    TEST_ASSERT_EQUAL_UINT32(2 + 8 + 2, st.tx_packets);
    TEST_ASSERT_TRUE(rtp_send_dtmf(s, '1', 20));            /* idle again */
    rtp_session_destroy(s);
}

void test_dtmf_digit_mapping_and_limits(void)
{
    static const char digits[] = "0123456789*#ABCDabcd";
    rtp_config_t cfg = make_cfg(8);
    rtp_session_t *s = rtp_session_create(&cfg);
    int16_t pcm[RTP_FRAME_SAMPLES] = {0};
    uint8_t p[RTP_MAX_PACKET];
    for (size_t i = 0; i < sizeof(digits) - 1; i++) {
        uint8_t want = (uint8_t)(i < 16 ? i : i - 4);
        TEST_ASSERT_TRUE(rtp_send_dtmf(s, digits[i], 20));
        for (int n = 0; n < 4; n++) {                       /* 1 frame + 3 end packets */
            TEST_ASSERT_EQUAL_UINT(16, rtp_build_audio(s, pcm, p, sizeof(p)));
            TEST_ASSERT_EQUAL_UINT8(want, p[12]);
            TEST_ASSERT_EQUAL_UINT8(101, p[1] & 0x7F);
            TEST_ASSERT_EQUAL_UINT16(160, pkt_dur(p));
            TEST_ASSERT_EQUAL_UINT8(n == 0 ? 0 : 0x80, p[13] & 0x80);
        }
        TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(s, pcm, p, sizeof(p)));
        TEST_ASSERT_EQUAL_HEX8(0x88, p[1]);                 /* PT 8 + marker after DTMF */
    }
    TEST_ASSERT_FALSE(rtp_send_dtmf(s, 'E', 100));
    TEST_ASSERT_FALSE(rtp_send_dtmf(s, 'x', 100));
    TEST_ASSERT_FALSE(rtp_send_dtmf(s, '\0', 100));
    /* duration 0 still sends one frame */
    TEST_ASSERT_TRUE(rtp_send_dtmf(s, '9', 0));
    for (int n = 0; n < 4; n++) {
        TEST_ASSERT_EQUAL_UINT(16, rtp_build_audio(s, pcm, p, sizeof(p)));
        TEST_ASSERT_EQUAL_UINT16(160, pkt_dur(p));
    }
    /* huge durations are clamped so the 16-bit field cannot overflow */
    TEST_ASSERT_TRUE(rtp_send_dtmf(s, '9', 65535));
    int n = 0;
    while (rtp_build_audio(s, pcm, p, sizeof(p)) == 16) {
        n++;
    }
    TEST_ASSERT_EQUAL_INT(409 + 3, n);
    rtp_session_destroy(s);
}

void test_dtmf_event_packets_ignored_on_rx(void)
{
    /* Our own DTMF stream fed to a receiver: no audio, no invalid, no crash. */
    rtp_config_t cfg = make_cfg(0);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    sim_advance(&m, 40 * 20);
    TEST_ASSERT_TRUE(rtp_send_dtmf(m.tx, '#', 100));
    sim_advance(&m, 20 * 20);
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_invalid);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_dup);
    rtp_get_stats(m.tx, &st);
    TEST_ASSERT_EQUAL_UINT32(60, st.tx_packets);
    sim_done(&m);
}

/* ---- tests: G.722 (PT 9, 16 kHz audio, 8000 Hz RTP clock) ------------ */

static double frame_snr_db(const int16_t *ref, const int16_t *got)
{
    double sig = 0, err = 0;
    for (int i = 0; i < RTP_FRAME_SAMPLES; i++) {
        double d = (double)got[i] - ref[i];
        sig += (double)ref[i] * ref[i];
        err += d * d;
    }
    return err == 0 ? 200.0 : 10.0 * log10(sig / err);
}

void test_g722_header_ts_and_roundtrip(void)
{
    rtp_config_t cfg = make_cfg(9);
    rtp_session_t *tx = rtp_session_create(&cfg);
    rtp_session_t *rx = rtp_session_create(&cfg);
    uint8_t p[3][RTP_MAX_PACKET], ref[RTP_FRAME_BYTES];
    int16_t pcm[RTP_FRAME_SAMPLES];
    for (uint32_t k = 0; k < 3; k++) {
        gen_frame(k, pcm);
        /* 320 samples in, 160 payload bytes out */
        TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(tx, pcm, p[k], sizeof(p[k])));
        TEST_ASSERT_EQUAL_HEX8(k == 0 ? 0x89 : 0x09, p[k][1]);     /* PT 9, M on first */
        /* RFC 3551: G.722's RTP clock is 8000 Hz, so +160 per 20 ms */
        TEST_ASSERT_EQUAL_UINT32(50000 + 160u * k,
                                 ((uint32_t)p[k][4] << 24) | ((uint32_t)p[k][5] << 16) |
                                 ((uint32_t)p[k][6] << 8) | p[k][7]);
        ref_payload(9, k, ref);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, p[k] + HDR, RTP_FRAME_BYTES);
    }
    for (uint32_t k = 0; k < 3; k++) {
        rtp_on_packet(rx, p[k], HDR + 160, k * 20);
    }
    int16_t out[RTP_FRAME_SAMPLES], e[RTP_FRAME_SAMPLES];
    for (uint32_t k = 0; k < 3; k++) {
        rtp_pull_frame(rx, out, 60 + k * 20);
        expected_frame(9, k, e);
        TEST_ASSERT_EQUAL_INT16_ARRAY(e, out, RTP_FRAME_SAMPLES);
    }
    rtp_stats_t st;
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(3, st.rx_packets);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_invalid);

    /* A G.711-sized packet with the wrong PT is not G.722 */
    p[0][1] = 0;
    rtp_on_packet(rx, p[0], HDR + 160, 200);
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(1, st.rx_invalid);
    rtp_session_destroy(tx);
    rtp_session_destroy(rx);
}

void test_g722_perfect_stream_with_reordering(void)
{
    rtp_config_t cfg = make_cfg(9);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.net.swap_every = 6;
    enum { N = 600 };
    static int16_t rec[N * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = N;
    sim_advance(&m, N * 20);
    size_t real;
    size_t lead = check_perfect(&m, 9, N, &real);   /* bit-exact, in order */
    TEST_ASSERT_LESS_OR_EQUAL_UINT(6, lead);
    TEST_ASSERT_EQUAL_UINT(N - lead, real);
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.plc_frames);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_lost);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_late);
    TEST_ASSERT_EQUAL_UINT16(40, st.jb_target_ms);
    sim_done(&m);
}

void test_g722_dtmf_timestamps_and_encoder_state(void)
{
    rtp_config_t cfg = make_cfg(9);
    rtp_session_t *s = rtp_session_create(&cfg);
    int16_t pcm[RTP_FRAME_SAMPLES];
    uint8_t p[RTP_MAX_PACKET], ref[RTP_FRAME_BYTES];

    for (uint32_t k = 0; k < 2; k++) {
        gen_frame(k, pcm);
        TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(s, pcm, p, sizeof(p)));
    }
    uint16_t seq0 = pkt_seq(p);
    uint32_t start_ts = pkt_ts(p) + 160;

    TEST_ASSERT_TRUE(rtp_send_dtmf(s, '#', 100));
    gen_frame(999, pcm);                            /* audio during the event: not sent */
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT(16, rtp_build_audio(s, pcm, p, sizeof(p)));
        TEST_ASSERT_EQUAL_HEX8(i == 0 ? (0x80 | 101) : 101, p[1]);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(seq0 + 1 + i), pkt_seq(p));
        TEST_ASSERT_EQUAL_UINT32(start_ts, pkt_ts(p));
        TEST_ASSERT_EQUAL_UINT8(11, p[12]);                         /* '#' */
        /* durations in 8000 Hz units, like the G.722 media clock */
        TEST_ASSERT_EQUAL_UINT16(i < 5 ? (uint16_t)(160 * (i + 1)) : 800, pkt_dur(p));
        TEST_ASSERT_EQUAL_UINT8(i < 5 ? 0 : 0x80, p[13] & 0x80);
    }
    /* Back to audio: ts past the event, and the encoder did not run during
     * it, so frame 2 encodes exactly as if it followed frame 1 directly. */
    gen_frame(2, pcm);
    TEST_ASSERT_EQUAL_UINT(HDR + 160, rtp_build_audio(s, pcm, p, sizeof(p)));
    TEST_ASSERT_EQUAL_HEX8(0x89, p[1]);
    TEST_ASSERT_EQUAL_UINT32(start_ts + 8 * 160, pkt_ts(p));
    ref_payload(9, 2, ref);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, p + HDR, RTP_FRAME_BYTES);
    rtp_session_destroy(s);
}

void test_g722_burst_loss_plc_and_recovery(void)
{
    rtp_config_t cfg = make_cfg(9);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.net.burst_from = 50;
    m.net.burst_n = 4;
    enum { N = 120 };
    static int16_t rec[N * RTP_FRAME_SAMPLES];
    m.rec = rec;
    m.rec_cap = N;
    sim_advance(&m, N * 20);

    size_t i = 0;
    int last = -1;
    while (i < N && last != 49) {
        int k = classify(9, rec_frame(&m, i), last + 1);
        if (k >= 0) {
            TEST_ASSERT_EQUAL_INT(last + 1, k);     /* exact, in order, before the loss */
            last = k;
        }
        i++;
    }
    TEST_ASSERT_EQUAL_INT(49, last);
    const int16_t *real49 = rec_frame(&m, i - 1);
    static const int32_t gain[3] = {22938, 16056, 11239};
    int16_t e[RTP_FRAME_SAMPLES];
    for (int n = 0; n < 3; n++) {
        for (int j = 0; j < RTP_FRAME_SAMPLES; j++) {
            e[j] = (int16_t)((real49[j] * gain[n]) >> 15);
        }
        TEST_ASSERT_EQUAL_INT16_ARRAY(e, rec_frame(&m, i + (size_t)n), RTP_FRAME_SAMPLES);
    }
    TEST_ASSERT_TRUE(is_silence(rec_frame(&m, i + 3)));
    TEST_ASSERT_EQUAL_INT16(0, rec_frame(&m, i + 4)[0]);  /* fade in from silence */

    /* After the gap the decoder has missed 4 frames of ADPCM updates, so the
     * output is no longer bit-exact, but it must re-converge on the sender's
     * signal quickly and stay there. */
    int first_good = -1;
    for (size_t j = i + 4; j < N - 5; j++) {
        int16_t ref[RTP_FRAME_SAMPLES];
        uint32_t k = 54 + (uint32_t)(j - (i + 4));
        expected_frame(9, k, ref);
        double snr = frame_snr_db(ref, rec_frame(&m, j));
        if (first_good < 0 && snr > 25) {
            first_good = (int)(j - (i + 4));
        }
        if (first_good >= 0 && j > i + 4 + (size_t)first_good) {
            TEST_ASSERT_TRUE_MESSAGE(snr > 25, "G.722 output diverged again after recovery");
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, first_good);
    TEST_ASSERT_LESS_OR_EQUAL_INT(10, first_good);  /* <= 200 ms */

    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(4, st.plc_frames);
    TEST_ASSERT_EQUAL_UINT32(4, st.rx_lost);
    sim_done(&m);
}

void test_g722_random_loss_stats(void)
{
    rtp_config_t cfg = make_cfg(9);
    sim_t m;
    sim_init(&m, &cfg, &cfg);
    m.net.loss_pct = 5;
    m.guard_end = 1900;
    s_rng = 12345;
    enum { N = 2000 };
    sim_advance(&m, N * 20);
    TEST_ASSERT_GREATER_THAN_UINT32(50, m.dropped);
    rtp_stats_t st;
    rtp_get_stats(m.rx, &st);
    TEST_ASSERT_EQUAL_UINT32(m.dropped, st.rx_lost);
    TEST_ASSERT_EQUAL_UINT32(m.dropped, st.plc_frames);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_late);
    TEST_ASSERT_EQUAL_UINT32(0, st.rx_invalid);
    sim_done(&m);
}

void test_g722_dropped_frames_keep_decoder_in_step(void)
{
    /* 12 frames arrive at once into a 60 ms jitter buffer: the hard cap drops
     * the oldest ones. They are decoded and discarded, so what is played
     * afterwards is still bit-exact. */
    rtp_config_t cfg = make_cfg(9);
    cfg.jb_max_ms = 60;
    rtp_session_t *tx = rtp_session_create(&cfg);
    rtp_session_t *rx = rtp_session_create(&cfg);
    uint8_t p[RTP_MAX_PACKET];
    int16_t pcm[RTP_FRAME_SAMPLES], out[RTP_FRAME_SAMPLES], e[RTP_FRAME_SAMPLES];
    uint32_t k = 0;
    for (; k < 12; k++) {
        gen_frame(k, pcm);
        rtp_build_audio(tx, pcm, p, sizeof(p));
        rtp_on_packet(rx, p, HDR + 160, 0);
    }
    uint32_t t = 10;
    rtp_pull_frame(rx, out, t);
    int first = classify(9, out, 0);
    TEST_ASSERT_GREATER_THAN_INT(0, first);         /* some frames were skipped */
    expected_frame(9, (uint32_t)first, e);
    TEST_ASSERT_EQUAL_INT16_ARRAY(e, out, RTP_FRAME_SAMPLES);
    for (int n = 1; n < 30; n++, k++) {
        t += 20;
        gen_frame(k, pcm);
        rtp_build_audio(tx, pcm, p, sizeof(p));
        rtp_on_packet(rx, p, HDR + 160, t - 5);
        rtp_pull_frame(rx, out, t);
        expected_frame(9, (uint32_t)(first + n), e);
        TEST_ASSERT_EQUAL_INT16_ARRAY(e, out, RTP_FRAME_SAMPLES);
    }
    rtp_stats_t st;
    rtp_get_stats(rx, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.plc_frames);
    rtp_session_destroy(tx);
    rtp_session_destroy(rx);
}

void test_g722_ssrc_switch_resets_decoder(void)
{
    /* A new source starts a fresh encoder: after the switch the receiver
     * must decode it bit-exactly, i.e. its decoder was reset too. */
    rtp_config_t ca = make_cfg(9);
    rtp_config_t cb = make_cfg(9);
    cb.ssrc = 0x55667788;
    cb.initial_seq = 40000;
    cb.initial_ts = 0x12345678;
    rtp_session_t *a = rtp_session_create(&ca);
    rtp_session_t *b = rtp_session_create(&cb);
    rtp_session_t *rx = rtp_session_create(&ca);
    uint8_t p[RTP_MAX_PACKET];
    int16_t pcm[RTP_FRAME_SAMPLES], out[RTP_FRAME_SAMPLES];
    uint32_t t = 0;
    for (uint32_t k = 0; k < 30; k++, t += 20) {
        gen_frame(1000 + k, pcm);                   /* unrelated audio first */
        rtp_build_audio(a, pcm, p, sizeof(p));
        feed_and_pull(rx, p, HDR + 160, t, out);
    }
    /* B sends frames 0.. from a fresh encoder. The SSRC lock withholds its
     * first two packets, so playout starts at frame 2: the reference is a
     * fresh decoder fed B's frames from 2 on. Matching it bit-exactly means
     * nothing of A's decoder state was carried over. */
    static uint8_t pl[40][RTP_FRAME_BYTES];
    static int16_t want[40][RTP_FRAME_SAMPLES];
    g722_enc_t re;
    g722_dec_t rd;
    g722_enc_init(&re);
    g722_dec_init(&rd);
    for (uint32_t k = 0; k < 40; k++) {
        gen_frame(k, pcm);
        g722_encode(&re, pcm, RTP_FRAME_SAMPLES, pl[k]);
        if (k >= 2) {
            g722_decode(&rd, pl[k], RTP_FRAME_BYTES, want[k]);
        }
    }
    int matched = 0, next = 2;
    for (uint32_t k = 0; k < 40; k++, t += 20) {
        gen_frame(k, pcm);
        rtp_build_audio(b, pcm, p, sizeof(p));
        feed_and_pull(rx, p, HDR + 160, t, out);
        for (int j = next; j < 40; j++) {
            if (frames_match(out, want[j])) {       /* in order; first may be cross-faded */
                matched++;
                next = j + 1;
                break;
            }
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(30, matched);
    rtp_session_destroy(a);
    rtp_session_destroy(b);
    rtp_session_destroy(rx);
}

/* ---- fuzz ------------------------------------------------------------- */

void test_fuzz_packets_and_pulls(void)
{
    static const uint8_t pts[] = {0, 8, 9};
    for (size_t pi = 0; pi < sizeof(pts); pi++) {
        uint8_t pt = pts[pi];
        rtp_config_t cfg = make_cfg(pt);
        rtp_session_t *tx = rtp_session_create(&cfg);
        rtp_session_t *rx = rtp_session_create(&cfg);
        s_rng = 0xC0FFEEu + pt;
        uint8_t pkt[RTP_MAX_PACKET + 128];
        int16_t pcm[RTP_FRAME_SAMPLES], out[RTP_FRAME_SAMPLES];
        uint32_t now = 0;
        for (int it = 0; it < 8000; it++) {
            gen_frame((uint32_t)it, pcm);
            if (it % 400 == 399) {
                (void)rtp_send_dtmf(tx, "0123456789*#ABCD"[rnd_range(16)], (uint16_t)rnd_range(300));
            }
            size_t len = rtp_build_audio(tx, pcm, pkt, sizeof(pkt));
            switch (rnd_range(8)) {
            case 0:         /* pure noise, random length */
                len = rnd_range(sizeof(pkt));
                for (size_t i = 0; i < len; i++) {
                    pkt[i] = (uint8_t)rnd();
                }
                break;
            case 1:         /* truncate */
                len = rnd_range((uint32_t)len + 1);
                break;
            case 2:         /* flip header bits */
                pkt[0] = (uint8_t)rnd();
                break;
            case 3:         /* random bytes in header */
                pkt[rnd_range(12)] = (uint8_t)rnd();
                pkt[rnd_range(12)] = (uint8_t)rnd();
                break;
            case 4:         /* P bit with random pad count at the end */
                pkt[0] |= 0x20;
                pkt[len - 1] = (uint8_t)rnd();
                break;
            case 5:         /* jump seq / ts */
                pkt[2] = (uint8_t)rnd();
                pkt[4] = (uint8_t)rnd();
                break;
            default:        /* leave valid */
                break;
            }
            now += rnd_range(40);
            rtp_on_packet(rx, pkt, len, now);
            if (rnd_range(3) != 0) {
                rtp_pull_frame(rx, out, now);
            }
        }
        rtp_stats_t st;
        rtp_get_stats(rx, &st);
        TEST_ASSERT_LESS_OR_EQUAL_UINT16(320, st.jb_depth_ms);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT16(40, st.jb_target_ms);
        TEST_ASSERT_LESS_OR_EQUAL_UINT16(160, st.jb_target_ms);
        TEST_ASSERT_GREATER_THAN_UINT32(0, st.rx_invalid);
        TEST_ASSERT_GREATER_THAN_UINT32(0, st.rx_packets);
        rtp_session_destroy(tx);
        rtp_session_destroy(rx);
    }
}

void test_fuzz_random_noise_50k(void)
{
    rtp_config_t cfg = make_cfg(0);
    rtp_session_t *rx = rtp_session_create(&cfg);
    s_rng = 0xBADF00Du;
    uint8_t pkt[RTP_MAX_PACKET + 64];
    int16_t out[RTP_FRAME_SAMPLES];
    uint32_t now = 0;
    for (int it = 0; it < 50000; it++) {
        size_t len = rnd_range(sizeof(pkt));
        for (size_t i = 0; i < len; i++) {
            pkt[i] = (uint8_t)rnd();
        }
        if (len > 12 && rnd_range(2)) {             /* often almost valid */
            pkt[0] = (uint8_t)(0x80 | (pkt[0] & 0x3F));
            pkt[1] = (uint8_t)((pkt[1] & 0x80) | (rnd_range(4) == 0 ? 101 : 0));
            pkt[8] = pkt[9] = pkt[10] = 0;
            pkt[11] = (uint8_t)rnd_range(3);
        }
        now += rnd_range(30);
        rtp_on_packet(rx, pkt, len, now);
        rtp_pull_frame(rx, out, now);
    }
    rtp_stats_t st;
    rtp_get_stats(rx, &st);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(320, st.jb_depth_ms);
    rtp_session_destroy(rx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_build_and_roundtrip);
    RUN_TEST(test_pcma_payload_and_pt);
    RUN_TEST(test_build_audio_small_buffer_does_not_advance);
    RUN_TEST(test_create_validation_and_defaults);
    RUN_TEST(test_rx_padding_csrc_extension);
    RUN_TEST(test_rx_malformed_counts_invalid);
    RUN_TEST(test_rx_multi_frame_packet);
    RUN_TEST(test_perfect_stream);
    RUN_TEST(test_wraparound_seq_and_ts);
    RUN_TEST(test_wraparound_with_loss_across_boundary);
    RUN_TEST(test_seq_wrap_second_cycle_lost_count);
    RUN_TEST(test_reordering_adjacent_swap);
    RUN_TEST(test_duplicates_are_counted_and_dropped);
    RUN_TEST(test_duplicate_of_already_played_packet);
    RUN_TEST(test_late_packet_is_counted_and_dropped);
    RUN_TEST(test_random_loss_5_percent);
    RUN_TEST(test_burst_loss_of_four_plc_then_silence);
    RUN_TEST(test_single_loss_crossfades_smoothly);
    RUN_TEST(test_jitter_grows_then_shrinks);
    RUN_TEST(test_jitter_moderate_has_little_concealment);
    RUN_TEST(test_long_run_depth_stays_bounded);
    RUN_TEST(test_clock_drift_receiver_fast);
    RUN_TEST(test_clock_drift_receiver_slow);
    RUN_TEST(test_burst_after_stall_resyncs);
    RUN_TEST(test_timestamp_gap_silence_suppression);
    RUN_TEST(test_ssrc_switch_after_three_packets);
    RUN_TEST(test_ssrc_stray_packets_are_ignored);
    RUN_TEST(test_ssrc_stray_does_not_disturb_stream);
    RUN_TEST(test_dtmf_sequence_100ms);
    RUN_TEST(test_dtmf_digit_mapping_and_limits);
    RUN_TEST(test_dtmf_event_packets_ignored_on_rx);
    RUN_TEST(test_g722_header_ts_and_roundtrip);
    RUN_TEST(test_g722_perfect_stream_with_reordering);
    RUN_TEST(test_g722_dtmf_timestamps_and_encoder_state);
    RUN_TEST(test_g722_burst_loss_plc_and_recovery);
    RUN_TEST(test_g722_random_loss_stats);
    RUN_TEST(test_g722_dropped_frames_keep_decoder_in_step);
    RUN_TEST(test_g722_ssrc_switch_resets_decoder);
    RUN_TEST(test_fuzz_packets_and_pulls);
    RUN_TEST(test_fuzz_random_noise_50k);
    int rc = UNITY_END();
    for (int c = 0; c < 2; c++) {
        free(s_exp[c]);
        free(s_exp_ok[c]);
    }
    return rc;
}

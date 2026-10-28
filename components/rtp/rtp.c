/* RTP session: RFC 3550 packetization, RFC 4733 DTMF send, adaptive jitter
 * buffer with PLC. Pure C17, no IDF deps. Allocation only in create/destroy.
 *
 * Receive path overview:
 *   rtp_on_packet: parse/validate -> SSRC lock/switch -> sequence stats ->
 *                  interarrival jitter -> jitter buffer slot (by RTP ts)
 *   rtp_pull_frame: one 20 ms frame per call; real audio, PLC or silence.
 * The jitter buffer is a ring of JB_SLOTS raw G.711 frames. Slot `head` is
 * the frame at RTP timestamp `play_ts`; a packet's slot is its timestamp
 * distance from `play_ts` in 20 ms frames, so reordering, duplicates and
 * timestamp gaps (silence suppression, DTMF) all fall out of the indexing. */

#include "rtp.h"

#include <stdlib.h>
#include <string.h>

#include "g711.h"

#define RTP_HDR_LEN         12
#define RTP_VERSION         2

#define JB_SLOTS            16              /* 320 ms window, power of two */
#define JB_MASK             (JB_SLOTS - 1)
#define JB_DEFAULT_MIN_MS   40
#define JB_DEFAULT_MAX_MS   160
#define JB_STALE_FRAMES     50              /* "far behind" = over 1 s old */
#define JB_STALE_RESYNC     3               /* far-behind packets before resync */
#define JB_REPRIME_FRAMES   5               /* underrun length that forces a re-prime */
#define JB_GROW_COOLDOWN    5               /* frames between stretches */
#define JB_SHRINK_HOLD      25              /* excess frames before a drop */
#define JB_SHRINK_HOLD_LOUD 100             /* ... when the audio is not quiet */
#define JB_QUIET_AVG_ABS    200             /* mean |sample| of a "quiet" frame */

#define MAX_FRAMES_PER_PKT  4               /* accept 20..80 ms packets */
#define SSRC_SWITCH_PKTS    3               /* RFC-ish: consecutive packets */
#define SEQ_MAX_DROPOUT     3000            /* RFC 3550 A.1 */

#define PLC_MAX_LOSS        3               /* then silence */
#define PLC_XFADE           32              /* samples blended after a loss */

#define DTMF_VOLUME         10              /* -10 dBm0 */
#define DTMF_END_PKTS       3               /* RFC 4733 2.5.1.4 */
#define DTMF_MAX_FRAMES     409             /* duration field is 16 bits */
#define DTMF_PKT_LEN        (RTP_HDR_LEN + 4)

/* PLC gain per consecutive loss, Q15: 0.7, 0.49, 0.343. */
static const int32_t s_plc_gain[PLC_MAX_LOSS] = {22938, 16056, 11239};

typedef struct {
    bool used;
    uint8_t data[RTP_SAMPLES_8K];       /* raw G.711, 1 byte per sample */
} jb_slot_t;

typedef enum {
    RX_IDLE,                            /* nothing received yet */
    RX_BUFFERING,                       /* waiting for depth >= target */
    RX_PLAYING,
} rx_state_t;

struct rtp_session {
    rtp_config_t cfg;

    /* TX */
    uint16_t tx_seq;
    uint32_t tx_ts;
    bool tx_marker;                     /* next audio packet gets M */
    uint32_t tx_packets;
    bool dtmf_active;
    uint8_t dtmf_event;
    uint32_t dtmf_ts;                   /* timestamp of the whole event */
    uint16_t dtmf_frames;               /* non-end packets */
    uint16_t dtmf_idx;                  /* packets sent so far */

    /* RX stream identity */
    bool locked;
    uint32_t rx_ssrc;
    uint32_t cand_ssrc;
    uint16_t cand_last_seq;
    uint8_t cand_count;

    /* RX sequence statistics (RFC 3550 A.1/A.3) */
    uint16_t base_seq, max_seq;
    uint32_t cycles;
    uint32_t received;                  /* unique packets, this stream */
    uint64_t seen;                      /* bit i: seq (max_seq - i) received */
    uint16_t bad_seq;
    bool bad_seq_valid;
    uint32_t lost_acc;                  /* loss from earlier streams/restarts */
    uint32_t rx_packets, rx_late, rx_dup, rx_invalid, plc_frames;

    /* RFC 3550 A.8 jitter, in timestamp units x16 */
    bool jit_valid;
    uint32_t prev_transit;
    int32_t j16;

    /* jitter buffer */
    rx_state_t state;
    jb_slot_t slots[JB_SLOTS];
    uint8_t head;
    uint32_t play_ts;
    uint8_t span;                       /* frames from head to newest, inclusive */
    bool anchor_fresh;                  /* nothing played from this timeline yet */
    bool reanchor;                      /* re-prime on the next arriving frame */
    uint8_t underrun_run;               /* consecutive pulls with nothing buffered */
    uint8_t stale_run;
    int min_f, max_f, target_f;         /* in frames */
    uint8_t last_depth_f;
    uint32_t since_adjust, excess_run;

    /* PLC */
    int16_t last[RTP_SAMPLES_8K];
    int16_t tail[PLC_XFADE];            /* what concealment would play next */
    uint8_t consec_loss;
    bool started;                       /* some real audio has been played */
    bool blend_pending;
    bool last_quiet;
};

/* ---- helpers ---------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* V=2, P=0, X=0, CC=0 (we never send CSRCs or extensions). */
static void rtp_write_header(uint8_t *out, bool marker, uint8_t pt, uint16_t seq,
                             uint32_t ts, uint32_t ssrc)
{
    out[0] = RTP_VERSION << 6;
    out[1] = (uint8_t)((marker ? 0x80 : 0) | (pt & 0x7F));
    wr16(out + 2, seq);
    wr32(out + 4, ts);
    wr32(out + 8, ssrc);
}

static int frames_from_ms(int ms)
{
    return (ms + RTP_FRAME_MS / 2) / RTP_FRAME_MS;
}

/* ---- create / destroy ------------------------------------------------- */

rtp_session_t *rtp_session_create(const rtp_config_t *cfg)
{
    if (cfg == NULL || (cfg->payload_type != 0 && cfg->payload_type != 8) ||
        cfg->dtmf_payload_type > 127 ||
        (cfg->dtmf_payload_type != 0 && cfg->dtmf_payload_type == cfg->payload_type)) {
        return NULL;
    }
    rtp_session_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->cfg = *cfg;
    if (s->cfg.jb_min_ms == 0) {
        s->cfg.jb_min_ms = JB_DEFAULT_MIN_MS;
    }
    if (s->cfg.jb_max_ms == 0) {
        s->cfg.jb_max_ms = JB_DEFAULT_MAX_MS;
    }
    if (s->cfg.jb_max_ms < s->cfg.jb_min_ms) {
        s->cfg.jb_max_ms = s->cfg.jb_min_ms;
    }

    s->min_f = frames_from_ms(s->cfg.jb_min_ms);
    s->max_f = frames_from_ms(s->cfg.jb_max_ms);
    if (s->min_f < 1) {
        s->min_f = 1;
    }
    if (s->max_f > JB_SLOTS - 2) {          /* leave room for packets past the target */
        s->max_f = JB_SLOTS - 2;
    }
    if (s->min_f > s->max_f) {
        s->min_f = s->max_f;
    }
    s->target_f = s->min_f;

    s->tx_seq = cfg->initial_seq;
    s->tx_ts = cfg->initial_ts;
    s->tx_marker = true;
    return s;
}

void rtp_session_destroy(rtp_session_t *s)
{
    free(s);
}

/* ---- TX --------------------------------------------------------------- */

static size_t rtp_build_dtmf_packet(rtp_session_t *s, uint8_t *out)
{
    bool first = (s->dtmf_idx == 0);
    bool end = (s->dtmf_idx >= s->dtmf_frames);
    if (first) {
        s->dtmf_ts = s->tx_ts;
    }
    uint16_t dur = end ? (uint16_t)(s->dtmf_frames * RTP_SAMPLES_8K)
                       : (uint16_t)((s->dtmf_idx + 1) * RTP_SAMPLES_8K);

    /* RFC 4733 2.3: all packets of one event share the start timestamp. */
    rtp_write_header(out, first, s->cfg.dtmf_payload_type, s->tx_seq, s->dtmf_ts, s->cfg.ssrc);
    out[12] = s->dtmf_event;
    out[13] = (uint8_t)((end ? 0x80 : 0) | DTMF_VOLUME);   /* E, R=0, volume */
    wr16(out + 14, dur);

    s->tx_seq++;
    s->tx_ts += RTP_SAMPLES_8K;             /* media clock keeps running */
    s->tx_packets++;
    s->dtmf_idx++;
    if (s->dtmf_idx >= s->dtmf_frames + DTMF_END_PKTS) {
        s->dtmf_active = false;
        s->tx_marker = true;                /* first audio after the event */
    }
    return DTMF_PKT_LEN;
}

size_t rtp_build_audio(rtp_session_t *s, const int16_t pcm[RTP_SAMPLES_8K],
                       uint8_t *out, size_t out_cap)
{
    if (s == NULL || out == NULL) {
        return 0;
    }
    if (s->dtmf_active) {
        return out_cap >= DTMF_PKT_LEN ? rtp_build_dtmf_packet(s, out) : 0;
    }
    if (pcm == NULL || out_cap < RTP_HDR_LEN + RTP_SAMPLES_8K) {
        return 0;
    }
    rtp_write_header(out, s->tx_marker, s->cfg.payload_type, s->tx_seq, s->tx_ts, s->cfg.ssrc);
    if (s->cfg.payload_type == 8) {
        g711_alaw_encode_buf(pcm, out + RTP_HDR_LEN, RTP_SAMPLES_8K);
    } else {
        g711_ulaw_encode_buf(pcm, out + RTP_HDR_LEN, RTP_SAMPLES_8K);
    }
    s->tx_marker = false;
    s->tx_seq++;
    s->tx_ts += RTP_SAMPLES_8K;
    s->tx_packets++;
    return RTP_HDR_LEN + RTP_SAMPLES_8K;
}

bool rtp_send_dtmf(rtp_session_t *s, char digit, uint16_t duration_ms)
{
    static const char k_digits[] = "0123456789*#ABCD";
    if (s == NULL || s->cfg.dtmf_payload_type == 0 || s->dtmf_active) {
        return false;
    }
    if (digit >= 'a' && digit <= 'd') {
        digit = (char)(digit - 'a' + 'A');
    }
    const char *p = (digit != '\0') ? strchr(k_digits, digit) : NULL;
    if (p == NULL) {
        return false;
    }
    unsigned frames = ((unsigned)duration_ms + RTP_FRAME_MS - 1) / RTP_FRAME_MS;
    if (frames < 1) {
        frames = 1;
    }
    if (frames > DTMF_MAX_FRAMES) {
        frames = DTMF_MAX_FRAMES;
    }
    s->dtmf_event = (uint8_t)(p - k_digits);
    s->dtmf_frames = (uint16_t)frames;
    s->dtmf_idx = 0;
    s->dtmf_active = true;
    return true;
}

/* ---- RX: sequence statistics ------------------------------------------ */

typedef enum {
    SEQ_NEW,        /* counted */
    SEQ_DUP,
    SEQ_OLD,        /* too old to classify; leave to the jitter buffer */
    SEQ_DROP,       /* suspicious jump, wait for confirmation */
} seq_res_t;

static void seq_init(rtp_session_t *s, uint16_t seq)
{
    s->base_seq = seq;
    s->max_seq = seq;
    s->cycles = 0;
    s->received = 0;
    s->seen = 0;
    s->bad_seq_valid = false;
}

static uint32_t seq_lost(const rtp_session_t *s)
{
    uint32_t expected = s->cycles + s->max_seq - s->base_seq + 1;
    return expected > s->received ? expected - s->received : 0;
}

static seq_res_t seq_update(rtp_session_t *s, uint16_t seq)
{
    int d = (int16_t)(uint16_t)(seq - s->max_seq);
    if (d > 0) {
        if (d > SEQ_MAX_DROPOUT) {
            goto jump;
        }
        if (seq < s->max_seq) {
            s->cycles += 0x10000;           /* wrapped */
        }
        s->seen = (d >= 64) ? 0 : (s->seen << d);
        s->seen |= 1;
        s->max_seq = seq;
        s->received++;
        s->bad_seq_valid = false;
        return SEQ_NEW;
    }
    int back = -d;
    if (back >= 64) {
        if (back > SEQ_MAX_DROPOUT) {
            goto jump;
        }
        return SEQ_OLD;
    }
    uint64_t bit = (uint64_t)1 << back;
    if (s->seen & bit) {
        return SEQ_DUP;
    }
    s->seen |= bit;
    s->received++;
    return SEQ_NEW;

jump:
    /* RFC 3550 A.1: a jump is accepted as a source restart only when the
     * next packet follows it in sequence. */
    if (s->bad_seq_valid && seq == s->bad_seq) {
        s->lost_acc += seq_lost(s);
        seq_init(s, seq);
        s->seen = 1;
        s->received = 1;
        return SEQ_NEW;
    }
    s->bad_seq = (uint16_t)(seq + 1);
    s->bad_seq_valid = true;
    return SEQ_DROP;
}

/* ---- RX: jitter estimate and target ----------------------------------- */

static void rtp_update_target(rtp_session_t *s)
{
    /* target = 2.5 * jitter + 20 ms. j16 is jitter in ts units x16 (8 ts/ms):
     * 2.5 * j16 / (16 * 8) = 5 * j16 / 256. */
    int32_t ms = (5 * s->j16) / 256 + RTP_FRAME_MS;
    int f = frames_from_ms(ms);
    if (f < s->min_f) {
        f = s->min_f;
    }
    if (f > s->max_f) {
        f = s->max_f;
    }
    s->target_f = f;
}

static void rtp_update_jitter(rtp_session_t *s, uint32_t ts, bool marker, uint32_t now_ms)
{
    uint32_t transit = now_ms * 8u - ts;    /* arrival in 8 kHz units, mod 2^32 */
    if (s->jit_valid && !marker) {          /* a talkspurt start restarts the clock */
        int32_t d = (int32_t)(transit - s->prev_transit);
        if (d < 0) {
            d = -d < 0 ? INT32_MAX : -d;
        }
        if (d > 8000) {
            d = 8000;                       /* ignore gross jumps (1 s) */
        }
        s->j16 += d - ((s->j16 + 8) >> 4);  /* A.8: J += (|D| - J) / 16 */
    }
    s->prev_transit = transit;
    s->jit_valid = true;
    rtp_update_target(s);
}

/* ---- jitter buffer ---------------------------------------------------- */

static void jb_reset(rtp_session_t *s, uint32_t ts)
{
    memset(s->slots, 0, sizeof(s->slots));
    s->head = 0;
    s->span = 0;
    s->play_ts = ts;
    s->state = RX_BUFFERING;
    s->anchor_fresh = true;
    s->reanchor = false;
    s->underrun_run = 0;
    s->stale_run = 0;
    s->excess_run = 0;
    s->since_adjust = JB_GROW_COOLDOWN;
}

/* Move the playout point back by `back` empty slots. */
static void jb_rewind(rtp_session_t *s, int32_t back)
{
    s->head = (uint8_t)((s->head - back) & JB_MASK);
    s->play_ts -= (uint32_t)back * RTP_SAMPLES_8K;
    s->span = (uint8_t)(s->span + back);
}

/* Consume (or skip) the head frame. */
static void jb_advance(rtp_session_t *s)
{
    s->slots[s->head].used = false;
    s->head = (uint8_t)((s->head + 1) & JB_MASK);
    s->play_ts += RTP_SAMPLES_8K;
    if (s->span > 0) {
        s->span--;
    }
}

/* Slot distance of `ts` from the playout point, rounded to the nearest frame. */
static int32_t jb_index(const rtp_session_t *s, uint32_t ts)
{
    int64_t v = (int64_t)(int32_t)(ts - s->play_ts) + RTP_SAMPLES_8K / 2;
    int64_t q = v / RTP_SAMPLES_8K;
    if (v % RTP_SAMPLES_8K < 0) {
        q--;
    }
    return (int32_t)q;
}

static void jb_put(rtp_session_t *s, uint32_t ts, const uint8_t *data)
{
    if (s->state == RX_IDLE || s->reanchor) {
        jb_reset(s, ts);
    }
    int32_t idx = jb_index(s, ts);
    if (idx < 0) {
        int32_t back = -idx;
        if (s->span + back <= JB_SLOTS &&
            ((s->state == RX_BUFFERING && s->anchor_fresh) ||
             (s->state == RX_PLAYING && back <= s->underrun_run))) {
            /* Priming: an earlier frame arrived out of order. Or playing: the
             * frame we were starved of turned up after `back` slots that were
             * concealed only because nothing at all was buffered. Take it back
             * (stretches playout by `back` frames) instead of dropping it. */
            jb_rewind(s, back);
            idx = 0;
        } else if (back > JB_STALE_FRAMES && ++s->stale_run >= JB_STALE_RESYNC) {
            jb_reset(s, ts);                /* sender timeline moved back */
            idx = 0;
        } else {
            s->rx_late++;
            return;
        }
    } else if (idx >= JB_SLOTS) {
        jb_reset(s, ts);                    /* gap too big for the window */
        idx = 0;
    }
    s->stale_run = 0;

    jb_slot_t *slot = &s->slots[(s->head + idx) & JB_MASK];
    if (slot->used) {
        s->rx_dup++;
        return;
    }
    slot->used = true;
    memcpy(slot->data, data, RTP_SAMPLES_8K);
    if (idx + 1 > s->span) {
        s->span = (uint8_t)(idx + 1);
    }
    if (s->state != RX_PLAYING) {
        s->last_depth_f = s->span;
    }
}

/* ---- RX: stream lock / packet entry ----------------------------------- */

/* New stream (first packet, or SSRC switch). Totals carry over. */
static void rx_new_stream(rtp_session_t *s, uint32_t ssrc, uint16_t seq)
{
    if (s->locked) {
        s->lost_acc += seq_lost(s);
    }
    seq_init(s, seq);
    s->locked = true;
    s->rx_ssrc = ssrc;
    s->cand_count = 0;
    s->jit_valid = false;
    s->j16 = 0;
    s->target_f = s->min_f;
    s->state = RX_IDLE;                     /* jb_put re-anchors on the next frame */
}

void rtp_on_packet(rtp_session_t *s, const uint8_t *pkt, size_t len, uint32_t now_ms)
{
    if (s == NULL || pkt == NULL) {
        return;
    }
    /* RFC 3550 5.1 fixed header, then CSRC list, extension, padding. */
    if (len < RTP_HDR_LEN || (pkt[0] >> 6) != RTP_VERSION) {
        s->rx_invalid++;
        return;
    }
    size_t hdr = RTP_HDR_LEN + 4u * (pkt[0] & 0x0F);
    if (pkt[0] & 0x10) {
        if (hdr + 4 > len) {
            s->rx_invalid++;
            return;
        }
        hdr += 4 + 4u * rd16(pkt + hdr + 2);        /* length counts 32-bit words */
    }
    size_t pad = 0;
    if (pkt[0] & 0x20) {
        pad = pkt[len - 1];
        if (pad == 0 || hdr + pad > len) {
            s->rx_invalid++;
            return;
        }
    }
    if (hdr > len) {
        s->rx_invalid++;
        return;
    }
    size_t plen = len - hdr - pad;
    bool marker = (pkt[1] & 0x80) != 0;
    uint8_t pt = pkt[1] & 0x7F;
    uint16_t seq = rd16(pkt + 2);
    uint32_t ts = rd32(pkt + 4);
    uint32_t ssrc = rd32(pkt + 8);

    if (s->cfg.dtmf_payload_type != 0 && pt == s->cfg.dtmf_payload_type) {
        return;                                     /* telephone-event RX: ignored */
    }
    if (pt != s->cfg.payload_type || plen == 0 || plen % RTP_SAMPLES_8K != 0 ||
        plen / RTP_SAMPLES_8K > MAX_FRAMES_PER_PKT) {
        s->rx_invalid++;
        return;
    }

    if (!s->locked) {
        rx_new_stream(s, ssrc, seq);
    } else if (ssrc != s->rx_ssrc) {
        /* Switch after SSRC_SWITCH_PKTS consecutive in-sequence packets. */
        if (s->cand_count > 0 && ssrc == s->cand_ssrc &&
            seq == (uint16_t)(s->cand_last_seq + 1)) {
            s->cand_count++;
        } else {
            s->cand_ssrc = ssrc;
            s->cand_count = 1;
        }
        s->cand_last_seq = seq;
        if (s->cand_count < SSRC_SWITCH_PKTS) {
            return;
        }
        rx_new_stream(s, ssrc, seq);
    } else {
        s->cand_count = 0;
    }

    seq_res_t r = seq_update(s, seq);
    if (r == SEQ_DROP) {
        s->rx_invalid++;
        return;
    }
    s->rx_packets++;
    if (r == SEQ_DUP) {
        s->rx_dup++;
        return;
    }
    rtp_update_jitter(s, ts, marker, now_ms);
    for (size_t i = 0; i < plen / RTP_SAMPLES_8K; i++) {
        jb_put(s, ts + (uint32_t)(i * RTP_SAMPLES_8K), pkt + hdr + i * RTP_SAMPLES_8K);
    }
}

/* ---- RX: playout ------------------------------------------------------ */

/* One concealed frame: last good frame x0.7 per consecutive loss, then
 * silence (after PLC_MAX_LOSS). `tail` remembers the continuation so the
 * next real frame can be cross-faded in. */
static void plc_fill(rtp_session_t *s, int16_t *pcm)
{
    unsigned n = s->consec_loss;
    if (s->started && n < PLC_MAX_LOSS) {
        int32_t g = s_plc_gain[n];
        int32_t gn = (n + 1 < PLC_MAX_LOSS) ? s_plc_gain[n + 1] : 0;
        for (int i = 0; i < RTP_SAMPLES_8K; i++) {
            pcm[i] = (int16_t)((s->last[i] * g) >> 15);
        }
        for (int i = 0; i < PLC_XFADE; i++) {
            s->tail[i] = (int16_t)((s->last[i] * gn) >> 15);
        }
    } else {
        memset(pcm, 0, RTP_SAMPLES_8K * sizeof(pcm[0]));
        memset(s->tail, 0, sizeof(s->tail));
    }
    if (s->consec_loss < UINT8_MAX) {
        s->consec_loss++;
    }
    if (s->started) {
        s->plc_frames++;
        s->blend_pending = true;
    }
}

static void play_real(rtp_session_t *s, const jb_slot_t *slot, int16_t *pcm)
{
    if (s->cfg.payload_type == 8) {
        g711_alaw_decode_buf(slot->data, pcm, RTP_SAMPLES_8K);
    } else {
        g711_ulaw_decode_buf(slot->data, pcm, RTP_SAMPLES_8K);
    }
    if (s->blend_pending) {
        for (int i = 0; i < PLC_XFADE; i++) {
            pcm[i] = (int16_t)((s->tail[i] * (PLC_XFADE - i) + pcm[i] * i) / PLC_XFADE);
        }
        s->blend_pending = false;
    }
    memcpy(s->last, pcm, sizeof(s->last));
    int32_t sum = 0;
    for (int i = 0; i < RTP_SAMPLES_8K; i++) {
        sum += pcm[i] < 0 ? -pcm[i] : pcm[i];
    }
    s->last_quiet = (sum / RTP_SAMPLES_8K) < JB_QUIET_AVG_ABS;
    s->consec_loss = 0;
    s->started = true;
}

void rtp_pull_frame(rtp_session_t *s, int16_t pcm[RTP_SAMPLES_8K], uint32_t now_ms)
{
    (void)now_ms;
    if (s == NULL || pcm == NULL) {
        return;
    }
    if (s->state == RX_IDLE) {
        memset(pcm, 0, RTP_SAMPLES_8K * sizeof(pcm[0]));
        return;
    }
    s->last_depth_f = s->span;

    if (s->state == RX_BUFFERING) {
        if (s->span < s->target_f) {
            plc_fill(s, pcm);               /* silence, or the tail of a fade-out */
            return;
        }
        /* Start (or restart after an underrun) at the oldest frame we hold;
         * frames that never arrived before it are not worth waiting for. */
        while (s->span > 0 && !s->slots[s->head].used) {
            jb_advance(s);
        }
        s->state = RX_PLAYING;
        s->anchor_fresh = false;
        s->since_adjust = JB_GROW_COOLDOWN;
        s->excess_run = 0;
    } else if (s->span == 0) {
        /* Underrun: nothing buffered at all. Conceal this slot and move on
         * (the sender's timeline does not wait); a frame that was merely late
         * is taken back by jb_put. After a few frames, give up on the old
         * timeline and re-prime on whatever arrives next. */
        plc_fill(s, pcm);
        jb_advance(s);
        if (++s->underrun_run >= JB_REPRIME_FRAMES) {
            s->state = RX_BUFFERING;
            s->reanchor = true;
        }
        return;
    }
    s->underrun_run = 0;

    /* Hard cap at jb_max. */
    while (s->span > s->max_f) {
        jb_advance(s);
    }
    s->since_adjust++;

    /* Grow: stretch playout with one concealed frame, without consuming.
     * The band is target-1..target+1 frames: arrival phase vs. pull phase
     * alone moves the depth by one frame, which must not cause concealment. */
    if (s->span + 1 < s->target_f && s->since_adjust >= JB_GROW_COOLDOWN) {
        s->since_adjust = 0;
        s->excess_run = 0;
        plc_fill(s, pcm);
        return;
    }
    /* Shrink: after a sustained excess, drop one frame (preferably a quiet one). */
    if (s->span >= s->target_f + 2) {
        s->excess_run++;
        if (s->excess_run >= JB_SHRINK_HOLD &&
            (s->last_quiet || s->excess_run >= JB_SHRINK_HOLD_LOUD)) {
            jb_advance(s);
            s->excess_run = 0;
            s->since_adjust = 0;
        }
    } else {
        s->excess_run = 0;
    }

    const jb_slot_t *slot = &s->slots[s->head];
    if (slot->used) {
        play_real(s, slot, pcm);
    } else {
        s->excess_run = 0;
        plc_fill(s, pcm);
    }
    jb_advance(s);
}

void rtp_get_stats(const rtp_session_t *s, rtp_stats_t *out)
{
    if (s == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->rx_packets = s->rx_packets;
    out->rx_lost = s->lost_acc + (s->locked ? seq_lost(s) : 0);
    out->rx_late = s->rx_late;
    out->rx_dup = s->rx_dup;
    out->rx_invalid = s->rx_invalid;
    out->tx_packets = s->tx_packets;
    out->plc_frames = s->plc_frames;
    out->jb_target_ms = (uint16_t)(s->target_f * RTP_FRAME_MS);
    out->jb_depth_ms = (uint16_t)(s->last_depth_f * RTP_FRAME_MS);
    out->jitter_ms = (uint32_t)(((s->j16 >> 4) + 4) / 8);
}

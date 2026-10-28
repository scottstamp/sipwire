/* Host tests: phone call-control policy + UI-bridge model building. */

#include <string.h>

#include "phone_logic.h"
#include "ui_bridge_model.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- tones ---- */

static void test_tone_per_state(void)
{
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE, phone_tone_for(SIP_UA_CALL_IDLE, false, false, 0));
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE, phone_tone_for(SIP_UA_CALL_DIALING, true, false, 0));
    TEST_ASSERT_EQUAL(PHONE_TONE_RINGBACK, phone_tone_for(SIP_UA_CALL_RINGING_OUT, true, false, 0));
    TEST_ASSERT_EQUAL(PHONE_TONE_RING, phone_tone_for(SIP_UA_CALL_RINGING_IN, false, false, 0));
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE, phone_tone_for(SIP_UA_CALL_ACTIVE, true, false, 0));
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE, phone_tone_for(SIP_UA_CALL_HELD, true, false, 0));
}

static void test_early_media_replaces_ringback(void)
{
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE, phone_tone_for(SIP_UA_CALL_RINGING_OUT, true, true, 0));
}

static void test_busy_tone_only_for_refused_outgoing(void)
{
    TEST_ASSERT_EQUAL(PHONE_TONE_BUSY, phone_tone_for(SIP_UA_CALL_ENDED, true, false, 486));
    TEST_ASSERT_EQUAL(PHONE_TONE_BUSY, phone_tone_for(SIP_UA_CALL_ENDED, true, false, 600));
    TEST_ASSERT_EQUAL(PHONE_TONE_BUSY, phone_tone_for(SIP_UA_CALL_ENDED, true, false, 603));
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE, phone_tone_for(SIP_UA_CALL_ENDED, true, false, 404));
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE, phone_tone_for(SIP_UA_CALL_ENDED, true, false, 0));
    /* we rejected an incoming call with 486: no busy tone for us */
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE, phone_tone_for(SIP_UA_CALL_ENDED, false, false, 486));
}

/* ---- call end ---- */

static void test_end_codes(void)
{
    TEST_ASSERT_EQUAL(0, phone_end_code(SIP_END_LOCAL, 0));
    TEST_ASSERT_EQUAL(0, phone_end_code(SIP_END_REMOTE, 200));
    TEST_ASSERT_EQUAL(486, phone_end_code(SIP_END_REJECTED, 486));
    TEST_ASSERT_EQUAL(404, phone_end_code(SIP_END_REJECTED, 404));
    TEST_ASSERT_EQUAL(487, phone_end_code(SIP_END_CANCELLED, 0));
    TEST_ASSERT_EQUAL(408, phone_end_code(SIP_END_TIMEOUT, 0));
    TEST_ASSERT_EQUAL(408, phone_end_code(SIP_END_TIMEOUT, 408));
    TEST_ASSERT_EQUAL(488, phone_end_code(SIP_END_ERROR, 488));
}

static void test_ended_hold_time(void)
{
    TEST_ASSERT_EQUAL_UINT32(PHONE_ENDED_HOLD_MS, phone_ended_hold_ms(true, 0));
    TEST_ASSERT_EQUAL_UINT32(PHONE_ENDED_HOLD_MS, phone_ended_hold_ms(false, 486));
    TEST_ASSERT_EQUAL_UINT32(PHONE_BUSY_HOLD_MS, phone_ended_hold_ms(true, 486));
    TEST_ASSERT_EQUAL_UINT32(2000, PHONE_ENDED_HOLD_MS);
    TEST_ASSERT_EQUAL_UINT32(3000, PHONE_BUSY_HOLD_MS);
}

/* ---- media flags ---- */

static void flags(sip_ua_call_state_t st, sip_media_dir_t dir, bool early, bool want_send, bool want_play)
{
    bool send = !want_send, play = !want_play;
    phone_media_flags(st, dir, early, &send, &play);
    TEST_ASSERT_EQUAL(want_send, send);
    TEST_ASSERT_EQUAL(want_play, play);
}

static void test_media_flags(void)
{
    flags(SIP_UA_CALL_ACTIVE, SIP_DIR_SENDRECV, false, true, true);
    flags(SIP_UA_CALL_ACTIVE, SIP_DIR_RECVONLY, false, false, true);   /* remote holds us: MOH */
    flags(SIP_UA_CALL_ACTIVE, SIP_DIR_SENDONLY, false, true, false);
    flags(SIP_UA_CALL_ACTIVE, SIP_DIR_INACTIVE, false, false, false);
    flags(SIP_UA_CALL_HELD, SIP_DIR_SENDONLY, false, false, false);    /* we hold */
    flags(SIP_UA_CALL_HELD, SIP_DIR_INACTIVE, false, false, false);
    flags(SIP_UA_CALL_RINGING_OUT, SIP_DIR_SENDRECV, true, false, true); /* early media: listen only */
    flags(SIP_UA_CALL_RINGING_OUT, SIP_DIR_SENDRECV, false, false, false);
    flags(SIP_UA_CALL_DIALING, SIP_DIR_SENDRECV, true, false, true);
    flags(SIP_UA_CALL_RINGING_IN, SIP_DIR_SENDRECV, false, false, false);
    flags(SIP_UA_CALL_ENDED, SIP_DIR_SENDRECV, false, false, false);
    flags(SIP_UA_CALL_IDLE, SIP_DIR_SENDRECV, false, false, false);
}

static void test_rtp_source_filter(void)
{
    uint8_t rtp[12] = { 0x80, 0x00 };
    uint8_t bad[12] = { 0x40, 0x00 };
    const uint32_t remote = 0xC0A80105, server = 0x0A000202;
    TEST_ASSERT_TRUE(phone_rtp_source_ok(remote, server, remote, rtp, sizeof(rtp)));
    TEST_ASSERT_TRUE(phone_rtp_source_ok(remote, server, server, rtp, sizeof(rtp)));   /* PBX behind NAT */
    TEST_ASSERT_FALSE(phone_rtp_source_ok(remote, server, remote + 1, rtp, sizeof(rtp))); /* other host */
    TEST_ASSERT_FALSE(phone_rtp_source_ok(remote, 0, server, rtp, sizeof(rtp)));
    TEST_ASSERT_FALSE(phone_rtp_source_ok(remote, server, remote, bad, sizeof(bad)));  /* not v2 */
    TEST_ASSERT_FALSE(phone_rtp_source_ok(remote, server, remote, rtp, 11));           /* runt */
    TEST_ASSERT_FALSE(phone_rtp_source_ok(0, 0, 0, rtp, sizeof(rtp)));                 /* c=0.0.0.0 */
    TEST_ASSERT_FALSE(phone_rtp_source_ok(remote, server, remote, NULL, 12));
}

/* ---- RTP ports ---- */

static void test_port_alloc_range_and_wrap(void)
{
    phone_port_alloc_t a;
    phone_port_alloc_init(&a, 0);
    TEST_ASSERT_EQUAL_UINT16(16384, phone_port_alloc_next(&a, 0));
    TEST_ASSERT_EQUAL_UINT16(16386, phone_port_alloc_next(&a, 0));
    TEST_ASSERT_EQUAL(50, PHONE_RTP_PORT_COUNT);
    bool seen[PHONE_RTP_PORT_COUNT] = { false };
    seen[0] = seen[1] = true;
    for (unsigned i = 2; i < PHONE_RTP_PORT_COUNT; i++) {
        uint16_t p = phone_port_alloc_next(&a, 0);
        TEST_ASSERT_TRUE(p >= PHONE_RTP_PORT_MIN && p <= PHONE_RTP_PORT_MAX);
        TEST_ASSERT_EQUAL(0, p % 2);
        seen[(p - PHONE_RTP_PORT_MIN) / 2] = true;
    }
    for (unsigned i = 0; i < PHONE_RTP_PORT_COUNT; i++) {
        TEST_ASSERT_TRUE(seen[i]);
    }
    TEST_ASSERT_EQUAL_UINT16(16384, phone_port_alloc_next(&a, 0));  /* wrapped */
}

static void test_port_alloc_seed_and_skip(void)
{
    phone_port_alloc_t a;
    phone_port_alloc_init(&a, 49 + 50 * 7);             /* seed reduced modulo the range */
    TEST_ASSERT_EQUAL_UINT16(16482, phone_port_alloc_next(&a, 0));
    phone_port_alloc_init(&a, 3);
    TEST_ASSERT_EQUAL_UINT16(16392, phone_port_alloc_next(&a, 16390));  /* 16390 in use */
    phone_port_alloc_init(&a, 3);
    TEST_ASSERT_EQUAL_UINT16(16390, phone_port_alloc_next(&a, 16384));
}

/* ---- DTMF ---- */

static void test_dtmf_valid(void)
{
    const char *ok = "0123456789*#ABCDabcd";
    for (const char *p = ok; *p; p++) {
        TEST_ASSERT_TRUE(phone_dtmf_valid(*p));
    }
    TEST_ASSERT_FALSE(phone_dtmf_valid('E'));
    TEST_ASSERT_FALSE(phone_dtmf_valid(','));
    TEST_ASSERT_FALSE(phone_dtmf_valid(' '));
    TEST_ASSERT_FALSE(phone_dtmf_valid('\0'));
}

static void test_dtmf_sequence_spacing(void)
{
    phone_dtmf_seq_t s;
    phone_dtmf_seq_clear(&s);
    TEST_ASSERT_EQUAL_CHAR(0, phone_dtmf_seq_due(&s, 0));
    TEST_ASSERT_TRUE(phone_dtmf_seq_push(&s, "1a#"));

    TEST_ASSERT_EQUAL_CHAR('1', phone_dtmf_seq_due(&s, 1000));
    phone_dtmf_seq_consume(&s, 1000);
    TEST_ASSERT_EQUAL_CHAR(0, phone_dtmf_seq_due(&s, 1000 + PHONE_DTMF_GAP_MS - 1));
    TEST_ASSERT_EQUAL_CHAR('A', phone_dtmf_seq_due(&s, 1000 + PHONE_DTMF_GAP_MS));   /* upper-cased */
    /* busy: not consumed, still due a bit later */
    TEST_ASSERT_EQUAL_CHAR('A', phone_dtmf_seq_due(&s, 1000 + PHONE_DTMF_GAP_MS + 20));
    phone_dtmf_seq_consume(&s, 1170);
    TEST_ASSERT_EQUAL_CHAR(0, phone_dtmf_seq_due(&s, 1170 + PHONE_DTMF_GAP_MS - 1));
    TEST_ASSERT_EQUAL_CHAR('#', phone_dtmf_seq_due(&s, 1170 + PHONE_DTMF_GAP_MS));
    phone_dtmf_seq_consume(&s, 1320);
    TEST_ASSERT_EQUAL_CHAR(0, phone_dtmf_seq_due(&s, 99999));
    phone_dtmf_seq_consume(&s, 99999);                  /* empty: no-op */
    TEST_ASSERT_EQUAL(0, s.len);
}

static void test_dtmf_event_fits_in_gap(void)
{
    /* event frames + 3 end packets (20 ms each) must finish before the next start */
    unsigned frames = (PHONE_DTMF_DURATION_MS + 19) / 20 + 3;
    TEST_ASSERT_LESS_OR_EQUAL(PHONE_DTMF_GAP_MS, frames * 20);
}

static void test_dtmf_push_rejects(void)
{
    phone_dtmf_seq_t s;
    phone_dtmf_seq_clear(&s);
    TEST_ASSERT_FALSE(phone_dtmf_seq_push(&s, ""));
    TEST_ASSERT_FALSE(phone_dtmf_seq_push(&s, NULL));
    TEST_ASSERT_FALSE(phone_dtmf_seq_push(&s, "12x3"));
    TEST_ASSERT_EQUAL(0, s.len);                        /* nothing partially queued */

    char many[PHONE_DTMF_QUEUE + 1];
    memset(many, '5', PHONE_DTMF_QUEUE);
    many[PHONE_DTMF_QUEUE] = '\0';
    TEST_ASSERT_TRUE(phone_dtmf_seq_push(&s, many));
    TEST_ASSERT_FALSE(phone_dtmf_seq_push(&s, "1"));    /* full */
    phone_dtmf_seq_consume(&s, 0);
    TEST_ASSERT_TRUE(phone_dtmf_seq_push(&s, "9"));     /* wraps around the ring */
    for (int i = 0; i < PHONE_DTMF_QUEUE - 1; i++) {
        TEST_ASSERT_EQUAL_CHAR('5', phone_dtmf_seq_due(&s, UINT64_MAX));
        phone_dtmf_seq_consume(&s, 0);
    }
    TEST_ASSERT_EQUAL_CHAR('9', phone_dtmf_seq_due(&s, UINT64_MAX));
}

/* ---- volume ---- */

static void test_volume_kind(void)
{
    TEST_ASSERT_EQUAL(PHONE_VOL_RING, phone_volume_kind(SIP_UA_CALL_IDLE, false));
    TEST_ASSERT_EQUAL(PHONE_VOL_RING, phone_volume_kind(SIP_UA_CALL_RINGING_IN, true));
    TEST_ASSERT_EQUAL(PHONE_VOL_CALL, phone_volume_kind(SIP_UA_CALL_ACTIVE, false));
    TEST_ASSERT_EQUAL(PHONE_VOL_SPEAKER, phone_volume_kind(SIP_UA_CALL_ACTIVE, true));
    TEST_ASSERT_EQUAL(PHONE_VOL_CALL, phone_volume_kind(SIP_UA_CALL_RINGING_OUT, false));
    TEST_ASSERT_EQUAL(PHONE_VOL_CALL, phone_volume_kind(SIP_UA_CALL_ENDED, false));
}

/* ---- UI bridge model ---- */

static void test_account_format(void)
{
    char out[64];
    ui_bridge_format_account(out, sizeof(out), "Alice", "1001", "pbx.lan");
    TEST_ASSERT_EQUAL_STRING("Alice <1001@pbx.lan>", out);
    ui_bridge_format_account(out, sizeof(out), "", "1001", "pbx.lan");
    TEST_ASSERT_EQUAL_STRING("1001@pbx.lan", out);
    ui_bridge_format_account(out, sizeof(out), NULL, "1001", "");
    TEST_ASSERT_EQUAL_STRING("1001", out);
    ui_bridge_format_account(out, sizeof(out), "Alice", "", "pbx.lan");
    TEST_ASSERT_EQUAL_STRING("", out);

    char small[8];
    ui_bridge_format_account(small, sizeof(small), "Alice", "1001", "pbx.lan");
    TEST_ASSERT_EQUAL_STRING("Alice <", small);         /* truncated, terminated */

    char big_name[64], big_srv[64];
    memset(big_name, 'n', 63);
    big_name[63] = '\0';
    memset(big_srv, 's', 63);
    big_srv[63] = '\0';
    ui_bridge_format_account(out, sizeof(out), big_name, "1001", big_srv);
    TEST_ASSERT_EQUAL(63, strlen(out));
}

static void test_call_seconds(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, ui_bridge_call_seconds(CALL_ACTIVE, 0, 5000000));
    TEST_ASSERT_EQUAL_UINT32(4, ui_bridge_call_seconds(CALL_ACTIVE, 1000000, 5999999));
    TEST_ASSERT_EQUAL_UINT32(5, ui_bridge_call_seconds(CALL_HELD, 1000000, 6000000));
    TEST_ASSERT_EQUAL_UINT32(0, ui_bridge_call_seconds(CALL_ENDED, 1000000, 6000000));
    TEST_ASSERT_EQUAL_UINT32(0, ui_bridge_call_seconds(CALL_ACTIVE, 6000000, 1000000));
}

static void test_hhmm(void)
{
    char t[6];
    ui_bridge_format_hhmm(true, 9, 5, t);
    TEST_ASSERT_EQUAL_STRING("09:05", t);
    ui_bridge_format_hhmm(true, 23, 59, t);
    TEST_ASSERT_EQUAL_STRING("23:59", t);
    ui_bridge_format_hhmm(false, 9, 5, t);
    TEST_ASSERT_EQUAL_STRING("", t);
    ui_bridge_format_hhmm(true, 24, 0, t);
    TEST_ASSERT_EQUAL_STRING("", t);
}

static void test_state_maps(void)
{
    TEST_ASSERT_EQUAL(UI_REG_NONE, ui_bridge_map_reg(SIP_REG_NONE));
    TEST_ASSERT_EQUAL(UI_REG_REGISTERING, ui_bridge_map_reg(SIP_REG_REGISTERING));
    TEST_ASSERT_EQUAL(UI_REG_REGISTERED, ui_bridge_map_reg(SIP_REG_REGISTERED));
    TEST_ASSERT_EQUAL(UI_REG_FAILED, ui_bridge_map_reg(SIP_REG_FAILED));
    TEST_ASSERT_EQUAL(UI_CALL_IDLE, ui_bridge_map_call(CALL_IDLE));
    TEST_ASSERT_EQUAL(UI_CALL_DIALING, ui_bridge_map_call(CALL_DIALING));
    TEST_ASSERT_EQUAL(UI_CALL_RINGING_OUT, ui_bridge_map_call(CALL_RINGING_OUT));
    TEST_ASSERT_EQUAL(UI_CALL_RINGING_IN, ui_bridge_map_call(CALL_RINGING_IN));
    TEST_ASSERT_EQUAL(UI_CALL_ACTIVE, ui_bridge_map_call(CALL_ACTIVE));
    TEST_ASSERT_EQUAL(UI_CALL_HELD, ui_bridge_map_call(CALL_HELD));
    TEST_ASSERT_EQUAL(UI_CALL_ENDED, ui_bridge_map_call(CALL_ENDED));
}

static void test_build_model(void)
{
    ui_bridge_local_t loc = { .net = UI_NET_CONNECTED, .rssi = -61, .time_valid = true, .hour = 7, .minute = 3 };
    strcpy(loc.ip, "192.168.1.50");
    strcpy(loc.account, "1001@pbx");
    phone_status_t ph;
    memset(&ph, 0, sizeof(ph));
    ph.reg = SIP_REG_REGISTERED;
    ph.call = CALL_ACTIVE;
    strcpy(ph.number, "600");
    strcpy(ph.name, "Echo");
    ph.call_start_us = 10 * 1000000LL;
    ph.end_code = 486;                                  /* stale: not shown outside ENDED */
    ph.muted = true;
    ph.volume = 55;

    ui_model_t m;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_EQUAL(UI_NET_CONNECTED, m.net);
    TEST_ASSERT_EQUAL(-61, m.rssi);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", m.ip);
    TEST_ASSERT_EQUAL(UI_REG_REGISTERED, m.reg);
    TEST_ASSERT_EQUAL_STRING("1001@pbx", m.account);
    TEST_ASSERT_EQUAL(UI_CALL_ACTIVE, m.call);
    TEST_ASSERT_EQUAL_STRING("600", m.remote_number);
    TEST_ASSERT_EQUAL_STRING("Echo", m.remote_name);
    TEST_ASSERT_EQUAL_UINT32(62, m.call_seconds);
    TEST_ASSERT_EQUAL(0, m.end_code);
    TEST_ASSERT_TRUE(m.muted);
    TEST_ASSERT_FALSE(m.speaker);
    TEST_ASSERT_EQUAL(55, m.volume);
    TEST_ASSERT_EQUAL(-1, m.battery_pct);
    TEST_ASSERT_EQUAL_STRING("07:03", m.time_hhmm);

    ph.call = CALL_ENDED;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_EQUAL(UI_CALL_ENDED, m.call);
    TEST_ASSERT_EQUAL(486, m.end_code);
    TEST_ASSERT_EQUAL_UINT32(0, m.call_seconds);

    ph.call = CALL_IDLE;
    loc.net = UI_NET_OFF;
    loc.time_valid = false;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_EQUAL_STRING("", m.remote_number);      /* no leftovers on the home screen */
    TEST_ASSERT_EQUAL_STRING("", m.ip);
    TEST_ASSERT_EQUAL(0, m.rssi);
    TEST_ASSERT_EQUAL_STRING("", m.time_hhmm);
}

static void test_build_model_is_deterministic(void)
{
    /* the bridge skips ui_update when the model bytes are unchanged */
    ui_bridge_local_t loc;
    memset(&loc, 0, sizeof(loc));
    phone_status_t ph;
    memset(&ph, 0, sizeof(ph));
    ui_model_t a, b;
    memset(&a, 0xAA, sizeof(a));
    memset(&b, 0x55, sizeof(b));
    ui_bridge_build_model(&loc, &ph, 0, &a);
    ui_bridge_build_model(&loc, &ph, 0, &b);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tone_per_state);
    RUN_TEST(test_early_media_replaces_ringback);
    RUN_TEST(test_busy_tone_only_for_refused_outgoing);
    RUN_TEST(test_end_codes);
    RUN_TEST(test_ended_hold_time);
    RUN_TEST(test_media_flags);
    RUN_TEST(test_rtp_source_filter);
    RUN_TEST(test_port_alloc_range_and_wrap);
    RUN_TEST(test_port_alloc_seed_and_skip);
    RUN_TEST(test_dtmf_valid);
    RUN_TEST(test_dtmf_sequence_spacing);
    RUN_TEST(test_dtmf_event_fits_in_gap);
    RUN_TEST(test_dtmf_push_rejects);
    RUN_TEST(test_volume_kind);
    RUN_TEST(test_account_format);
    RUN_TEST(test_call_seconds);
    RUN_TEST(test_hhmm);
    RUN_TEST(test_state_maps);
    RUN_TEST(test_build_model);
    RUN_TEST(test_build_model_is_deterministic);
    return UNITY_END();
}

/* Host tests: phone call-control policy + UI-bridge model building. */

#define _GNU_SOURCE /* memmem */
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
    /* we declined (603 is a normal local end) or forwarded the call (302 shown as such) */
    TEST_ASSERT_EQUAL(0, phone_end_code(SIP_END_LOCAL, 603));
    TEST_ASSERT_EQUAL(302, phone_end_code(SIP_END_LOCAL, 302));
    TEST_ASSERT_FALSE(phone_end_is_busy(false, 302));
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

static void test_dtmf_sequence_next_and_defer(void)
{
    phone_dtmf_seq_t s;
    phone_dtmf_seq_clear(&s);
    TEST_ASSERT_TRUE(UINT64_MAX == phone_dtmf_seq_next_ms(&s));      /* empty: no deadline */
    phone_dtmf_seq_defer(&s, 500);                                  /* empty: no-op */
    TEST_ASSERT_TRUE(UINT64_MAX == phone_dtmf_seq_next_ms(&s));
    TEST_ASSERT_TRUE(phone_dtmf_seq_push(&s, "12"));
    TEST_ASSERT_TRUE(phone_dtmf_seq_next_ms(&s) <= 1000);            /* due at once */
    phone_dtmf_seq_defer(&s, 1020);                                 /* media busy */
    TEST_ASSERT_EQUAL_CHAR(0, phone_dtmf_seq_due(&s, 1019));
    TEST_ASSERT_EQUAL_CHAR('1', phone_dtmf_seq_due(&s, 1020));
    phone_dtmf_seq_consume(&s, 1020);
    TEST_ASSERT_TRUE(1020 + PHONE_DTMF_GAP_MS == phone_dtmf_seq_next_ms(&s));
    phone_dtmf_seq_consume(&s, 1170);
    TEST_ASSERT_TRUE(UINT64_MAX == phone_dtmf_seq_next_ms(&s));
}

static void test_wait_ms(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, phone_wait_ms(1000, 0, 2000));           /* overdue */
    TEST_ASSERT_EQUAL_UINT32(0, phone_wait_ms(1000, 1000, 2000));        /* due now */
    TEST_ASSERT_EQUAL_UINT32(1, phone_wait_ms(1000, 1001, 2000));
    TEST_ASSERT_EQUAL_UINT32(1500, phone_wait_ms(1000, 2500, 2000));
    TEST_ASSERT_EQUAL_UINT32(2000, phone_wait_ms(1000, 3000, 2000));     /* clamped */
    TEST_ASSERT_EQUAL_UINT32(2000, phone_wait_ms(1000, UINT64_MAX, 2000)); /* none */
    TEST_ASSERT_TRUE(5 == phone_deadline_min(5, UINT64_MAX));
    TEST_ASSERT_TRUE(5 == phone_deadline_min(7, 5));
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

static void test_time_24h(void)
{
    char t[UI_TIME_STR_LEN];
    ui_bridge_format_time(true, 9, 5, true, t);
    TEST_ASSERT_EQUAL_STRING("09:05", t);
    ui_bridge_format_time(true, 23, 59, true, t);
    TEST_ASSERT_EQUAL_STRING("23:59", t);
    ui_bridge_format_time(true, 0, 5, true, t);
    TEST_ASSERT_EQUAL_STRING("00:05", t);
    ui_bridge_format_time(true, 12, 0, true, t);
    TEST_ASSERT_EQUAL_STRING("12:00", t);
    ui_bridge_format_time(false, 9, 5, true, t);
    TEST_ASSERT_EQUAL_STRING("", t);
    ui_bridge_format_time(true, 24, 0, true, t);
    TEST_ASSERT_EQUAL_STRING("", t);
    ui_bridge_format_time(true, 3, 60, true, t);
    TEST_ASSERT_EQUAL_STRING("", t);
    ui_bridge_format_time(true, -1, 0, false, t);
    TEST_ASSERT_EQUAL_STRING("", t);
}

static void test_time_12h(void)
{
    char t[UI_TIME_STR_LEN];
    ui_bridge_format_time(true, 0, 5, false, t);        /* midnight hour is 12 AM */
    TEST_ASSERT_EQUAL_STRING("12:05 AM", t);
    ui_bridge_format_time(true, 0, 0, false, t);
    TEST_ASSERT_EQUAL_STRING("12:00 AM", t);
    ui_bridge_format_time(true, 12, 0, false, t);       /* noon is 12 PM */
    TEST_ASSERT_EQUAL_STRING("12:00 PM", t);
    ui_bridge_format_time(true, 12, 59, false, t);
    TEST_ASSERT_EQUAL_STRING("12:59 PM", t);
    ui_bridge_format_time(true, 1, 0, false, t);
    TEST_ASSERT_EQUAL_STRING("1:00 AM", t);
    ui_bridge_format_time(true, 9, 5, false, t);
    TEST_ASSERT_EQUAL_STRING("9:05 AM", t);
    ui_bridge_format_time(true, 11, 59, false, t);
    TEST_ASSERT_EQUAL_STRING("11:59 AM", t);
    ui_bridge_format_time(true, 13, 0, false, t);
    TEST_ASSERT_EQUAL_STRING("1:00 PM", t);
    ui_bridge_format_time(true, 15, 7, false, t);
    TEST_ASSERT_EQUAL_STRING("3:07 PM", t);
    ui_bridge_format_time(true, 23, 59, false, t);
    TEST_ASSERT_EQUAL_STRING("11:59 PM", t);
    ui_bridge_format_time(false, 15, 7, false, t);
    TEST_ASSERT_EQUAL_STRING("", t);
    ui_bridge_format_time(true, 24, 0, false, t);
    TEST_ASSERT_EQUAL_STRING("", t);
    /* the longest string fits the model field with its NUL */
    ui_bridge_format_time(true, 10, 59, false, t);
    TEST_ASSERT_EQUAL_STRING("10:59 AM", t);
    TEST_ASSERT_TRUE(strlen(t) < UI_TIME_STR_LEN);
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
    strcpy(loc.wifi_ssid, "homenet");
    loc.wifi_pass_set = true;
    strcpy(loc.sip_server, "pbx.lan");
    loc.sip_port = 5061;
    strcpy(loc.sip_user, "1001");
    strcpy(loc.sip_display_name, "Alice");
    loc.sip_pass_set = false;
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
    TEST_ASSERT_EQUAL_STRING("7:03 AM", m.time_hhmm);   /* 12 h is the default */
    loc.clock_24h = true;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_EQUAL_STRING("07:03", m.time_hhmm);
    loc.clock_24h = false;
    TEST_ASSERT_EQUAL_STRING("homenet", m.wifi_ssid);
    TEST_ASSERT_TRUE(m.wifi_pass_set);
    TEST_ASSERT_EQUAL_STRING("pbx.lan", m.sip_server);
    TEST_ASSERT_EQUAL_UINT16(5061, m.sip_port);
    TEST_ASSERT_EQUAL_STRING("1001", m.sip_user);
    TEST_ASSERT_EQUAL_STRING("Alice", m.sip_display_name);
    TEST_ASSERT_FALSE(m.sip_pass_set);

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

/* call features: second call, transfer, dnd, page */
static void test_model_call_features(void)
{
    ui_bridge_local_t loc;
    memset(&loc, 0, sizeof(loc));
    phone_status_t ph;
    memset(&ph, 0, sizeof(ph));
    ui_model_t m;

    ph.call = CALL_ACTIVE;
    ph.call_start_us = 10 * 1000000LL;
    strcpy(ph.number, "600");
    ph.auto_answered = true;
    ph.second.call = CALL_RINGING_IN;
    strcpy(ph.second.number, "1002");
    strcpy(ph.second.name, "Carol");
    ph.second.call_start_us = 0;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_TRUE(m.auto_answered);
    TEST_ASSERT_FALSE(m.find_ringing);
    TEST_ASSERT_EQUAL(UI_CALL_RINGING_IN, m.second_call);
    TEST_ASSERT_EQUAL_STRING("1002", m.second_number);
    TEST_ASSERT_EQUAL_STRING("Carol", m.second_name);
    TEST_ASSERT_EQUAL_UINT32(0, m.second_seconds);
    TEST_ASSERT_FALSE(m.dnd);
    TEST_ASSERT_EQUAL(UI_XFER_IDLE, m.xfer);
    TEST_ASSERT_EQUAL_UINT16(0, m.missed_seq);

    /* CALL_EVENT_MISSED as kept by the bridge: passed through for the UI's notice */
    loc.missed_seq = 3;
    loc.missed_dnd = true;
    strcpy(loc.missed_from, "Bob");
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_EQUAL_UINT16(3, m.missed_seq);
    TEST_ASSERT_TRUE(m.missed_dnd);
    TEST_ASSERT_EQUAL_STRING("Bob", m.missed_from);

    /* held background call: elapsed time; a background ACTIVE (hold unconfirmed) counts as held */
    ph.second.call = CALL_ACTIVE;
    ph.second.call_start_us = 40 * 1000000LL;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_EQUAL(UI_CALL_HELD, m.second_call);
    TEST_ASSERT_EQUAL_UINT32(32, m.second_seconds);
    ph.second.call = CALL_HELD;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_EQUAL(UI_CALL_HELD, m.second_call);

    /* no second call: no leftovers */
    ph.second.call = CALL_IDLE;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_EQUAL(UI_CALL_IDLE, m.second_call);
    TEST_ASSERT_EQUAL_STRING("", m.second_number);
    TEST_ASSERT_EQUAL_UINT32(0, m.second_seconds);

    /* auto answer flag only with a call */
    ph.call = CALL_IDLE;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_FALSE(m.auto_answered);

    /* find the handset: only while that call rings */
    ph.find = true;
    ph.call = CALL_RINGING_IN;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_TRUE(m.find_ringing);
    ph.call = CALL_ACTIVE;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_FALSE(m.find_ringing);
    ph.find = false;
    ph.call = CALL_IDLE;

    /* quiet hours: settings passed through, active only inside the window with the clock set */
    ui_bridge_fill_quiet(&loc, true, 22 * 60, 7 * 60, true);
    loc.time_valid = true;
    loc.hour = 23;
    loc.minute = 15;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_TRUE(m.quiet_hours);
    TEST_ASSERT_EQUAL_UINT16(22 * 60, m.quiet_start);
    TEST_ASSERT_EQUAL_UINT16(7 * 60, m.quiet_end);
    TEST_ASSERT_TRUE(m.quiet_allow_contacts);
    TEST_ASSERT_TRUE(m.quiet_active);
    loc.hour = 12;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_FALSE(m.quiet_active);
    loc.hour = 6;
    loc.time_valid = false;     /* no SNTP yet */
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_FALSE(m.quiet_active);
    ui_bridge_fill_quiet(&loc, false, 22 * 60, 7 * 60, true);
    loc.time_valid = true;
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_FALSE(m.quiet_active);
    loc.time_valid = false;

    /* call forwarding settings pass through (NULL = "") */
    ui_bridge_fill_forward(&loc, "1003", NULL, "sip:vm@pbx", 25);
    ui_bridge_build_model(&loc, &ph, 72 * 1000000LL, &m);
    TEST_ASSERT_EQUAL_STRING("1003", m.fwd_always);
    TEST_ASSERT_EQUAL_STRING("", m.fwd_busy);
    TEST_ASSERT_EQUAL_STRING("sip:vm@pbx", m.fwd_noanswer);
    TEST_ASSERT_EQUAL_UINT16(25, m.fwd_noanswer_s);
    ui_bridge_fill_forward(&loc, "", "", "", 20);

    /* transfer states, attended flag, code and target */
    ph.xfer.state = XFER_FAILED;
    ph.xfer.attended = false;
    ph.xfer.code = 486;
    strcpy(ph.xfer.target, "1003");
    ui_bridge_build_model(&loc, &ph, 0, &m);
    TEST_ASSERT_EQUAL(UI_XFER_FAILED, m.xfer);
    TEST_ASSERT_EQUAL(486, m.xfer_code);
    TEST_ASSERT_EQUAL_STRING("1003", m.xfer_target);
    TEST_ASSERT_FALSE(m.xfer_attended);
    ph.xfer.state = XFER_CONSULT;
    ph.xfer.attended = true;
    ui_bridge_build_model(&loc, &ph, 0, &m);
    TEST_ASSERT_EQUAL(UI_XFER_CONSULT, m.xfer);
    TEST_ASSERT_TRUE(m.xfer_attended);
    TEST_ASSERT_EQUAL(UI_XFER_TRYING, ui_bridge_map_xfer(XFER_TRYING));
    TEST_ASSERT_EQUAL(UI_XFER_ACCEPTED, ui_bridge_map_xfer(XFER_ACCEPTED));
    TEST_ASSERT_EQUAL(UI_XFER_PROGRESS, ui_bridge_map_xfer(XFER_PROGRESS));
    TEST_ASSERT_EQUAL(UI_XFER_DONE, ui_bridge_map_xfer(XFER_DONE));
    ph.xfer.state = XFER_IDLE;
    ui_bridge_build_model(&loc, &ph, 0, &m);
    TEST_ASSERT_EQUAL_STRING("", m.xfer_target);
    TEST_ASSERT_EQUAL(0, m.xfer_code);

    /* dnd follows the setting copy in loc */
    ui_bridge_fill_dnd(&loc, true);
    ui_bridge_build_model(&loc, &ph, 0, &m);
    TEST_ASSERT_TRUE(m.dnd);
}

static void test_fill_settings(void)
{
    ui_bridge_local_t loc;
    memset(&loc, 0, sizeof(loc));
    ui_bridge_fill_settings(&loc, "homenet", "hunter2", "pbx.lan", 5062, "1001", "", "Alice");
    TEST_ASSERT_EQUAL_STRING("homenet", loc.wifi_ssid);
    TEST_ASSERT_TRUE(loc.wifi_pass_set);
    TEST_ASSERT_EQUAL_STRING("pbx.lan", loc.sip_server);
    TEST_ASSERT_EQUAL_UINT16(5062, loc.sip_port);
    TEST_ASSERT_EQUAL_STRING("1001", loc.sip_user);
    TEST_ASSERT_FALSE(loc.sip_pass_set);
    TEST_ASSERT_EQUAL_STRING("Alice", loc.sip_display_name);
    TEST_ASSERT_EQUAL_STRING("Alice <1001@pbx.lan>", loc.account);

    /* the secret itself must not appear anywhere in what the UI gets */
    phone_status_t ph;
    memset(&ph, 0, sizeof(ph));
    ui_model_t m;
    ui_bridge_build_model(&loc, &ph, 0, &m);
    TEST_ASSERT_NULL(memmem(&m, sizeof(m), "hunter2", 7));
    TEST_ASSERT_TRUE(m.wifi_pass_set);

    /* unset / NULL -> empty strings and cleared flags */
    ui_bridge_fill_settings(&loc, NULL, NULL, NULL, 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("", loc.wifi_ssid);
    TEST_ASSERT_FALSE(loc.wifi_pass_set);
    TEST_ASSERT_EQUAL_STRING("", loc.sip_server);
    TEST_ASSERT_EQUAL_UINT16(0, loc.sip_port);
    TEST_ASSERT_EQUAL_STRING("", loc.account);
}

static void test_fill_time_ota(void)
{
    ui_bridge_local_t loc;
    memset(&loc, 0, sizeof(loc));
    ui_bridge_fill_time_ota(&loc, "ntp.lan", "CET-1CEST,M3.5.0,M10.5.0/3", true, "http://10.0.0.5:8090/ota.bin");
    ui_bridge_fill_fw(&loc, "v1.2-3-gabc", "ota_1");
    ota_status_t os = { .state = OTA_STATE_RUNNING, .percent = 42, .bytes = 1234 };
    strcpy(os.message, "Downloading 42%");
    ui_bridge_fill_ota(&loc, &os);

    phone_status_t ph;
    memset(&ph, 0, sizeof(ph));
    ui_model_t m;
    ui_bridge_build_model(&loc, &ph, 0, &m);
    TEST_ASSERT_EQUAL_STRING("ntp.lan", m.ntp_server);
    TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", m.tz);
    TEST_ASSERT_TRUE(m.clock_24h);
    TEST_ASSERT_EQUAL_STRING("http://10.0.0.5:8090/ota.bin", m.ota_url);
    TEST_ASSERT_EQUAL_STRING("v1.2-3-gabc", m.fw_version);
    TEST_ASSERT_EQUAL_STRING("ota_1", m.fw_partition);
    TEST_ASSERT_EQUAL(UI_OTA_RUNNING, m.ota_state);
    TEST_ASSERT_EQUAL_UINT8(42, m.ota_percent);
    TEST_ASSERT_EQUAL_STRING("Downloading 42%", m.ota_message);

    /* states map one to one; out-of-range percent is clamped */
    TEST_ASSERT_EQUAL(UI_OTA_IDLE, ui_bridge_map_ota(OTA_STATE_IDLE));
    TEST_ASSERT_EQUAL(UI_OTA_DONE, ui_bridge_map_ota(OTA_STATE_DONE));
    TEST_ASSERT_EQUAL(UI_OTA_FAILED, ui_bridge_map_ota(OTA_STATE_FAILED));
    os.state = OTA_STATE_DONE;
    os.percent = 250;
    ui_bridge_fill_ota(&loc, &os);
    TEST_ASSERT_EQUAL_UINT8(100, loc.ota_percent);

    /* NULL -> empty */
    ui_bridge_fill_time_ota(&loc, NULL, NULL, false, NULL);
    ui_bridge_fill_fw(&loc, NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("", loc.ntp_server);
    TEST_ASSERT_EQUAL_STRING("", loc.tz);
    TEST_ASSERT_FALSE(loc.clock_24h);
    TEST_ASSERT_EQUAL_STRING("", loc.ota_url);
    TEST_ASSERT_EQUAL_STRING("", loc.fw_version);
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

static void test_codec_names(void)
{
    TEST_ASSERT_EQUAL_STRING("G722", phone_codec_name(9));
    TEST_ASSERT_EQUAL_STRING("PCMU", phone_codec_name(0));
    TEST_ASSERT_EQUAL_STRING("PCMA", phone_codec_name(8));
    TEST_ASSERT_EQUAL_STRING("?", phone_codec_name(18));
}

static void test_codec_pref_parse(void)
{
    uint8_t pts[SIP_UA_MAX_CODECS];
    TEST_ASSERT_EQUAL_INT(3, phone_codec_pref_parse(PHONE_CODEC_PREF_DEFAULT, pts, SIP_UA_MAX_CODECS));
    TEST_ASSERT_EQUAL_UINT8(9, pts[0]);
    TEST_ASSERT_EQUAL_UINT8(0, pts[1]);
    TEST_ASSERT_EQUAL_UINT8(8, pts[2]);
    TEST_ASSERT_EQUAL_INT(2, phone_codec_pref_parse(" ALaw, G722 ", pts, SIP_UA_MAX_CODECS));
    TEST_ASSERT_EQUAL_UINT8(8, pts[0]);
    TEST_ASSERT_EQUAL_UINT8(9, pts[1]);
    TEST_ASSERT_EQUAL_INT(1, phone_codec_pref_parse("ulaw", pts, SIP_UA_MAX_CODECS));
    TEST_ASSERT_EQUAL_UINT8(0, pts[0]);
    TEST_ASSERT_EQUAL_INT(2, phone_codec_pref_parse("pcmu pcma", pts, SIP_UA_MAX_CODECS));
    /* rejected */
    TEST_ASSERT_EQUAL_INT(0, phone_codec_pref_parse("", pts, SIP_UA_MAX_CODECS));
    TEST_ASSERT_EQUAL_INT(0, phone_codec_pref_parse(" , ", pts, SIP_UA_MAX_CODECS));
    TEST_ASSERT_EQUAL_INT(0, phone_codec_pref_parse(NULL, pts, SIP_UA_MAX_CODECS));
    TEST_ASSERT_EQUAL_INT(0, phone_codec_pref_parse("g722,opus", pts, SIP_UA_MAX_CODECS));
    TEST_ASSERT_EQUAL_INT(0, phone_codec_pref_parse("g7221", pts, SIP_UA_MAX_CODECS));
    TEST_ASSERT_EQUAL_INT(0, phone_codec_pref_parse("pcmu,ulaw", pts, SIP_UA_MAX_CODECS));   /* duplicate */
    TEST_ASSERT_EQUAL_INT(0, phone_codec_pref_parse("g722,pcmu,pcma", pts, 2));             /* too many */
}

/* ---- BLF list / plan / model ---- */

static void test_blf_parse(void)
{
    sipwire_blf_entry_t e[SIPWIRE_BLF_MAX];
    TEST_ASSERT_EQUAL(0, sipwire_blf_parse("", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(0, sipwire_blf_parse(NULL, e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(0, sipwire_blf_parse(" , ,", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(3, sipwire_blf_parse("1002:Bob, 1003 : Front desk ,*8#1,", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL_STRING("1002", e[0].ext);
    TEST_ASSERT_EQUAL_STRING("Bob", e[0].label);
    TEST_ASSERT_EQUAL_STRING("1003", e[1].ext);
    TEST_ASSERT_EQUAL_STRING("Front desk", e[1].label);
    TEST_ASSERT_EQUAL_STRING("*8#1", e[2].ext);
    TEST_ASSERT_EQUAL_STRING("", e[2].label);
    TEST_ASSERT_EQUAL(1, sipwire_blf_parse("1004:", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(1, sipwire_blf_parse("alice.b_c-d+1", NULL, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(8, sipwire_blf_parse("1,2,3,4,5,6,7,8", e, SIPWIRE_BLF_MAX));
    /* invalid */
    TEST_ASSERT_EQUAL(-1, sipwire_blf_parse("1,2,3,4,5,6,7,8,9", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(-1, sipwire_blf_parse("1,2,3", e, 2));
    TEST_ASSERT_EQUAL(-1, sipwire_blf_parse(":Bob", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(-1, sipwire_blf_parse("10 02", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(-1, sipwire_blf_parse("1002:a:b", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(-1, sipwire_blf_parse("1002,1003,1002", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(-1, sipwire_blf_parse("1002:tab\there", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(-1, sipwire_blf_parse("1002:123456789012345678901234", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(1, sipwire_blf_parse("1002:12345678901234567890123", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(-1, sipwire_blf_parse("12345678901234567890123456789012", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL(1, sipwire_blf_parse("1234567890123456789012345678901", e, SIPWIRE_BLF_MAX));
}

static void test_blf_format_roundtrip(void)
{
    sipwire_blf_entry_t e[SIPWIRE_BLF_MAX], f[SIPWIRE_BLF_MAX];
    char out[SIPWIRE_BLF_LIST_MAX];
    TEST_ASSERT_EQUAL(3, sipwire_blf_parse(" 1002 : Bob ,1003,1004:Front desk", e, SIPWIRE_BLF_MAX));
    TEST_ASSERT_TRUE(sipwire_blf_format(e, 3, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("1002:Bob,1003,1004:Front desk", out);
    TEST_ASSERT_EQUAL(3, sipwire_blf_parse(out, f, SIPWIRE_BLF_MAX));
    TEST_ASSERT_EQUAL_MEMORY(e, f, 3 * sizeof(e[0]));
    TEST_ASSERT_FALSE(sipwire_blf_format(e, 3, out, 10));
    TEST_ASSERT_EQUAL_STRING("", out);
    /* 8 entries with the longest extension and label still fit the setting */
    for (int i = 0; i < SIPWIRE_BLF_MAX; i++) {
        memset(e[i].ext, '0' + i, SIPWIRE_BLF_EXT_MAX - 1);
        e[i].ext[SIPWIRE_BLF_EXT_MAX - 1] = '\0';
        memset(e[i].label, 'a' + i, SIPWIRE_BLF_LABEL_MAX - 1);
        e[i].label[SIPWIRE_BLF_LABEL_MAX - 1] = '\0';
    }
    TEST_ASSERT_TRUE(sipwire_blf_format(e, SIPWIRE_BLF_MAX, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SIPWIRE_BLF_MAX, sipwire_blf_parse(out, f, SIPWIRE_BLF_MAX));
}

static void test_blf_plan(void)
{
    sipwire_blf_entry_t old[SIPWIRE_BLF_MAX], nw[SIPWIRE_BLF_MAX];
    int keep[SIPWIRE_BLF_MAX];
    int no = sipwire_blf_parse("1002:Bob,1003,1004", old, SIPWIRE_BLF_MAX);
    int nn = sipwire_blf_parse("1004,1005,1002:Robert", nw, SIPWIRE_BLF_MAX);
    uint32_t drop = phone_blf_plan(old, no, nw, nn, keep);
    TEST_ASSERT_EQUAL_HEX32(0x2, drop);                 /* 1003 removed */
    TEST_ASSERT_EQUAL(2, keep[0]);                      /* 1004 kept */
    TEST_ASSERT_EQUAL(-1, keep[1]);                     /* 1005 new */
    TEST_ASSERT_EQUAL(0, keep[2]);                      /* 1002 kept, label changed */
    TEST_ASSERT_EQUAL_HEX32(0x7, phone_blf_plan(old, no, nw, 0, keep));
    TEST_ASSERT_EQUAL_HEX32(0x0, phone_blf_plan(old, 0, nw, nn, keep));
    TEST_ASSERT_EQUAL(-1, keep[0]);
    TEST_ASSERT_EQUAL(BLF_RINGING, phone_blf_map(SIP_BLF_RINGING));
    TEST_ASSERT_EQUAL(BLF_UNKNOWN, phone_blf_map((sip_blf_state_t)42));
    TEST_ASSERT_EQUAL_STRING("busy", phone_blf_state_name(BLF_BUSY));
}

static void test_build_model_blf_mwi(void)
{
    ui_bridge_local_t loc;
    memset(&loc, 0, sizeof(loc));
    ui_bridge_fill_vm(&loc, "*97");
    phone_status_t ph;
    memset(&ph, 0, sizeof(ph));
    ph.blf_count = 3;
    strcpy(ph.blf[0].ext, "1002");
    strcpy(ph.blf[0].label, "Bob");
    ph.blf[0].state = BLF_RINGING;
    strcpy(ph.blf[1].ext, "1003");
    ph.blf[1].state = BLF_BUSY;
    strcpy(ph.blf[2].ext, "1004");
    ph.blf[2].state = BLF_UNKNOWN;
    ph.mwi = (sip_event_mwi_t){ .known = true, .waiting = true, .new_msgs = 3, .old_msgs = 7 };
    ui_model_t m;
    ui_bridge_build_model(&loc, &ph, 0, &m);
    TEST_ASSERT_EQUAL(3, m.blf_count);
    TEST_ASSERT_EQUAL_STRING("1002", m.blf[0].ext);
    TEST_ASSERT_EQUAL_STRING("Bob", m.blf[0].label);
    TEST_ASSERT_EQUAL(UI_BLF_RINGING, m.blf[0].state);
    TEST_ASSERT_EQUAL(UI_BLF_BUSY, m.blf[1].state);
    TEST_ASSERT_EQUAL_STRING("", m.blf[1].label);
    TEST_ASSERT_EQUAL(UI_BLF_UNKNOWN, m.blf[2].state);
    TEST_ASSERT_TRUE(m.mwi_waiting);
    TEST_ASSERT_EQUAL_UINT16(3, m.mwi_new);
    TEST_ASSERT_EQUAL_UINT16(7, m.mwi_old);
    TEST_ASSERT_EQUAL_STRING("*97", m.vm_number);
    /* unknown MWI shows nothing; too many entries are capped */
    ph.mwi.known = false;
    ph.blf_count = 200;
    ui_bridge_build_model(&loc, &ph, 0, &m);
    TEST_ASSERT_FALSE(m.mwi_waiting);
    TEST_ASSERT_EQUAL_UINT16(0, m.mwi_new);
    TEST_ASSERT_EQUAL(UI_BLF_MAX, m.blf_count);
    TEST_ASSERT_EQUAL(UI_BLF_IDLE, ui_bridge_map_blf(BLF_IDLE));
    ui_bridge_fill_vm(&loc, NULL);
    TEST_ASSERT_EQUAL_STRING("", loc.vm_number);
}

static void test_fill_sounds(void)
{
    ui_bridge_local_t loc;
    memset(&loc, 0, sizeof(loc));
    ui_bridge_fill_sounds(&loc, 6, 2, 70, 150, 90, true, false, true);
    TEST_ASSERT_EQUAL_UINT8(6, loc.ringtone_count);
    TEST_ASSERT_EQUAL_UINT8(100, loc.call_volume);   /* clamped */
    phone_status_t ph;
    memset(&ph, 0, sizeof(ph));
    ui_model_t m;
    ui_bridge_build_model(&loc, &ph, 0, &m);
    TEST_ASSERT_EQUAL_UINT8(6, m.ringtone_count);
    TEST_ASSERT_EQUAL_UINT8(2, m.ring_tone);
    TEST_ASSERT_EQUAL_UINT8(70, m.ring_volume);
    TEST_ASSERT_EQUAL_UINT8(100, m.call_volume);
    TEST_ASSERT_EQUAL_UINT8(90, m.speaker_volume);
    TEST_ASSERT_TRUE(m.call_waiting);
    TEST_ASSERT_FALSE(m.auto_answer);
    TEST_ASSERT_TRUE(m.dnd);
    TEST_ASSERT_EQUAL_UINT8(0, m.missed_unseen);
    TEST_ASSERT_EQUAL_INT64(0, m.now);
    ui_bridge_fill_sounds(&loc, -3, 0, 0, 0, 0, false, false, false);
    TEST_ASSERT_EQUAL_UINT8(0, loc.ringtone_count);
}

static void test_tone_call_waiting(void)
{
    /* beep only while the foreground call is established and the other rings */
    TEST_ASSERT_EQUAL(PHONE_TONE_CALL_WAITING,
                      phone_tone_waiting(PHONE_TONE_NONE, SIP_UA_CALL_ACTIVE, SIP_UA_CALL_RINGING_IN));
    TEST_ASSERT_EQUAL(PHONE_TONE_CALL_WAITING,
                      phone_tone_waiting(PHONE_TONE_NONE, SIP_UA_CALL_HELD, SIP_UA_CALL_RINGING_IN));
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE, phone_tone_waiting(PHONE_TONE_NONE, SIP_UA_CALL_ACTIVE, SIP_UA_CALL_IDLE));
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE, phone_tone_waiting(PHONE_TONE_NONE, SIP_UA_CALL_ACTIVE, SIP_UA_CALL_HELD));
    TEST_ASSERT_EQUAL(PHONE_TONE_RINGBACK,
                      phone_tone_waiting(PHONE_TONE_RINGBACK, SIP_UA_CALL_RINGING_OUT, SIP_UA_CALL_RINGING_IN));
    TEST_ASSERT_EQUAL(PHONE_TONE_RING,
                      phone_tone_waiting(PHONE_TONE_RING, SIP_UA_CALL_RINGING_IN, SIP_UA_CALL_IDLE));
    TEST_ASSERT_EQUAL(PHONE_TONE_NONE,
                      phone_tone_waiting(PHONE_TONE_NONE, SIP_UA_CALL_ENDED, SIP_UA_CALL_RINGING_IN));
}

static void test_auto_answer_policy(void)
{
    TEST_ASSERT_TRUE(phone_auto_answer(0, true, false));
    TEST_ASSERT_TRUE(phone_auto_answer(3, true, false));
    TEST_ASSERT_FALSE(phone_auto_answer(-1, true, false));   /* not a page */
    TEST_ASSERT_FALSE(phone_auto_answer(0, false, false));   /* setting off */
    TEST_ASSERT_FALSE(phone_auto_answer(0, true, true));     /* in a call: waits like any call */
}

static void test_xfer_map(void)
{
    TEST_ASSERT_EQUAL(XFER_TRYING, phone_xfer_map(SIP_XFER_TRYING));
    TEST_ASSERT_EQUAL(XFER_ACCEPTED, phone_xfer_map(SIP_XFER_ACCEPTED));
    TEST_ASSERT_EQUAL(XFER_PROGRESS, phone_xfer_map(SIP_XFER_PROGRESS));
    TEST_ASSERT_EQUAL(XFER_DONE, phone_xfer_map(SIP_XFER_SUCCESS));
    TEST_ASSERT_EQUAL(XFER_FAILED, phone_xfer_map(SIP_XFER_FAILED));
}

static void test_call_log_classify(void)
{
    TEST_ASSERT_EQUAL(PHONE_LOG_ANSWERED, phone_log_result(false, true, false, false, false));
    TEST_ASSERT_EQUAL(PHONE_LOG_ANSWERED, phone_log_result(true, true, false, false, false));
    TEST_ASSERT_EQUAL(PHONE_LOG_MISSED, phone_log_result(false, false, false, false, false));
    TEST_ASSERT_EQUAL(PHONE_LOG_REJECTED, phone_log_result(false, false, true, false, false));
    TEST_ASSERT_EQUAL(PHONE_LOG_DND, phone_log_result(false, false, false, true, false));
    TEST_ASSERT_EQUAL(PHONE_LOG_MISSED, phone_log_result(true, false, false, false, false));
    TEST_ASSERT_EQUAL(PHONE_LOG_MISSED, phone_log_result(true, false, true, false, false));
    /* forwarded (302): also when the user declined it with busy forwarding set */
    TEST_ASSERT_EQUAL(PHONE_LOG_FORWARDED, phone_log_result(false, false, false, false, true));
    TEST_ASSERT_EQUAL(PHONE_LOG_FORWARDED, phone_log_result(false, false, true, false, true));
    TEST_ASSERT_EQUAL(PHONE_LOG_ANSWERED, phone_log_result(false, true, false, false, true));
}

static phone_screen_t screen(bool find, bool dnd, bool quiet, bool allowed, bool busy, bool fa, bool fb)
{
    const phone_screen_in_t in = {
        .find = find, .dnd = dnd, .quiet = quiet, .caller_allowed = allowed,
        .busy = busy, .fwd_always = fa, .fwd_busy = fb,
    };
    return phone_screen_decide(&in);
}

static void test_screen_policy(void)
{
    /*                                       find   dnd    quiet  allow  busy   fwdA   fwdB */
    TEST_ASSERT_EQUAL(PHONE_SCREEN_NORMAL, screen(false, false, false, false, false, false, false));
    /* find the handset: through DND, quiet hours and always-forward; not when busy */
    TEST_ASSERT_EQUAL(PHONE_SCREEN_FIND, screen(true, true, true, false, false, true, true));
    TEST_ASSERT_EQUAL(PHONE_SCREEN_NORMAL, screen(true, false, false, false, true, false, false));
    TEST_ASSERT_EQUAL(PHONE_SCREEN_FWD_BUSY, screen(true, false, false, false, true, false, true));
    /* always-forward beats DND and quiet hours */
    TEST_ASSERT_EQUAL(PHONE_SCREEN_FWD_ALWAYS, screen(false, true, true, false, true, true, true));
    /* DND: the UA refuses (normal), quiet hours never apply on top */
    TEST_ASSERT_EQUAL(PHONE_SCREEN_NORMAL, screen(false, true, true, false, false, false, true));
    /* quiet hours: refused unless the caller is an allowed contact */
    TEST_ASSERT_EQUAL(PHONE_SCREEN_QUIET, screen(false, false, true, false, false, false, false));
    TEST_ASSERT_EQUAL(PHONE_SCREEN_QUIET, screen(false, false, true, false, true, false, true));
    TEST_ASSERT_EQUAL(PHONE_SCREEN_NORMAL, screen(false, false, true, true, false, false, false));
    /* busy-forward only when busy */
    TEST_ASSERT_EQUAL(PHONE_SCREEN_FWD_BUSY, screen(false, false, false, false, true, false, true));
    TEST_ASSERT_EQUAL(PHONE_SCREEN_NORMAL, screen(false, false, false, false, false, false, true));
    TEST_ASSERT_EQUAL(PHONE_SCREEN_NORMAL, screen(false, false, false, false, true, false, false));
    TEST_ASSERT_EQUAL(PHONE_SCREEN_FWD_BUSY, screen(false, false, true, true, true, false, true));
}

#define HM(h, m) ((h) * 60 + (m))

static void test_quiet_window(void)
{
    /* same day: 13:00-14:30 */
    TEST_ASSERT_FALSE(phone_quiet_window(HM(13, 0), HM(14, 30), HM(12, 59)));
    TEST_ASSERT_TRUE(phone_quiet_window(HM(13, 0), HM(14, 30), HM(13, 0)));
    TEST_ASSERT_TRUE(phone_quiet_window(HM(13, 0), HM(14, 30), HM(14, 29)));
    TEST_ASSERT_FALSE(phone_quiet_window(HM(13, 0), HM(14, 30), HM(14, 30)));
    /* wraps past midnight: 22:00-07:00 */
    TEST_ASSERT_FALSE(phone_quiet_window(HM(22, 0), HM(7, 0), HM(21, 59)));
    TEST_ASSERT_TRUE(phone_quiet_window(HM(22, 0), HM(7, 0), HM(22, 0)));
    TEST_ASSERT_TRUE(phone_quiet_window(HM(22, 0), HM(7, 0), HM(23, 59)));
    TEST_ASSERT_TRUE(phone_quiet_window(HM(22, 0), HM(7, 0), 0));
    TEST_ASSERT_TRUE(phone_quiet_window(HM(22, 0), HM(7, 0), HM(6, 59)));
    TEST_ASSERT_FALSE(phone_quiet_window(HM(22, 0), HM(7, 0), HM(7, 0)));
    TEST_ASSERT_FALSE(phone_quiet_window(HM(22, 0), HM(7, 0), HM(12, 0)));
    /* from midnight / until midnight */
    TEST_ASSERT_TRUE(phone_quiet_window(0, HM(6, 0), 0));
    TEST_ASSERT_FALSE(phone_quiet_window(0, HM(6, 0), HM(23, 59)));
    TEST_ASSERT_TRUE(phone_quiet_window(HM(20, 0), 0, HM(23, 59)));
    TEST_ASSERT_FALSE(phone_quiet_window(HM(20, 0), 0, 0));
    /* start == end: empty; clock not set or out of range: never */
    TEST_ASSERT_FALSE(phone_quiet_window(HM(8, 0), HM(8, 0), HM(8, 0)));
    TEST_ASSERT_FALSE(phone_quiet_window(HM(22, 0), HM(7, 0), -1));
    TEST_ASSERT_FALSE(phone_quiet_window(HM(22, 0), HM(7, 0), 1440));
    TEST_ASSERT_FALSE(phone_quiet_window(1440, HM(7, 0), HM(3, 0)));
}

static void test_call_log_times(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, phone_log_duration_s(0, 5000000));
    TEST_ASSERT_EQUAL_UINT32(0, phone_log_duration_s(5000000, 5000000));
    TEST_ASSERT_EQUAL_UINT32(12, phone_log_duration_s(1000000, 13900000));
    TEST_ASSERT_EQUAL_INT64(0, phone_log_start(0, 10));
    TEST_ASSERT_EQUAL_INT64(1700000000 - 30, phone_log_start(1700000000, 30));
    TEST_ASSERT_EQUAL_INT64(1700000000, phone_log_start(1700000000, 0));
    TEST_ASSERT_EQUAL_INT64(5, phone_log_start(5, 50));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tone_call_waiting);
    RUN_TEST(test_auto_answer_policy);
    RUN_TEST(test_xfer_map);
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
    RUN_TEST(test_fill_sounds);
    RUN_TEST(test_call_log_classify);
    RUN_TEST(test_screen_policy);
    RUN_TEST(test_quiet_window);
    RUN_TEST(test_call_log_times);
    RUN_TEST(test_codec_names);
    RUN_TEST(test_codec_pref_parse);
    RUN_TEST(test_account_format);
    RUN_TEST(test_call_seconds);
    RUN_TEST(test_time_24h);
    RUN_TEST(test_time_12h);
    RUN_TEST(test_fill_time_ota);
    RUN_TEST(test_state_maps);
    RUN_TEST(test_build_model);
    RUN_TEST(test_fill_settings);
    RUN_TEST(test_build_model_is_deterministic);
    RUN_TEST(test_blf_parse);
    RUN_TEST(test_blf_format_roundtrip);
    RUN_TEST(test_blf_plan);
    RUN_TEST(test_build_model_blf_mwi);
    RUN_TEST(test_model_call_features);
    RUN_TEST(test_dtmf_sequence_next_and_defer);
    RUN_TEST(test_wait_ms);
    return UNITY_END();
}

/* Pure call-control policy (see phone_logic.h). No IDF dependencies. */

#include "phone_logic.h"

#include <ctype.h>
#include <string.h>

phone_tone_t phone_tone_for(sip_ua_call_state_t state, bool outgoing, bool early_media, int end_code)
{
    switch (state) {
    case SIP_UA_CALL_RINGING_OUT:
        return early_media ? PHONE_TONE_NONE : PHONE_TONE_RINGBACK;
    case SIP_UA_CALL_RINGING_IN:
        return PHONE_TONE_RING;
    case SIP_UA_CALL_ENDED:
        return phone_end_is_busy(outgoing, end_code) ? PHONE_TONE_BUSY : PHONE_TONE_NONE;
    case SIP_UA_CALL_IDLE:
    case SIP_UA_CALL_DIALING:
    case SIP_UA_CALL_ACTIVE:
    case SIP_UA_CALL_HELD:
    default:
        return PHONE_TONE_NONE;
    }
}

int phone_end_code(sip_ua_end_reason_t reason, int status_code)
{
    switch (reason) {
    case SIP_END_NONE:
    case SIP_END_LOCAL:
    case SIP_END_REMOTE:
        return 0;
    case SIP_END_CANCELLED:
        return status_code ? status_code : 487;
    case SIP_END_TIMEOUT:
        return status_code ? status_code : 408;
    case SIP_END_REJECTED:
    case SIP_END_ERROR:
    default:
        return status_code;
    }
}

bool phone_end_is_busy(bool outgoing, int end_code)
{
    return outgoing && (end_code == 486 || end_code == 600 || end_code == 603);
}

uint32_t phone_ended_hold_ms(bool outgoing, int end_code)
{
    return phone_end_is_busy(outgoing, end_code) ? PHONE_BUSY_HOLD_MS : PHONE_ENDED_HOLD_MS;
}

void phone_media_flags(sip_ua_call_state_t state, sip_media_dir_t dir, bool early, bool *send, bool *play)
{
    bool dir_send = dir == SIP_DIR_SENDRECV || dir == SIP_DIR_SENDONLY;
    bool dir_recv = dir == SIP_DIR_SENDRECV || dir == SIP_DIR_RECVONLY;
    bool s = false, p = false;

    switch (state) {
    case SIP_UA_CALL_ACTIVE:
        s = dir_send;
        p = dir_recv;
        break;
    case SIP_UA_CALL_HELD:
        /* we hold: nothing to send (no music on hold), play whatever the
         * direction allows (normally recvonly→nothing, sendonly→nothing) */
        p = dir_recv;
        break;
    case SIP_UA_CALL_DIALING:
    case SIP_UA_CALL_RINGING_OUT:
        p = early && dir_recv;
        break;
    case SIP_UA_CALL_IDLE:
    case SIP_UA_CALL_RINGING_IN:
    case SIP_UA_CALL_ENDED:
    default:
        break;
    }
    *send = s;
    *play = p;
}

bool phone_rtp_source_ok(uint32_t sdp_ip, uint32_t server_ip, uint32_t src_ip, const uint8_t *pkt, size_t len)
{
    bool from_peer = src_ip != 0 && (src_ip == sdp_ip || src_ip == server_ip);
    return from_peer && pkt != NULL && len >= 12 && (pkt[0] >> 6) == 2;
}

void phone_port_alloc_init(phone_port_alloc_t *a, uint32_t seed)
{
    a->idx = (uint16_t)(seed % PHONE_RTP_PORT_COUNT);
}

uint16_t phone_port_alloc_next(phone_port_alloc_t *a, uint16_t skip)
{
    for (unsigned tries = 0; tries < 2; tries++) {
        uint16_t port = (uint16_t)(PHONE_RTP_PORT_MIN + 2u * a->idx);
        a->idx = (uint16_t)((a->idx + 1u) % PHONE_RTP_PORT_COUNT);
        if (port != skip) {
            return port;
        }
    }
    return (uint16_t)(PHONE_RTP_PORT_MIN + 2u * a->idx);   /* unreachable with COUNT > 1 */
}

bool phone_dtmf_valid(char c)
{
    return (c >= '0' && c <= '9') || c == '*' || c == '#' || (c >= 'A' && c <= 'D') || (c >= 'a' && c <= 'd');
}

void phone_dtmf_seq_clear(phone_dtmf_seq_t *s)
{
    memset(s, 0, sizeof(*s));
}

bool phone_dtmf_seq_push(phone_dtmf_seq_t *s, const char *digits)
{
    if (digits == NULL || digits[0] == '\0') {
        return false;
    }
    size_t n = strlen(digits);
    if (n > (size_t)(PHONE_DTMF_QUEUE - s->len)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!phone_dtmf_valid(digits[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < n; i++) {
        char c = digits[i];
        if (c >= 'a' && c <= 'd') {
            c = (char)(c - 'a' + 'A');
        }
        s->q[(s->head + s->len) % PHONE_DTMF_QUEUE] = c;
        s->len++;
    }
    return true;
}

char phone_dtmf_seq_due(const phone_dtmf_seq_t *s, uint64_t now_ms)
{
    if (s->len == 0 || now_ms < s->next_ms) {
        return 0;
    }
    return s->q[s->head];
}

void phone_dtmf_seq_consume(phone_dtmf_seq_t *s, uint64_t now_ms)
{
    if (s->len == 0) {
        return;
    }
    s->head = (uint8_t)((s->head + 1u) % PHONE_DTMF_QUEUE);
    s->len--;
    s->next_ms = now_ms + PHONE_DTMF_GAP_MS;
}

phone_vol_kind_t phone_volume_kind(sip_ua_call_state_t state, bool speaker)
{
    switch (state) {
    case SIP_UA_CALL_IDLE:
    case SIP_UA_CALL_RINGING_IN:
        return PHONE_VOL_RING;
    case SIP_UA_CALL_DIALING:
    case SIP_UA_CALL_RINGING_OUT:
    case SIP_UA_CALL_ACTIVE:
    case SIP_UA_CALL_HELD:
    case SIP_UA_CALL_ENDED:
    default:
        return speaker ? PHONE_VOL_SPEAKER : PHONE_VOL_CALL;
    }
}

/* ---- codecs ---- */

const char *phone_codec_name(uint8_t pt)
{
    switch (pt) {
    case 9: return "G722";
    case 0: return "PCMU";
    case 8: return "PCMA";
    default: return "?";
    }
}

static int codec_pt(const char *name, size_t len)
{
    static const struct {
        const char *name;
        uint8_t pt;
    } k_names[] = {
        {"g722", 9}, {"pcmu", 0}, {"ulaw", 0}, {"pcma", 8}, {"alaw", 8},
    };
    for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        if (strlen(k_names[i].name) != len) {
            continue;
        }
        size_t j = 0;
        while (j < len && tolower((unsigned char)name[j]) == k_names[i].name[j]) {
            j++;
        }
        if (j == len) {
            return k_names[i].pt;
        }
    }
    return -1;
}

int phone_codec_pref_parse(const char *s, uint8_t *pts, int max)
{
    int n = 0;
    if (s == NULL) {
        return 0;
    }
    while (*s) {
        if (*s == ',' || *s == ' ') {
            s++;
            continue;
        }
        size_t len = strcspn(s, ", ");
        int pt = codec_pt(s, len);
        if (pt < 0 || n >= max) {
            return 0;
        }
        for (int i = 0; i < n; i++) {
            if (pts[i] == pt) {
                return 0;
            }
        }
        pts[n++] = (uint8_t)pt;
        s += len;
    }
    return n;
}

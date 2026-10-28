#include "ui_bridge_model.h"

#include <string.h>

ui_reg_state_t ui_bridge_map_reg(sip_reg_state_t st)
{
    switch (st) {
    case SIP_REG_REGISTERING: return UI_REG_REGISTERING;
    case SIP_REG_REGISTERED:  return UI_REG_REGISTERED;
    case SIP_REG_FAILED:      return UI_REG_FAILED;
    case SIP_REG_NONE:
    default:                  return UI_REG_NONE;
    }
}

ui_call_state_t ui_bridge_map_call(call_state_t st)
{
    switch (st) {
    case CALL_DIALING:     return UI_CALL_DIALING;
    case CALL_RINGING_OUT: return UI_CALL_RINGING_OUT;
    case CALL_RINGING_IN:  return UI_CALL_RINGING_IN;
    case CALL_ACTIVE:      return UI_CALL_ACTIVE;
    case CALL_HELD:        return UI_CALL_HELD;
    case CALL_ENDED:       return UI_CALL_ENDED;
    case CALL_IDLE:
    default:               return UI_CALL_IDLE;
    }
}

/* Bounded append; always NUL-terminates, truncates silently. */
static void append(char *out, size_t size, size_t *len, const char *s)
{
    while (*s && *len + 1 < size) {
        out[(*len)++] = *s++;
    }
    out[*len] = '\0';
}

void ui_bridge_format_account(char *out, size_t size, const char *display_name, const char *user,
                              const char *server)
{
    size_t len = 0;
    if (size == 0) {
        return;
    }
    out[0] = '\0';
    if (user == NULL || user[0] == '\0') {
        return;
    }
    bool name = display_name && display_name[0];
    bool srv = server && server[0];
    if (name) {
        append(out, size, &len, display_name);
        append(out, size, &len, " <");
    }
    append(out, size, &len, user);
    if (srv) {
        append(out, size, &len, "@");
        append(out, size, &len, server);
    }
    if (name) {
        append(out, size, &len, ">");
    }
}

uint32_t ui_bridge_call_seconds(call_state_t st, int64_t start_us, int64_t now_us)
{
    if ((st != CALL_ACTIVE && st != CALL_HELD) || start_us <= 0 || now_us <= start_us) {
        return 0;
    }
    return (uint32_t)((now_us - start_us) / 1000000);
}

void ui_bridge_format_hhmm(bool valid, int hour, int minute, char out[6])
{
    if (!valid || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        out[0] = '\0';
        return;
    }
    out[0] = (char)('0' + hour / 10);
    out[1] = (char)('0' + hour % 10);
    out[2] = ':';
    out[3] = (char)('0' + minute / 10);
    out[4] = (char)('0' + minute % 10);
    out[5] = '\0';
}

static void copy(char *dst, size_t size, const char *src)
{
    size_t len = 0;
    dst[0] = '\0';
    append(dst, size, &len, src);
}

void ui_bridge_build_model(const ui_bridge_local_t *loc, const phone_status_t *ph, int64_t now_us,
                           ui_model_t *out)
{
    memset(out, 0, sizeof(*out));
    out->net = loc->net;
    out->rssi = loc->net == UI_NET_CONNECTED ? loc->rssi : 0;
    copy(out->ip, sizeof(out->ip), loc->net == UI_NET_CONNECTED ? loc->ip : "");

    out->reg = ui_bridge_map_reg(ph->reg);
    copy(out->account, sizeof(out->account), loc->account);

    out->call = ui_bridge_map_call(ph->call);
    if (out->call != UI_CALL_IDLE) {
        copy(out->remote_number, sizeof(out->remote_number), ph->number);
        copy(out->remote_name, sizeof(out->remote_name), ph->name);
    }
    out->call_seconds = ui_bridge_call_seconds(ph->call, ph->call_start_us, now_us);
    out->end_code = out->call == UI_CALL_ENDED ? ph->end_code : 0;
    out->muted = ph->muted;
    out->speaker = ph->speaker;
    out->volume = ph->volume;

    out->battery_pct = -1;
    out->charging = false;
    ui_bridge_format_hhmm(loc->time_valid, loc->hour, loc->minute, out->time_hhmm);
}

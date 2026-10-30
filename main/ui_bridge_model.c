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

ui_blf_state_t ui_bridge_map_blf(blf_state_t st)
{
    switch (st) {
    case BLF_IDLE:    return UI_BLF_IDLE;
    case BLF_RINGING: return UI_BLF_RINGING;
    case BLF_BUSY:    return UI_BLF_BUSY;
    case BLF_UNKNOWN:
    default:          return UI_BLF_UNKNOWN;
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

ui_xfer_state_t ui_bridge_map_xfer(xfer_state_t st)
{
    switch (st) {
    case XFER_CONSULT:  return UI_XFER_CONSULT;
    case XFER_TRYING:   return UI_XFER_TRYING;
    case XFER_ACCEPTED: return UI_XFER_ACCEPTED;
    case XFER_PROGRESS: return UI_XFER_PROGRESS;
    case XFER_DONE:     return UI_XFER_DONE;
    case XFER_FAILED:   return UI_XFER_FAILED;
    case XFER_IDLE:
    default:            return UI_XFER_IDLE;
    }
}

ui_call_state_t ui_bridge_map_second(call_state_t st)
{
    return st == CALL_ACTIVE ? UI_CALL_HELD : ui_bridge_map_call(st);
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

void ui_bridge_format_time(bool valid, int hour, int minute, bool clock_24h, char out[UI_TIME_STR_LEN])
{
    if (!valid || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        out[0] = '\0';
        return;
    }
    size_t n = 0;
    int h = hour;
    if (!clock_24h) {
        h = hour % 12;
        if (h == 0) {
            h = 12;
        }
        if (h >= 10) {
            out[n++] = (char)('0' + h / 10);
        }
    } else {
        out[n++] = (char)('0' + h / 10);
    }
    out[n++] = (char)('0' + h % 10);
    out[n++] = ':';
    out[n++] = (char)('0' + minute / 10);
    out[n++] = (char)('0' + minute % 10);
    if (!clock_24h) {
        out[n++] = ' ';
        out[n++] = hour < 12 ? 'A' : 'P';
        out[n++] = 'M';
    }
    out[n] = '\0';
}

static void copy(char *dst, size_t size, const char *src)
{
    size_t len = 0;
    dst[0] = '\0';
    append(dst, size, &len, src);
}

static void copy_or_empty(char *dst, size_t size, const char *src)
{
    copy(dst, size, src ? src : "");
}

void ui_bridge_fill_settings(ui_bridge_local_t *loc, const char *wifi_ssid, const char *wifi_pass,
                             const char *sip_server, uint16_t sip_port, const char *sip_user,
                             const char *sip_pass, const char *sip_display_name)
{
    copy_or_empty(loc->wifi_ssid, sizeof(loc->wifi_ssid), wifi_ssid);
    loc->wifi_pass_set = wifi_pass && wifi_pass[0];
    copy_or_empty(loc->sip_server, sizeof(loc->sip_server), sip_server);
    loc->sip_port = sip_port;
    copy_or_empty(loc->sip_user, sizeof(loc->sip_user), sip_user);
    loc->sip_pass_set = sip_pass && sip_pass[0];
    copy_or_empty(loc->sip_display_name, sizeof(loc->sip_display_name), sip_display_name);
    ui_bridge_format_account(loc->account, sizeof(loc->account), sip_display_name, sip_user,
                             sip_server);
}

void ui_bridge_fill_time_ota(ui_bridge_local_t *loc, const char *ntp_server, const char *tz,
                             bool clock_24h, const char *ota_url)
{
    copy_or_empty(loc->ntp_server, sizeof(loc->ntp_server), ntp_server);
    copy_or_empty(loc->tz, sizeof(loc->tz), tz);
    loc->clock_24h = clock_24h;
    copy_or_empty(loc->ota_url, sizeof(loc->ota_url), ota_url);
}

void ui_bridge_fill_vm(ui_bridge_local_t *loc, const char *vm_number)
{
    copy_or_empty(loc->vm_number, sizeof(loc->vm_number), vm_number);
}

void ui_bridge_fill_dnd(ui_bridge_local_t *loc, bool dnd)
{
    loc->dnd = dnd;
}

void ui_bridge_fill_sounds(ui_bridge_local_t *loc, int ringtone_count, uint8_t ring_tone, uint8_t ring_volume,
                           uint8_t call_volume, uint8_t speaker_volume, bool call_waiting, bool auto_answer,
                           bool dnd)
{
    loc->ringtone_count = (uint8_t)(ringtone_count < 0 ? 0 : ringtone_count > 255 ? 255 : ringtone_count);
    loc->ring_tone = ring_tone;
    loc->ring_volume = ring_volume > 100 ? 100 : ring_volume;
    loc->call_volume = call_volume > 100 ? 100 : call_volume;
    loc->speaker_volume = speaker_volume > 100 ? 100 : speaker_volume;
    loc->call_waiting = call_waiting;
    loc->auto_answer = auto_answer;
    loc->dnd = dnd;
}

void ui_bridge_fill_fw(ui_bridge_local_t *loc, const char *version, const char *partition)
{
    copy_or_empty(loc->fw_version, sizeof(loc->fw_version), version);
    copy_or_empty(loc->fw_partition, sizeof(loc->fw_partition), partition);
}

ui_ota_state_t ui_bridge_map_ota(ota_state_t st)
{
    switch (st) {
    case OTA_STATE_RUNNING: return UI_OTA_RUNNING;
    case OTA_STATE_DONE:    return UI_OTA_DONE;
    case OTA_STATE_FAILED:  return UI_OTA_FAILED;
    case OTA_STATE_IDLE:
    default:                return UI_OTA_IDLE;
    }
}

void ui_bridge_fill_ota(ui_bridge_local_t *loc, const ota_status_t *st)
{
    loc->ota_state = ui_bridge_map_ota(st->state);
    loc->ota_percent = st->percent > 100 ? 100 : st->percent;
    copy(loc->ota_message, sizeof(loc->ota_message), st->message);
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
    copy(out->wifi_ssid, sizeof(out->wifi_ssid), loc->wifi_ssid);
    out->wifi_pass_set = loc->wifi_pass_set;
    copy(out->sip_server, sizeof(out->sip_server), loc->sip_server);
    out->sip_port = loc->sip_port;
    copy(out->sip_user, sizeof(out->sip_user), loc->sip_user);
    copy(out->sip_display_name, sizeof(out->sip_display_name), loc->sip_display_name);
    out->sip_pass_set = loc->sip_pass_set;

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
    ui_bridge_format_time(loc->time_valid, loc->hour, loc->minute, loc->clock_24h, out->time_hhmm);

    copy(out->ntp_server, sizeof(out->ntp_server), loc->ntp_server);
    copy(out->tz, sizeof(out->tz), loc->tz);
    out->clock_24h = loc->clock_24h;
    copy(out->fw_version, sizeof(out->fw_version), loc->fw_version);
    copy(out->fw_partition, sizeof(out->fw_partition), loc->fw_partition);
    copy(out->ota_url, sizeof(out->ota_url), loc->ota_url);
    out->ota_state = loc->ota_state;
    out->ota_percent = loc->ota_percent;
    copy(out->ota_message, sizeof(out->ota_message), loc->ota_message);

    int n = ph->blf_count < UI_BLF_MAX ? ph->blf_count : UI_BLF_MAX;
    for (int i = 0; i < n; i++) {
        copy(out->blf[i].ext, sizeof(out->blf[i].ext), ph->blf[i].ext);
        copy(out->blf[i].label, sizeof(out->blf[i].label), ph->blf[i].label);
        out->blf[i].state = ui_bridge_map_blf(ph->blf[i].state);
    }
    out->blf_count = (uint8_t)n;
    if (ph->mwi.known) {
        out->mwi_waiting = ph->mwi.waiting;
        out->mwi_new = ph->mwi.new_msgs;
        out->mwi_old = ph->mwi.old_msgs;
    }
    copy(out->vm_number, sizeof(out->vm_number), loc->vm_number);

    /* call features */
    out->dnd = loc->dnd;
    out->auto_answered = out->call != UI_CALL_IDLE && ph->auto_answered;
    out->find_ringing = out->call == UI_CALL_RINGING_IN && ph->find;
    out->second_call = ui_bridge_map_second(ph->second.call);
    if (out->second_call != UI_CALL_IDLE) {
        copy(out->second_number, sizeof(out->second_number), ph->second.number);
        copy(out->second_name, sizeof(out->second_name), ph->second.name);
        out->second_seconds = ui_bridge_call_seconds(ph->second.call, ph->second.call_start_us, now_us);
    }
    out->xfer = ui_bridge_map_xfer(ph->xfer.state);
    if (out->xfer != UI_XFER_IDLE) {
        out->xfer_attended = ph->xfer.attended;
        out->xfer_code = ph->xfer.code;
        copy(out->xfer_target, sizeof(out->xfer_target), ph->xfer.target);
    }
    out->ringtone_count = loc->ringtone_count;
    out->ring_tone = loc->ring_tone;
    out->ring_volume = loc->ring_volume;
    out->call_volume = loc->call_volume;
    out->speaker_volume = loc->speaker_volume;
    out->call_waiting = loc->call_waiting;
    out->auto_answer = loc->auto_answer;
    out->missed_seq = loc->missed_seq;
    out->missed_dnd = loc->missed_dnd;
    copy(out->missed_from, sizeof(out->missed_from), loc->missed_from);
    /* missed_unseen, missed_last, history_rev and now come from the directory / clock in the
     * bridge task */
}

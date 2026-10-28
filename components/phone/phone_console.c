/* Serial console commands for call control (registered by phone_init). */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "phone.h"
#include "phone_priv.h"

/* Command output goes straight to the console with printf (it is the user's
 * reply, not a log), as in components/cli. */

static const char *TAG = "phone";

static const char *reg_name(sip_reg_state_t st)
{
    switch (st) {
    case SIP_REG_NONE:        return "not registered";
    case SIP_REG_REGISTERING: return "registering";
    case SIP_REG_REGISTERED:  return "registered";
    case SIP_REG_FAILED:      return "failed";
    default:                  return "?";
    }
}

static const char *call_name(call_state_t st)
{
    switch (st) {
    case CALL_IDLE:        return "idle";
    case CALL_DIALING:     return "dialing";
    case CALL_RINGING_OUT: return "ringing (out)";
    case CALL_RINGING_IN:  return "ringing (in)";
    case CALL_ACTIVE:      return "active";
    case CALL_HELD:        return "held";
    case CALL_ENDED:       return "ended";
    default:               return "?";
    }
}

static int report(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        printf("%s: %s\n", what, esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int cmd_call(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: call <number|sip:uri>\n");
        return 1;
    }
    return report(phone_dial(argv[1]), "call");
}

static int cmd_answer(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return report(phone_answer(), "answer");
}

static int cmd_hangup(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return report(phone_hangup(), "hangup");
}

static int cmd_reject(int argc, char **argv)
{
    int code = 0;
    if (argc > 2 || (argc == 2 && ((code = atoi(argv[1])) < 400 || code > 699))) {
        printf("usage: reject [400-699]   (default 603 Decline)\n");
        return 1;
    }
    return report(phone_reject(code), "reject");
}

static int cmd_dtmf(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: dtmf <digits 0-9*#A-D>\n");
        return 1;
    }
    return report(phone_dtmf_string(argv[1]), "dtmf");
}

static int cmd_hold(int argc, char **argv)
{
    (void)argc;
    return report(phone_hold(strcmp(argv[0], "hold") == 0), argv[0]);
}

static int cmd_mute(int argc, char **argv)
{
    (void)argc;
    bool mute = strcmp(argv[0], "mute") == 0;
    phone_set_mute(mute);
    printf("microphone %s\n", mute ? "muted" : "on");
    return 0;
}

static int cmd_vol(int argc, char **argv)
{
    phone_status_t st;
    if (argc == 1) {
        phone_get_status(&st);
        printf("volume %u%s\n", st.volume, st.speaker ? " (speaker)" : "");
        return 0;
    }
    char *end = NULL;
    long v = strtol(argv[1], &end, 10);
    if (argc != 2 || *end != '\0' || v < 0 || v > 100) {
        printf("usage: vol [0-100]\n");
        return 1;
    }
    phone_set_volume((uint8_t)v);
    return 0;
}

static int cmd_sip(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    phone_status_t st;
    phone_get_status(&st);
    printf("registration: %s (last code %d)\n", reg_name(st.reg), st.reg_code);
    printf("server:       %s\n", st.server[0] ? st.server : "(none)");
    printf("local:        %s\n", st.local_addr[0] ? st.local_addr : "(none)");
    printf("call:         %s", call_name(st.call));
    if (st.call != CALL_IDLE) {
        printf(" %s %s%s%s", st.outgoing ? "to" : "from", st.number, st.name[0] ? " " : "", st.name);
    }
    if (st.call == CALL_ENDED) {
        printf(" (code %d)", st.end_code);
    }
    if ((st.call == CALL_ACTIVE || st.call == CALL_HELD) && st.call_start_us) {
        printf(" %" PRId64 " s", (esp_timer_get_time() - st.call_start_us) / 1000000);
    }
    printf("\n");
    printf("audio:        volume %u, %s, %s\n", st.volume, st.speaker ? "speaker" : "handset",
           st.muted ? "muted" : "mic on");
    return 0;
}

static int cmd_rtp(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    phone_status_t st;
    phone_get_status(&st);
    const rtp_stats_t *r = &st.rtp;
    printf("%s, pt %u (%s), local %u -> %s\n", st.media_active ? "active" : "last call", st.payload_type,
           phone_codec_name(st.payload_type), st.rtp_local_port, st.rtp_remote[0] ? st.rtp_remote : "-");
    printf("rx %" PRIu32 " lost %" PRIu32 " late %" PRIu32 " dup %" PRIu32 " invalid %" PRIu32 " plc %" PRIu32 "\n",
           r->rx_packets, r->rx_lost, r->rx_late, r->rx_dup, r->rx_invalid, r->plc_frames);
    printf("tx %" PRIu32 " (errors %" PRIu32 "), jitter %" PRIu32 " ms, jb %u/%u ms\n", r->tx_packets,
           st.rtp_tx_errors, r->jitter_ms, r->jb_depth_ms, r->jb_target_ms);
    return 0;
}

esp_err_t phone_console_register(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "call", .help = "Dial a number or sip: URI", .hint = "<number>", .func = cmd_call },
        { .command = "answer", .help = "Answer the incoming call", .func = cmd_answer },
        { .command = "hangup", .help = "End / cancel / reject the call", .func = cmd_hangup },
        { .command = "reject", .help = "Reject the incoming call", .hint = "[code]", .func = cmd_reject },
        { .command = "dtmf", .help = "Send DTMF digits (RFC 4733), 150 ms apart", .hint = "<digits>",
          .func = cmd_dtmf },
        { .command = "hold", .help = "Put the call on hold", .func = cmd_hold },
        { .command = "resume", .help = "Resume a held call", .func = cmd_hold },
        { .command = "mute", .help = "Mute the microphone", .func = cmd_mute },
        { .command = "unmute", .help = "Unmute the microphone", .func = cmd_mute },
        { .command = "vol", .help = "Show/set the current output volume", .hint = "[0-100]", .func = cmd_vol },
        { .command = "sip", .help = "SIP registration and call status", .func = cmd_sip },
        { .command = "rtp", .help = "RTP statistics of the current/last call", .func = cmd_rtp },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(console_register_command(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    return ESP_OK;
}

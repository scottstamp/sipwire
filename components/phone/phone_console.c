/* Serial console commands for call control (registered by phone_init). */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
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

static int cmd_blf(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static phone_status_t st;           /* console task only; keeps the stack small */
    phone_get_status(&st);
    if (st.blf_count == 0) {
        printf("no BLF keys (cfg set blf \"1002:Bob,1003\")\n");
        return 0;
    }
    for (int i = 0; i < st.blf_count; i++) {
        printf("blf %d: %-8s %-12s %s\n", i + 1, st.blf[i].ext, st.blf[i].label[0] ? st.blf[i].label : "-",
               phone_blf_state_name(st.blf[i].state));
    }
    return 0;
}

static int cmd_mwi(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static phone_status_t st;           /* console task only */
    phone_get_status(&st);
    if (!st.mwi.known) {
        printf("mwi: unknown\n");
    } else {
        printf("mwi: %s, %u new, %u old\n", st.mwi.waiting ? "waiting" : "none", (unsigned)st.mwi.new_msgs,
               (unsigned)st.mwi.old_msgs);
    }
    return 0;
}

/* dB with one decimal, without printf float support */
static void print_db(const char *label, float db)
{
    int t = (int)(db * 10.0f + (db < 0 ? -0.5f : 0.5f));
    printf("%s %s%d.%d dB", label, t < 0 ? "-" : "", abs(t) / 10, abs(t) % 10);
}

static int cmd_aec(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "delay") == 0) {
        int d = strcmp(argv[2], "default") == 0 ? -1 : atoi(argv[2]);
        return report(audio_set_aec_delay(d), "aec delay");
    }
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "suppress") == 0) {
        return report(audio_set_aec_suppress(atoi(argv[2]), atoi(argv[3]), argc == 5 ? atoi(argv[4]) : 0),
                      "aec suppress");
    }
    if (argc != 1) {
        printf("usage: aec | aec delay <samples>|default | aec suppress <echo dB> <active dB> [<noise dB>]\n");
        return 1;
    }

    audio_aec_stats_t a;
    audio_get_aec_stats(&a);
    if (!a.built) {
        printf("aec: not built (CONFIG_SIPWIRE_AEC)\n");
        return 0;
    }
    printf("aec: %s, %s (setting aec; `cfg set aec false` to compare)\n", a.enabled ? "on" : "off",
           a.running ? "running" : a.ready ? "idle" : "not allocated");
    printf("speexdsp, tail %" PRIu32 " samples, suppress %d / active %d / noise %d dB (+%" PRIu32
           " latency), delay %" PRIu32 " samples%s\n", a.tail_samples, a.suppress_db, a.suppress_active_db,
           a.noise_db, a.latency_samples, a.delay_samples, a.delay_override ? " (override)" : "");
    printf("frames %" PRIu32 ", avg %" PRIu32 " us/frame, max %" PRIu32 " us/frame, resets %" PRIu32 "\n",
           a.frames, a.avg_us, a.max_us, a.resets);
    printf("ref late %" PRIu32 ", i2s xruns tx %" PRIu32 " rx %" PRIu32 ", heap internal %" PRIu32
           " B psram %" PRIu32 " B\n", a.ref_late, a.tx_xruns, a.rx_xruns, a.mem_internal, a.mem_psram);
    print_db("erl", a.erl_db);
    print_db(", erle", a.erle_db);
    print_db(", erle (2 s)", a.erle_recent_db);
    printf(" (far-end speech only; 0 = no data)\n");
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
        { .command = "aec", .help = "Echo canceller state and statistics of the current/last call; "
                                    "tune the reference delay / residual suppression (dB of attenuation, "
                                    "until the next settings change; cfg set aec_suppress[_act] persists)",
          .hint = "[delay <samples>|default | suppress <echo> <active> [<noise>]]", .func = cmd_aec },
        { .command = "blf", .help = "BLF keys (setting blf) and their state", .func = cmd_blf },
        { .command = "mwi", .help = "Message waiting (voicemail) state", .func = cmd_mwi },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(console_register_command(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    return ESP_OK;
}

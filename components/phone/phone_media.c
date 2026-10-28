/* Media path of the current call: RTP socket + session, the rtp_rx task and
 * the audio source/sink callbacks.
 *
 * Threads touching this state:
 *   sip task      : alloc_port / update / set_state / teardown / audio / dtmf
 *   rtp_rx (core1): recvfrom → rtp_on_packet
 *   audio_tx (c1) : source callback → rtp_pull_frame
 *   audio_rx (c1) : sink callback → rtp_build_audio → sendto
 * Everything in `m` below the "under mtx" line is guarded by m.mtx (the
 * single RTP-session mutex). Hold times are a packet build/parse + one
 * non-blocking sendto.
 *
 * Socket lifetime: the sip task creates sockets. Once the rtp_rx task has
 * adopted the current socket (rx_adopted) only rtp_rx closes it — after its
 * blocking recvfrom returned and it saw that m.fd changed — so no socket is
 * ever closed under a thread that is still using it. */

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "audio.h"
#include "net.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rtp.h"

#include "phone_priv.h"

static const char *TAG = "phone";

#define RTP_RX_STACK        4096
#define RTP_RX_PRIO         (configMAX_PRIORITIES - 4)  /* just below the audio tasks */
#define RTP_RX_CORE         1
#define RTP_RX_TIMEOUT_MS   100
#define RTP_RX_BUF          1500
#define RTP_IP_TOS_EF       0xB8                        /* DSCP EF → WMM voice queue */
#define JB_MIN_MS           40
#define JB_MAX_MS           160

/* audio hands us 16 kHz 20 ms frames, which is what rtp takes for any codec */
_Static_assert(AUDIO_SAMPLE_RATE == RTP_PCM_RATE && AUDIO_FRAME_SAMPLES == RTP_FRAME_SAMPLES,
               "audio and rtp frame formats differ");

static struct {
    SemaphoreHandle_t mtx;
    TaskHandle_t rx_task;

    /* sip task only */
    phone_port_alloc_t ports;
    int pending_fd;                 /* bound by alloc_port, not yet used */
    uint16_t pending_port;
    bool cb_on;                     /* source/sink installed in audio */
    bool running;                   /* audio streaming (callbacks or tone) */
    phone_tone_t tone;

    /* ---- under mtx ---- */
    int fd;
    uint16_t local_port;
    bool rx_adopted;
    rtp_session_t *rtp;
    uint8_t pt;
    int16_t dtmf_pt;
    uint32_t server_ip;             /* SIP server: also accepted as RTP source */
    uint32_t remote_ip;             /* SDP c= address (host order) */
    uint16_t sdp_port;              /* SDP m= port */
    uint32_t tx_ip;                 /* where we send: SDP address until the */
    uint16_t remote_port;           /* ... first valid packet latches its source */
    bool latched;
    sip_media_dir_t dir;
    bool early;
    sip_ua_call_state_t state;
    bool send, play;
    rtp_stats_t stats;              /* last snapshot (kept after teardown) */
    uint32_t tx_errors;
} m = {
    .pending_fd = -1,
    .fd = -1,
};

static uint32_t now_ms32(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lock(void)
{
    xSemaphoreTake(m.mtx, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(m.mtx);
}

/* ------------------------------------------------------------------ */
/* sockets                                                            */
/* ------------------------------------------------------------------ */

static int open_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = RTP_RX_TIMEOUT_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int tos = RTP_IP_TOS_EF;
    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    return fd;
}

static void close_pending(void)
{
    if (m.pending_fd >= 0) {
        close(m.pending_fd);
        m.pending_fd = -1;
        m.pending_port = 0;
    }
}

/* Caller holds mtx. */
static void release_fd_locked(void)
{
    if (m.fd >= 0 && !m.rx_adopted) {
        close(m.fd);                /* rtp_rx never saw it */
    }
    /* else: rtp_rx closes it once its recvfrom returns */
    m.fd = -1;
    m.rx_adopted = false;
    m.local_port = 0;
}

/* Caller holds mtx. */
static void recompute_locked(void)
{
    bool send = false, play = false;
    if (m.rtp) {
        phone_media_flags(m.state, m.dir, m.early, &send, &play);
    }
    m.send = send;
    m.play = play;
}

/* ------------------------------------------------------------------ */
/* rtp_rx task                                                        */
/* ------------------------------------------------------------------ */

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t buf[RTP_RX_BUF];
    int fd = -1;

    for (;;) {
        if (fd < 0) {
            lock();
            if (m.fd >= 0) {
                fd = m.fd;
                m.rx_adopted = true;
            }
            unlock();
            if (fd < 0) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
                continue;
            }
        }

        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&sa, &sl);
        int err = n < 0 ? errno : 0;
        uint32_t now = now_ms32();
        bool latched_now = false;
        uint32_t latched_ip = 0, sdp_ip = 0;
        uint16_t latched_port = 0, sdp_port = 0;

        lock();
        if (m.fd != fd) {
            unlock();
            close(fd);
            fd = -1;
            continue;
        }
        if (n > 0 && m.rtp && sa.sin_family == AF_INET &&
            phone_rtp_source_ok(m.remote_ip, m.server_ip, ntohl(sa.sin_addr.s_addr), buf, (size_t)n)) {
            if (!m.latched) {
                m.latched = true;
                latched_now = true;
                sdp_ip = m.remote_ip;
                sdp_port = m.sdp_port;
                latched_ip = ntohl(sa.sin_addr.s_addr);
                latched_port = ntohs(sa.sin_port);
                m.tx_ip = latched_ip;
                m.remote_port = latched_port;
            }
            if (m.play) {
                /* nobody pulls frames otherwise: don't fill the jitter buffer */
                rtp_on_packet(m.rtp, buf, (size_t)n, now);
            }
        }
        unlock();

        if (latched_now && (latched_ip != sdp_ip || latched_port != sdp_port)) {
            char from[16], sdp[16];
            sip_ip_format(latched_ip, from);
            sip_ip_format(sdp_ip, sdp);
            ESP_LOGI(TAG, "rtp: peer sends from %s:%u (SDP %s:%u), latched", from, latched_port, sdp, sdp_port);
        }
        if (n < 0 && err != EAGAIN && err != EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(20));  /* socket error: don't spin */
        }
    }
}

/* ------------------------------------------------------------------ */
/* audio callbacks (audio tasks, core 1)                              */
/* ------------------------------------------------------------------ */

static void audio_source(int16_t frame[AUDIO_FRAME_SAMPLES], void *ctx)
{
    (void)ctx;
    lock();
    if (m.rtp && m.play) {
        rtp_pull_frame(m.rtp, frame, now_ms32());
    }
    unlock();
}

static void audio_sink(const int16_t frame[AUDIO_FRAME_SAMPLES], void *ctx)
{
    (void)ctx;
    uint8_t pkt[RTP_MAX_PACKET];
    lock();
    if (m.rtp && m.send && m.fd >= 0 && m.tx_ip != 0 && m.remote_port != 0) {
        size_t n = rtp_build_audio(m.rtp, frame, pkt, sizeof(pkt));
        if (n > 0) {
            struct sockaddr_in to = {
                .sin_family = AF_INET,
                .sin_port = htons(m.remote_port),
                .sin_addr.s_addr = htonl(m.tx_ip),
            };
            if (sendto(m.fd, pkt, n, MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to)) != (int)n) {
                m.tx_errors++;
            }
        }
    }
    unlock();
}

/* ------------------------------------------------------------------ */
/* sip task API                                                       */
/* ------------------------------------------------------------------ */

esp_err_t phone_media_init(void)
{
    m.mtx = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(m.mtx, ESP_ERR_NO_MEM, TAG, "media mutex");
    phone_port_alloc_init(&m.ports, esp_random());
    BaseType_t ok = xTaskCreatePinnedToCore(rx_task, "rtp_rx", RTP_RX_STACK, NULL, RTP_RX_PRIO, &m.rx_task,
                                            RTP_RX_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "rtp_rx task");
    return ESP_OK;
}

uint16_t phone_media_alloc_port(void *ctx)
{
    (void)ctx;
    close_pending();

    lock();
    uint16_t in_use = m.fd >= 0 ? m.local_port : 0;
    unlock();

    uint16_t port = 0;
    for (unsigned i = 0; i < PHONE_RTP_PORT_COUNT; i++) {
        port = phone_port_alloc_next(&m.ports, in_use);
        int fd = open_socket(port);
        if (fd >= 0) {
            m.pending_fd = fd;
            m.pending_port = port;
            return port;
        }
    }
    ESP_LOGE(TAG, "rtp: no free UDP port in %u-%u", PHONE_RTP_PORT_MIN, PHONE_RTP_PORT_MAX);
    return port;
}

void phone_media_update(const sip_media_info_t *mi)
{
    int new_fd = -1;
    bool need_socket;

    net_set_low_latency(true);      /* idempotent; undone in teardown */
    lock();
    if (m.fd >= 0 && m.local_port != mi->local_port) {
        release_fd_locked();        /* should not happen within one call */
    }
    need_socket = m.fd < 0;
    unlock();

    if (need_socket) {
        if (m.pending_fd >= 0 && m.pending_port == mi->local_port) {
            new_fd = m.pending_fd;
            m.pending_fd = -1;
            m.pending_port = 0;
        } else {
            close_pending();
            new_fd = open_socket(mi->local_port);
            if (new_fd < 0) {
                ESP_LOGE(TAG, "rtp: cannot bind port %u (errno %d)", mi->local_port, errno);
            }
        }
    }

    bool created = false, failed = false;
    lock();
    if (new_fd >= 0) {
        m.fd = new_fd;
        m.local_port = mi->local_port;
        m.rx_adopted = false;
    }
    int16_t dtmf_pt = mi->dtmf_pt > 0 && mi->dtmf_pt < 128 ? mi->dtmf_pt : -1;
    if (!m.rtp || m.pt != mi->payload_type || m.dtmf_pt != dtmf_pt) {
        if (m.rtp) {
            rtp_session_destroy(m.rtp);
        }
        const rtp_config_t cfg = {
            .payload_type = mi->payload_type,
            .dtmf_payload_type = dtmf_pt > 0 ? (uint8_t)dtmf_pt : 0,
            .ssrc = esp_random(),
            .initial_seq = (uint16_t)esp_random(),
            .initial_ts = esp_random(),
            .jb_min_ms = JB_MIN_MS,
            .jb_max_ms = JB_MAX_MS,
        };
        m.rtp = rtp_session_create(&cfg);
        m.pt = mi->payload_type;
        m.dtmf_pt = dtmf_pt;
        m.tx_errors = 0;
        memset(&m.stats, 0, sizeof(m.stats));
        created = m.rtp != NULL;
        failed = m.rtp == NULL;
    }
    if (m.remote_ip != mi->remote_ip || m.sdp_port != mi->remote_port) {
        /* new or changed remote: send to the SDP address until a packet latches */
        m.remote_ip = mi->remote_ip;
        m.sdp_port = mi->remote_port;
        m.tx_ip = mi->remote_ip;
        m.remote_port = mi->remote_port;
        m.latched = false;
    }
    m.dir = mi->direction;
    m.early = mi->early;
    recompute_locked();
    unlock();

    if (new_fd >= 0) {
        xTaskNotifyGive(m.rx_task);
    }
    if (failed) {
        ESP_LOGE(TAG, "rtp: session alloc failed");
    } else if (created) {
        ESP_LOGI(TAG, "rtp: local %u -> %s:%u pt %u (%s) dtmf %d", mi->local_port, mi->remote_ip_str,
                 mi->remote_port, mi->payload_type, phone_codec_name(mi->payload_type), (int)dtmf_pt);
    }
}

void phone_media_set_state(sip_ua_call_state_t state)
{
    lock();
    m.state = state;
    recompute_locked();
    unlock();
}

void phone_media_teardown(void)
{
    lock();
    if (m.rtp) {
        rtp_get_stats(m.rtp, &m.stats);
        rtp_session_destroy(m.rtp);
        m.rtp = NULL;
    }
    release_fd_locked();
    m.remote_ip = 0;
    m.tx_ip = 0;
    m.sdp_port = 0;
    m.remote_port = 0;
    m.latched = false;
    m.early = false;
    m.dir = SIP_DIR_SENDRECV;
    recompute_locked();
    unlock();
    close_pending();
    net_set_low_latency(false);
}

void phone_media_set_server_ip(uint32_t ip)
{
    lock();
    m.server_ip = ip;
    unlock();
}

bool phone_media_active(void)
{
    lock();
    bool active = m.rtp != NULL;
    unlock();
    return active;
}

bool phone_media_early(void)
{
    lock();
    bool early = m.rtp != NULL && m.early;
    unlock();
    return early;
}

static audio_tone_t to_audio_tone(phone_tone_t t)
{
    switch (t) {
    case PHONE_TONE_RINGBACK: return AUDIO_TONE_RINGBACK;
    case PHONE_TONE_RING:     return AUDIO_TONE_RING;
    case PHONE_TONE_BUSY:     return AUDIO_TONE_BUSY;
    case PHONE_TONE_NONE:
    default:                  return AUDIO_TONE_NONE;
    }
}

void phone_media_audio(bool stream, phone_tone_t tone)
{
    if (!stream && tone == PHONE_TONE_NONE) {
        if (m.running) {
            audio_stop();
        }
        m.running = false;
        m.cb_on = false;
        m.tone = PHONE_TONE_NONE;
        return;
    }
    if (stream != m.cb_on) {
        /* audio_start on a running stream only swaps the callbacks */
        esp_err_t err = stream ? audio_start(audio_source, audio_sink, NULL) : audio_start(NULL, NULL, NULL);
        if (err == ESP_OK) {
            m.cb_on = stream;
            m.running = true;
        } else {
            ESP_LOGE(TAG, "audio_start: %s", esp_err_to_name(err));
        }
    }
    if (tone != m.tone) {
        audio_play_tone(to_audio_tone(tone));   /* auto-starts the stream if needed */
        m.tone = tone;
        m.running = true;
    }
}

phone_dtmf_res_t phone_media_send_dtmf(char digit)
{
    phone_dtmf_res_t res;
    lock();
    if (!m.rtp || !m.send || m.dtmf_pt <= 0) {
        res = PHONE_DTMF_UNAVAILABLE;
    } else {
        res = rtp_send_dtmf(m.rtp, digit, PHONE_DTMF_DURATION_MS) ? PHONE_DTMF_SENT : PHONE_DTMF_BUSY;
    }
    unlock();
    return res;
}

void phone_media_status(phone_status_t *st)
{
    uint32_t rip;
    uint16_t rport;
    lock();
    if (m.rtp) {
        rtp_get_stats(m.rtp, &m.stats);
    }
    st->media_active = m.rtp != NULL;
    st->payload_type = m.pt;
    st->rtp = m.stats;
    st->rtp_tx_errors = m.tx_errors;
    st->rtp_local_port = m.local_port;
    rip = m.tx_ip;
    rport = m.remote_port;
    unlock();

    if (rip != 0) {
        char ip[16];
        sip_ip_format(rip, ip);
        snprintf(st->rtp_remote, sizeof(st->rtp_remote), "%s:%u", ip, rport);
    } else {
        st->rtp_remote[0] = '\0';
    }
}

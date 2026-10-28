# sip — minimal SIP user agent

Own RFC 3261 UA for the handset (see `docs/ARCHITECTURE.md`, "SIP stack
choice"): UDP, REGISTER with digest auth, INVITE/ACK/BYE/CANCEL/OPTIONS,
re-INVITE (hold), SDP offer/answer for G.722 / G.711 + telephone-event. No media —
the UA reports the negotiated RTP parameters and `rtp` (Phase 3) does the rest.

## Layout

| file | role |
|------|------|
| `include/sip_ua.h` | the only public header: config, API, callbacks, UDP glue |
| `sip_msg.c` | parser (in place, bounded, compact forms, folding, lists) + writer |
| `sip_md5.c` | MD5 (RFC 1321, written from the spec) |
| `sip_auth.c` | digest (RFC 2617: MD5, qop=auth / legacy), challenge parsing |
| `sip_sdp.c` | SDP parse / negotiate / build (G722, PCMU, PCMA, telephone-event, ptime, direction) |
| `sip_txn.c` | transactions (§17 + RFC 6026 Accepted), timers A–K, ACK for non-2xx |
| `sip_dialog.c` | dialogs: tags, remote target, route set (loose/strict), CSeq |
| `sip_ua.c` | TU: registration, calls, request dispatch, callbacks |
| `sip_udp.c` | BSD-socket UDP glue — the only file that touches the network |

The core (everything but `sip_udp.c`) is pure C17 with no OS headers: time is
injected with `sip_ua_tick()`, datagrams with `sip_ua_on_packet()`, output goes
through `cfg.send`, randomness through `cfg.rand`, logs through `cfg.log`.

## Using it (sip task)

```c
static sip_udp_t udp;
sip_udp_open(&udp, 5060);
sip_addr_t srv;
sip_udp_resolve(settings.sip_server, settings.sip_port, &srv);   /* DNS here, not in the core */
uint32_t lip;
sip_udp_local_ip_for(&srv, &lip);

sip_ua_config_t cfg = {
    .server_host = settings.sip_server, .server_port = settings.sip_port, .server_addr = srv,
    .user = settings.sip_user, .auth_user = settings.sip_auth_user,
    .password = settings.sip_password, .display_name = settings.sip_display_name,
    .expires = settings.sip_reg_expires,
    .local_addr = { lip, udp.port }, .rtp_port = 40000,
    .codecs = { 9, 0, 8 }, .n_codecs = 3,   /* G.722 > PCMU > PCMA (also the default) */
    .keepalive_s = 25,
    .send = sip_udp_send, .send_ctx = &udp,
    .rand = esp_rand_cb,            /* wraps esp_random() */
    .log = esp_log_cb,              /* maps sip_log_level_t to ESP_LOGx, tag "sip" */
    .cb = { on_reg, on_incoming, on_call_state, on_media, ctx },
};
sip_ua_t *ua;
sip_ua_create(&cfg, &ua);
sip_ua_register(ua);
for (;;) {
    sip_udp_poll(&udp, ua, 50);                       /* feeds sip_ua_on_packet() */
    sip_ua_tick(ua, esp_timer_get_time() / 1000);
    /* + drain a command queue from the phone task: dial/answer/hangup/hold */
}
```

`sip_ua_reg_state_t` and `sip_ua_call_state_t` have the same order as
`sip_reg_state_t` / `call_state_t` in `sipwire_events.h`, so the phone can cast.

### Codecs

`cfg.codecs[]` is the preference list of static payload types: 9 = G.722,
0 = PCMU, 8 = PCMA (no duplicates; `n_codecs = 0` means `{9, 0, 8}`). Our
offer lists them in that order plus telephone-event, e.g.

```
m=audio 4000 RTP/AVP 9 0 8 101
a=rtpmap:9 G722/8000
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:101 telephone-event/8000
a=fmtp:101 0-16
a=ptime:20
```

When answering we pick the first entry of our list that the offer contains;
for an answer to our offer, the answerer's first format we support. G.722 is
announced as `G722/8000`: RFC 3551 fixes its RTP clock at 8000 Hz although it
samples at 16 kHz (`G722/16000` from a broken peer is tolerated). The `media`
callback reports the chosen PT (9, 0 or 8); `rtp` handles the rest.
Functions return `sip_err_t` (0 / negative) because the core cannot depend on
`esp_err.h`; the phone maps them.

### API summary

* lifecycle: `sip_ua_create`, `sip_ua_destroy`, `sip_ua_tick`, `sip_ua_on_packet`
* registration: `sip_ua_register` (refresh at min(expires/2, expires−30),
  honours the granted expires, 423 Min-Expires, backoff 5 s → 300 s on failure),
  `sip_ua_unregister` (Expires: 0), `sip_ua_reg_state`, `sip_ua_set_local_addr`
* calls: `sip_call_dial` → handle, `sip_call_answer`, `sip_call_reject(code)`,
  `sip_call_hangup` (CANCEL / 486 / BYE as appropriate), `sip_call_hold(bool)`,
  `sip_call_state`
* callbacks: `reg_state(state, code)`, `incoming_call(handle, number, name)`,
  `call_state(handle, info)` (state, end reason, status code, number, name),
  `media(handle, info)` (remote IP/port, PT 9/0/8, telephone-event PT, ptime,
  direction from our point of view, local RTP port, early flag)

### Callback rules

Callbacks are queued internally and delivered at the end of the outermost
public call (tick / on_packet / API), when internal state is consistent. They
may call any API function, including `sip_call_answer` from `incoming_call`
and even `sip_ua_destroy` (deferred until the outermost call returns; don't
touch the handle afterwards). String arguments are valid only during the call.
Not thread-safe: use one task.

## Behaviour notes

* All requests go to `server_addr` (outbound-proxy style); responses go to the
  source address of the request (RFC 3581). Datagrams from any other IP are
  dropped unless `accept_any_source` is set.
* NAT: `rport` is always requested; if the REGISTER response shows a different
  `received`/`rport`, the Contact (and SDP `c=`) switch to the public address
  and the UA re-registers. Keepalive: CRLF or OPTIONS (timeout → re-register).
* Auth: one cached challenge is reused (nc incremented) for REGISTER refreshes,
  INVITEs and re-INVITEs; a request is retried once per challenge, again only
  on `stale=true` (max 3). Passwords and Authorization values are never logged.
* One visible call. A second INVITE while busy gets 486; a hung-up call may
  linger internally (CANCEL / 487 / 200-race → ACK + BYE) without blocking a
  new call.
* Hangup on an incoming ringing call sends 486; `sip_call_reject` lets the
  app pick (e.g. 603). Hangup of an answered call before its ACK waits for the
  ACK (or its timeout) before sending BYE (§15).
* Remote re-INVITE: answered with matching direction (sendonly → recvonly,
  inactive → inactive, c=0.0.0.0 honoured), reported via `media`. 491 on glare.
  Re-INVITE / initial INVITE without SDP: we offer in the 200, answer is taken
  from the ACK.
* In-dialog INFO → 200, OPTIONS → 200, other unknown → 501; out-of-dialog
  OPTIONS → 200, NOTIFY → 200, known-but-unsupported methods → 405, unknown →
  501, Require → 420, non-sip R-URI → 416.

## Memory

One `calloc` in `sip_ua_create` (~40–45 KB: 10 transactions × 1.5 KB, 2 call
slots, parse/build scratch). No allocation afterwards. Message buffers live in
the UA, so stack frames stay small (largest ≈ 1 KB for a parsed SDP answer,
plus vsnprintf); a 6 KB sip task stack is plenty (log callback not included).
`sip_udp_poll` uses a static 2 KB receive buffer (one sip task).
Limits: 2048 B in (larger datagrams are dropped), 1500 B out, 8 Via / Route /
Contact entries per message, 64 header lines.

## Limitations / not implemented

* UDP only (no TCP/TLS, no SRV/NAPTR — the app resolves A records), IPv4 only.
* Single route target: requests are always sent to the configured server even
  if Route/Contact name another host.
* No forking support (a second 2xx with another To-tag is just ACKed), no
  PRACK/100rel, no UPDATE, no session timers, no REFER/transfer, no MESSAGE,
  no SUBSCRIBE/MWI.
* RFC 2543 (no magic cookie) request retransmissions are not matched.
* Codecs: G.722 and G.711 only, static payload types 9/0/8; one audio stream.
* Re-INVITE glare (491) is reported/logged but not retried automatically.
* Digest: MD5 and qop=auth only (no MD5-sess, auth-int, SHA-256).
* Raw NUL / control characters in headers are rejected (RFC 4475 "intmeth"
  accepts them; we choose to refuse).

## Tests

Host (`tools/ci.sh host-test`, needs `IDF_PATH` for Unity), under ASan/UBSan:

* `test_sip_msg` — parser incl. RFC 4475 torture cases (wsinv, intmeth, esc01,
  escnull, lwsdisp, semiuri, clerr, scalar02/scalarlg, quotbal, badinv01,
  ltgtruri, lwsruri, lwsstart, trws, badvers, bigcode, mismatch01, insuf,
  multi01), compact forms, lists, URIs, writer.
* `test_sip_auth` — RFC 1321 vectors, RFC 2617 §3.5 digest, challenge parsing.
* `test_sip_sdp` — parse/negotiate/build, G.722 negotiation and offer, hold
  variants, failures.
* `test_sip_ua` — fake transport + virtual clock: REGISTER 401→200, refresh
  timing, backoff, timers B/F/G/H, NAT rport, keepalive, outgoing call, 407,
  486 (ACK), CANCEL (incl. before provisional and the 200 race), incoming
  answer/reject/remote CANCEL/busy/488, BYE both ways, retransmissions,
  re-INVITE hold (remote and local), delayed offer, G.722 offer/answer and
  codec preference config, callback re-entrancy.
* `test_sip_fuzz` — 50k deterministic mutations through all parsers plus 10k
  through the full UA.

Integration against a real Asterisk 20 (private instance on 127.0.0.1:5080,
config in `test_host/sip/asterisk/`; not in ctest):

```
export IDF_PATH=...            # only needed if sipcli is not built yet
test_host/sip/integration.sh   # KEEP_STATE=1 / VERBOSE=1 for debugging
```

First it checks the G.722 codec against Asterisk's `codec_g722` (`file
convert` both ways must be bit-exact with `g722tool`). Then it registers 1001,
checks `pjsip show contacts`, calls 600 (Echo: PT 9 negotiated, a 5 kHz tone
sent as G.722 RTP must come back at the same level, hold/resume, BYE → 200),
calls 602 (Asterisk records the 5 kHz tone through its own G.722 decoder;
`tone_check.py` verifies the recording), calls 601 (remote BYE), answers an
inbound `channel originate PJSIP/1001 application Wait 3`, and unregisters.
A second, PCMU-only UA (1002) then echoes a 1 kHz tone through the
resampled G.711 path.
`build_host/sip/sipcli` can also be used by hand against any server.

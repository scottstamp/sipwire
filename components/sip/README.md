# sip — minimal SIP user agent

Own RFC 3261 UA for the handset (see `docs/ARCHITECTURE.md`, "SIP stack
choice"): UDP, REGISTER with digest auth, INVITE/ACK/BYE/CANCEL/OPTIONS,
re-INVITE (hold), SDP offer/answer for G.722 / G.711 + telephone-event, and
RFC 6665 subscriptions for BLF (`dialog`, RFC 4235) and MWI
(`message-summary`, RFC 3842). No media — the UA reports the negotiated RTP
parameters and `rtp` (Phase 3) does the rest.

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
| `sip_sub.c` | TU: SUBSCRIBE dialogs (BLF, MWI), NOTIFY, unsolicited MWI |
| `sip_event.c` | Event / Subscription-State headers, dialog-info+xml scanner, message-summary |
| `private_include/sip_ua_priv.h` | UA internals shared by `sip_ua.c` and `sip_sub.c` |
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
    .cb = { .reg_state = on_reg, .incoming_call = on_incoming, .call_state = on_call_state,
            .media = on_media, .blf_state = on_blf, .mwi = on_mwi, .ctx = ctx },
};
sip_ua_t *ua;
sip_ua_create(&cfg, &ua);
sip_ua_register(ua);
sip_sub_handle_t bob;
sip_ua_subscribe_blf(ua, "1002", &bob);   /* sent once registered */
sip_ua_subscribe_mwi(ua, NULL);
for (;;) {
    /* event-driven: sleep until a datagram, a wake-up on wake_fd (eventfd written by
     * whoever queues a command) or the UA's next timer; UINT64_MAX = none armed */
    uint64_t now = esp_timer_get_time() / 1000, d = sip_ua_next_deadline_ms(ua);
    int wait = d <= now ? 0 : d - now > 2000 ? 2000 : (int)(d - now);
    sip_udp_poll_wake(&udp, ua, wait, wake_fd);       /* feeds sip_ua_on_packet() */
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
  (or zero-copy `sip_ua_rx_buffer` + `sip_ua_on_rx`),
  `sip_ua_next_deadline_ms` (earliest timer: retransmissions, transaction timers,
  re-REGISTER / retry / keepalive, subscription refresh / retry / expiry, call cleanup,
  hold re-INVITE, REFER timeout; host tests check it on every tick of every test)
* registration: `sip_ua_register` (refresh at min(expires/2, expires−30),
  honours the granted expires, 423 Min-Expires, backoff 5 s → 300 s on failure),
  `sip_ua_unregister` (Expires: 0), `sip_ua_reg_state`, `sip_ua_set_local_addr`
* calls: `sip_call_dial` → handle, `sip_call_answer`, `sip_call_reject(code)`,
  `sip_call_hangup` (CANCEL / 486 / BYE as appropriate), `sip_call_hold(bool)`,
  `sip_call_state`, `sip_call_transfer(target)` (blind REFER),
  `sip_call_transfer_replace(call, consult)` (attended, Refer-To with Replaces),
  `sip_call_send_info_dtmf(digit, ms)` (INFO application/dtmf-relay)
* incoming policy: `sip_ua_set_call_waiting(bool)` (default on), `sip_ua_set_dnd(bool)`
* subscriptions: `sip_ua_subscribe_blf(ua, ext, &handle)` (≤ `SIP_UA_MAX_BLF` = 8),
  `sip_ua_subscribe_mwi(ua, &handle)` (one), `sip_ua_unsubscribe(ua, handle)`,
  `sip_ua_blf_state(ua, handle)`; `cfg.sub_expires` (default 600 s)
* callbacks: `reg_state(state, code)`, `incoming_call(handle, number, name)`,
  `call_state(handle, info)` (state, end reason, status code, number, name),
  `media(handle, info)` (remote IP/port, PT 9/0/8, telephone-event PT, ptime,
  direction from our point of view, local RTP port, early flag),
  `blf_state(handle, ext, state)` (UNKNOWN / IDLE / RINGING / BUSY, on change),
  `mwi(waiting, new, old)` (on change; subscription or unsolicited NOTIFY),
  `transfer(handle, state, code)` (TRYING / ACCEPTED / PROGRESS / SUCCESS / FAILED),
  `call_rejected(number, name, code)` (INVITE refused without being offered:
  busy, DND); `sip_call_info_t.answer_after` (auto answer request, -1 = none)

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
* Up to two visible calls. A second INVITE is offered (call waiting) only
  while exactly one call exists and it is established, and call waiting is
  on; otherwise 486 (+ `call_rejected`). DND: every new INVITE gets 486, no
  180. A second outgoing call is allowed next to one established call
  (consultation). A hung-up call may linger internally (CANCEL / 487 /
  200-race → ACK + BYE) without blocking a new call (4 slots).
* Outgoing INVITE: after a provisional, timer C (180 s, restarted by each
  1xx) bounds the wait for a final response: on expiry the call ends with
  408 (`SIP_END_TIMEOUT`) and a CANCEL goes out. Once CANCELled, the INVITE
  transaction gives up 64*T1 later if no 487 arrives (§9.1), so a PBX that
  vanishes mid-ring cannot pin transactions.
* Hold: `sip_call_hold` records the wanted state; the re-INVITE goes out when
  no INVITE transaction of the call is in flight. 491 → retried after the
  §14.1 delay (2.1-4 s if we placed the call / own the Call-ID, else 0-2 s);
  other failures give up (state unchanged).
* Transfer (RFC 3515): in-dialog REFER (Refer-To, Referred-By, Contact; auth
  retried once). 2xx → ACCEPTED; in-dialog NOTIFY `Event: refer` with
  `message/sipfrag` → PROGRESS (1xx) / SUCCESS (2xx) / FAILED (>= 300, or
  `Subscription-State: terminated` without a final status), 60 s without a
  final NOTIFY → FAILED 408. The app hangs up after SUCCESS (Asterisk keeps
  the transferor up, RFC 5589). Attended: `Refer-To: <consult-target?Replaces=
  callid%3Bto-tag%3D<remote tag>%3Bfrom-tag%3D<local tag>>` (RFC 3891: to-tag
  = the target's local tag = our remote tag), value escaped per §25.1 hvalue.
  Incoming REFER → 603. Incoming INVITE with Replaces is not supported (treated
  as a new call; `Require: replaces` → 420).
* Auto answer: `Call-Info: <...>;answer-after=N`, or Alert-Info with
  `info=alert-autoanswer` / `Ring Answer` / `intercom` / `auto answer`
  (outside the `<URI>`, optional `;delay=N`) → `answer_after` = N (else 0).
* Hangup on an incoming ringing call sends 486; `sip_call_reject` lets the
  app pick (e.g. 603). Hangup of an answered call before its ACK waits for the
  ACK (or its timeout) before sending BYE (§15).
* Remote re-INVITE: answered with matching direction (sendonly → recvonly,
  inactive → inactive, c=0.0.0.0 honoured), reported via `media`. 491 on glare.
  Re-INVITE / initial INVITE without SDP: we offer in the 200, answer is taken
  from the ACK.
* In-dialog INFO → 200 (DTMF from application/dtmf-relay `Signal=` or
  application/dtmf is logged), OPTIONS → 200, NOTIFY → refer subscription
  (other event packages 489), other unknown → 501; out-of-dialog
  OPTIONS → 200, NOTIFY with `Event: message-summary` → 200 (unsolicited MWI),
  any other NOTIFY that matches no subscription → 481, known-but-unsupported
  methods (incl. SUBSCRIBE: we are no notifier) → 405, unknown → 501, Require
  → 420, non-sip R-URI → 416.

### Subscriptions (RFC 6665)

* A subscription is created in WAIT and its SUBSCRIBE (Event, Accept,
  Expires, Contact; digest from the cached challenge, 401/407 retried as for
  INVITE) goes out once the registration is REGISTERED. At most 3 SUBSCRIBE
  transactions are in flight at a time (initial, refresh and un-SUBSCRIBE),
  so 8 BLF + MWI at boot or at a common refresh time never exhaust the
  transaction pool (16, lingering ones are evicted; tested with a call up).
* The dialog comes from the 2xx or from a NOTIFY that arrives first (we are
  the NOTIFY's UAS: remote tag, Contact, Record-Route in order). Every NOTIFY
  is answered: 200; 481 if it matches no subscription (Call-ID + our tag, and
  the remote tag once known); 489 for the wrong Event package; 500 for a
  CSeq that does not increase. NOTIFY is a target refresh.
* Refresh at min(expires/2, expires−30) of the granted Expires (2xx header,
  or `Subscription-State: ...;expires=` when the notifier shortens it), sent
  in the dialog. 423 → Min-Expires; a 481/408 to a refresh starts a new
  subscription at once; a subscription that runs out does too.
* Failure (timeout, 4xx-6xx) or `Subscription-State: terminated`: BLF state
  UNKNOWN, retry with backoff 5 s → 300 s. 403/404/405/489/501/603/604 and
  `reason=rejected|noresource|invariant` start at 60 s ("don't hammer");
  a `retry-after` (header or param) is honoured (capped at 1 h).
  `reason=deactivated|timeout` (or none) re-subscribes immediately, keeping
  the lamp, unless the subscription lived < 30 s (then it backs off too, so a
  notifier that ends every subscription right away is not hammered).
  Registration (re)established → pending soft retries go now; Contact change
  (NAT, `sip_ua_set_local_addr`) → established ones refresh now.
* `sip_ua_unsubscribe`: SUBSCRIBE Expires: 0 in the dialog (the final NOTIFY
  is answered and ignored); one still in flight is abandoned (a later NOTIFY
  gets 481, which ends it at the notifier). `sip_ua_unregister` ends them all.
* BLF from dialog-info: `state="full"` replaces the remembered dialogs (≤ 4
  per extension), `partial` merges (terminated removes); `version` not greater
  than the last one → ignored; a gap in partial versions → refresh now (the
  notifier answers with full state). Any `early`/`trying`/`proceeding` dialog
  that is not `direction="initiator"` → RINGING, else any live dialog → BUSY
  (placing a call counts as busy), none → IDLE. Asterisk sends full state.
* The dialog-info scanner is not an XML parser: one pass, no recursion, no
  entity expansion, ≤ 4096 bytes, depth ≤ 16, namespace prefixes ignored;
  DOCTYPE/CDATA, unbalanced or unterminated markup → the body is ignored
  (NOTIFY still answered 200).
* MWI from message-summary: `Messages-Waiting: yes|no` (required) and
  `Voice-Message: new/old (urgent/urgent)`; reported only when it changes,
  whether it came from our subscription or unsolicited.

## Memory

One `calloc` in `sip_ua_create` (~77 KB: 16 transactions × 1.8 KB, 2 call
slots × 6 KB, 9 subscription slots × 2.2 KB, event queue, parse/build scratch;
on the device it lands in PSRAM). No allocation afterwards. Message buffers
live in the UA, so stack frames stay small (largest ≈ 1 KB for a parsed SDP
answer or dialog-info document, plus vsnprintf); a 6 KB sip task stack is
plenty (log callback not included).
`sip_udp_poll` receives straight into the UA's own 2 KB receive buffer
(`sip_ua_rx_buffer` / `sip_ua_on_rx`; no static copy buffer, so no internal
RAM); `sip_ua_on_packet` copies into the same buffer.
Limits: 2048 B in (larger datagrams are dropped), 1500 B out, 8 Via / Route /
Contact entries per message, 64 header lines. A request whose response cannot
be built (Via / From / Call-ID over the 800 B template, To or Record-Route too
long) gets no answer and its server transaction is dropped at once; a final
response that does not fit 1500 B (e.g. a 200 + SDP behind long Via and
Record-Route) is replaced by a bare 500. Server transactions also carry a
safety timeout from creation (64*T1, INVITE 180 s: an unanswered ringing call
then gets 480), so no request can hold a pool slot forever.

## Limitations / not implemented

* UDP only (no TCP/TLS, no SRV/NAPTR — the app resolves A records), IPv4 only.
* Single route target: requests are always sent to the configured server even
  if Route/Contact name another host.
* No forking support (a second 2xx with another To-tag is just ACKed), no
  PRACK/100rel, no UPDATE, no session timers, no MESSAGE; REFER sent only
  (incoming REFER declined, no INVITE/Replaces handling).
* Subscriber only (incoming SUBSCRIBE → 405); one dialog per subscription
  (a forked NOTIFY from another notifier gets 481); event packages `dialog`
  and `message-summary` only.
* RFC 2543 (no magic cookie) request retransmissions are not matched.
* Codecs: G.722 and G.711 only, static payload types 9/0/8; one audio stream.
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
  codec preference config, callback re-entrancy, pool slots of requests that
  cannot be answered, 200 too big → 500, unanswered call → 480, timer C,
  unanswered CANCEL, zero-copy receive (also `sip_udp_poll` on loopback).
* `test_sip_sub` — SUBSCRIBE/401/200/NOTIFY, NOTIFY before the 2xx, refresh
  timing (granted / shortened expires), terminated + noresource / probation +
  retry-after / deactivated, 404 backoff to 5 min, 503 Retry-After, 481 to a
  refresh, 423, 481/489/500 answers, unsolicited and subscribed MWI,
  unsubscribe (also in flight), unregister ends subscriptions, a burst of
  8 BLF + MWI with NOTIFYs racing the 2xx during a call (pool + pacing),
  partial dialog-info with a version gap.
* `test_sip_event` — dialog-info: Asterisk 20 samples (captured), RFC 4235
  example, prefixes, > 8 dialogs, malformed / truncated at every byte /
  oversized / too deep; message-summary; Subscription-State; Event.
* `test_sip_fuzz` — 50k deterministic mutations through all parsers (incl.
  dialog-info, message-summary, Subscription-State, Event) plus 10k through
  the full UA and 10k NOTIFYs into live BLF / MWI subscriptions (incl. an
  over-long Contact), plus 5k requests at the size limits (Via block over the
  response template, 200 + SDP over 1500 B, re-INVITE Contact over
  `SIP_URI_MAX`) after which no transaction may be left.

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
resampled G.711 path. BLF/MWI: 1001 subscribes to 1002 and 1003 (hints in
`extensions.conf`); 1002 runs in the background and answers a `channel
originate PJSIP/1002`, so 1001 must see 1002 go ringing → busy → idle. 1001
then subscribes to its mailbox; 1003 calls `7001` (VoiceMail) and sends a tone
→ MWI yes (1 new); the message files are deleted from the spool
(`pollmailboxes`) → MWI no; unregister → no subscription left in Asterisk.
Finally 1002, without a subscription, gets the same through unsolicited
NOTIFYs (`mailboxes=` on the endpoint).
`build_host/sip/sipcli` can also be used by hand against any server.

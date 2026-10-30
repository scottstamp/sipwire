# sipwire dev PBX (Asterisk 20)

Asterisk 20 with PJSIP for testing the sipwire handset firmware. UDP SIP on
5060, digest auth, G.722 (wideband, preferred) and G.711 (PCMU/PCMA),
RFC 4733 DTMF, RTP on 10000-10100.
Dev only: passwords are `sipwire<ext>` and nothing here is hardened.

## Run locally (no root, no system install)

Needs the Ubuntu packages `asterisk`, `asterisk-core-sounds-en-gsm` and
`asterisk-moh-opsound-gsm`.

```bash
tools/asterisk/run-local.sh       # foreground, logs to the terminal
tools/asterisk/run-local.sh -i    # same, with the interactive CLI
tools/asterisk/cli.sh "core show version"
```

Runtime state (rendered config, CLI socket, logs, db) goes to
`$ASTERISK_STATE`, default `/tmp/sipwire-asterisk`. The log is
`$ASTERISK_STATE/log/messages.log`. Set the same `ASTERISK_STATE` for both
scripts if you change it. Other overrides: `ASTERISK_MODDIR`,
`ASTERISK_DATADIR`, `ASTERISK_MOHDIR`.

Stop with `tools/asterisk/cli.sh "core stop now"` or Ctrl-C.

## Run with Docker

```bash
cd tools/asterisk && docker compose up --build
```

Host networking, so no port mapping. Linux only.

## Extensions

| Ext | What it does |
|-----|--------------|
| `100X` (1000-1009) | Calls the registered endpoint `PJSIP/100X`, ring up to 30 s |
| `600` | Echo test: prompt, then your audio echoed back |
| `601` | Plays `demo-congrats` |
| `602` | 1 kHz Milliwatt tone, for audio level checks |
| `603` | DTMF test: reads up to 4 digits (RFC 4733), reads them back |
| `604` | Music on hold |
| `*97` | Voicemail menu for the caller's own mailbox (`VoiceMailMain`) |
| `*44<ext>` | Find a handset (`*441001`): rings it 60 s with `Alert-Info: <...>;info=find` |

`100X` that is not answered within 30 s (or busy) goes to voicemail `100X@default`.

## BLF and MWI

The handsets show busy lamps for the extensions in their `blf` setting and a
message-waiting envelope; this PBX provides both (mirror this on another PBX):

- **BLF**: `exten => 100X,hint,PJSIP/100X` in `[sipwire]` (the endpoint
  `context`; `allow_subscribe=yes`). An exact extension beats the `_100X`
  pattern, so each hinted one also has priority 1. The phone SUBSCRIBEs with
  `Event: dialog` to `sip:100X@<pbx>`; Asterisk answers with
  `application/dialog-info+xml` (early = ringing, confirmed = busy).
- **MWI**: `voicemail.conf` mailboxes `1001-1003@default` (PIN = extension);
  `mailboxes=100X@default` on the AOR (the phone SUBSCRIBEs to its own AOR with
  `Event: message-summary`) and on the endpoint (unsolicited NOTIFYs before it
  has; `mwi_subscribe_replaces_unsolicited=yes` stops them afterwards).
  `pollmailboxes=yes` also notices messages added or deleted on disk.
- **Modules**: `res_pjsip_pubsub`, `res_pjsip_exten_state` (needs
  `res_pjsip_outbound_publish` loaded for its symbols),
  `res_pjsip_dialog_info_body_generator`, `res_pjsip_mwi`,
  `res_pjsip_mwi_body_generator`, `app_voicemail` (+ `res_adsi`, `res_smdi`).
  `modules.conf` autoloads them; do not `noload` any of these.

Check: `cli.sh "core show hints"` (Watchers > 0 once a phone subscribed),
`cli.sh "pjsip show subscriptions inbound"`, `cli.sh "voicemail show users"`.
Leave a test message: `cli.sh "channel originate Local/1001@dial application Wait 5"`
(rings 30 s, then voicemail) or call `1001` from `1003` and let it ring.

## Find the handset, call forwarding

- **Find**: `*44<ext>` dials `PJSIP/<ext>` with a pre-dial handler (`Dial(...,b(sipwire-hdr^find^1))`)
  that adds `Alert-Info: <http://localhost>;info=find`. The handset (setting `find_alert`,
  default `find`) rings at full volume with its own pattern through do not disturb and quiet
  hours. Mirror on another PBX:

  ```
  exten => _*44XXXX,1,Dial(PJSIP/${EXTEN:3},60,b(sipwire-hdr^find^1))
  [sipwire-hdr]
  exten => find,1,Set(PJSIP_HEADER(add,Alert-Info)=<http://localhost>\;info=find)
   same => n,Return()
  ```
- **Forwarding** is done by the handset (settings `fwd_always`, `fwd_busy`, `fwd_noanswer`):
  it answers `302 Moved Temporarily` with `Contact: <sip:<target>@<pbx>>`. `Dial()` follows
  it (no `i` option; PJSIP endpoint `redirect_method` default `user`: the target's user part
  is dialled as `Local/<target>@<context of the call>`), so the target must be dialable in
  that context (here `[dial]`: `100X` and its voicemail).

## Endpoints

| User | Password | Notes |
|------|----------|-------|
| `1001` | `sipwire1001` | handset |
| `1002` | `sipwire1002` | handset |
| `1003` | `sipwire1003` | softphone for manual tests |

Each AOR allows one contact (`max_contacts=1`, `remove_existing=yes`).

## Point a handset at it

On the handset, set the SIP server to this machine's LAN IP, then log in as
user `1001` with password `sipwire1001`:

```
cfg set sip_server <host-ip>
```

## Originate a test call to a handset

With the handset registered as `1001`:

```bash
tools/asterisk/cli.sh "channel originate PJSIP/1001 extension 601@sipwire"
```

Useful checks:

```bash
tools/asterisk/cli.sh "pjsip show endpoints"
tools/asterisk/cli.sh "pjsip show contacts"
tools/asterisk/cli.sh "dialplan show sipwire"
```

## Layout

- `conf/` config templates. `run-local.sh` and the Dockerfile render the
  `@TOKENS@` in `asterisk.conf` and `musiconhold.conf`.
- `run-local.sh`, `cli.sh` helper scripts.
- `Dockerfile`, `docker-compose.yml` for the container run.

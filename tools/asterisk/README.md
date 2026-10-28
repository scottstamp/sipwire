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

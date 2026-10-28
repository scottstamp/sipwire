# sipwire development PBX: Asterisk 20 (Ubuntu 24.04 package), PJSIP only.
# Build context is tools/asterisk/. Runs as root in the foreground.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      asterisk \
      asterisk-core-sounds-en-gsm \
      asterisk-moh-opsound-gsm \
 && rm -rf /var/lib/apt/lists/*

# Our configs replace the packaged ones in /etc/asterisk.
COPY conf/ /etc/asterisk/

# Render the @TOKENS@ to the packaged system paths (same layout as run-local.sh,
# but with the real /etc, /var and /usr paths).
RUN set -eux; \
    MODDIR="$(dirname "$(find /usr/lib -name res_pjsip.so | head -n 1)")"; \
    test -f "$MODDIR/res_pjsip.so"; \
    sed -i \
      -e "s|@ETCDIR@|/etc/asterisk|g" \
      -e "s|@MODDIR@|${MODDIR}|g" \
      -e "s|@DATADIR@|/usr/share/asterisk|g" \
      -e "s|@VARLIBDIR@|/var/lib/asterisk|g" \
      -e "s|@DBDIR@|/var/lib/asterisk/db|g" \
      -e "s|@AGIDIR@|/var/lib/asterisk/agi-bin|g" \
      -e "s|@SPOOLDIR@|/var/spool/asterisk|g" \
      -e "s|@RUNDIR@|/var/run/asterisk|g" \
      -e "s|@LOGDIR@|/var/log/asterisk|g" \
      -e "s|@CACHEDIR@|/var/cache/asterisk|g" \
      -e "s|@MOHDIR@|/usr/share/asterisk/moh|g" \
      /etc/asterisk/*.conf; \
    mkdir -p /var/lib/asterisk/keys /var/lib/asterisk/db /var/lib/asterisk/agi-bin; \
    ! grep -l '@[A-Z]*DIR@' /etc/asterisk/*.conf

# SIP signalling and RTP media. With docker-compose (network_mode: host) these
# are bound directly on the host; EXPOSE is informational there.
EXPOSE 5060/udp
EXPOSE 10000-10100/udp

CMD ["asterisk", "-f", "-C", "/etc/asterisk/asterisk.conf", "-vvv"]

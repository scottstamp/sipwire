# Common prefix: boot, then the account through the console (each cfg set triggers a settings
# re-check), registered against this shard's Asterisk (${SIP_PORT}). The reply of a `cfg set`
# can have a log line of another task printed in the middle of it ("sip_port = I (..) phone:
# ..." then "5182"), so only the key is matched; the values are proven by the registration.
include _boot.inc
do cfg set sip_port ${SIP_PORT}
expect \bsip_port\s+= 15
do cfg set sip_password secret1001
expect \bsip_password\s+=
do cfg set sip_user 1001
expect \bsip_user\s+=
do cfg set sip_server 10.0.2.2
expect \bsip_server\s+=
expect registration: registered 20
do sip
expect registration: registered
shell $ASTX 'pjsip show contacts' | tee -a /dev/stderr | grep -q '1001/sip:1001@'

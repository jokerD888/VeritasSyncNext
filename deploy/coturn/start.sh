#!/bin/sh
set -eu
test -n "${TURN_SHARED_SECRET:-}" || { echo "TURN_SHARED_SECRET is required" >&2; exit 1; }
sed "s|__TURN_SHARED_SECRET__|${TURN_SHARED_SECRET}|g" /etc/coturn/turnserver.conf >/tmp/turnserver.conf
exec turnserver -c /tmp/turnserver.conf

#!/bin/sh
set -eu

echo "waiting for nginx on 127.0.0.1:80..."
i=0
while ! nc -z 127.0.0.1 80; do
  i=$((i + 1))
  if [ "$i" -gt 150 ]; then
    echo "timeout: nginx not reachable on localhost:80" >&2
    exit 1
  fi
  sleep 2
done

exec meson test -C build --verbose "${@}"

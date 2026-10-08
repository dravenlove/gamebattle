#!/bin/sh
# Starts the gamebattle release with the given command ("foreground" by default).
set -eu

if [ -z "${ERLANG_COOKIE:-}" ]; then
    echo "gamebattle: set ERLANG_COOKIE to the secret cookie shared with the game nodes" >&2
    exit 64
fi

exec /opt/gamebattle/bin/gamebattle "$@"

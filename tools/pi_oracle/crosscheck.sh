#!/bin/sh
# crosscheck.sh: the files session3.c writes (the writer and branched scenarios of
# tests/test_pi_session.c) opened by pi's own SessionManager (pi f10993bc).
# E1 / E11 automated half. PI_ORACLE names the oracle tree (see gen.ts); VTCON_DIR vtcon.
set -e
cd "$(dirname "$0")/../.."
out=$(mktemp -d "${TMPDIR:-/tmp}/picross.XXXXXX")
PI_SESSION_OUT=$out make test ONLY=pi_session >/dev/null
for f in "$out"/*.jsonl "$@"; do
    [ -f "$f" ] || continue
    d=$(node tools/pi_oracle/gen.ts --dump "$f" 2>/dev/null) || { echo "[ERROR] pi refused $f"; exit 1; }
    echo "[OK] $(basename "$f"): pi opened it, $(echo "$d" | sed -n 's/^entries /entries /p')"
done
rm -rf "$out"

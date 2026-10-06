#!/usr/bin/env bash
# The emulator's protocol fault rows against a locally built server, over the phone relay.
#
#   scripts/relay-interop.sh [--server <dir>] [--no-legacy] [--selftest] [--keep]
#
# Issue #12: the emulator's fault matrix used to talk only to the legacy device listener
# (/api/v2/...), the path being retired (front door #7). The real path is dongle -> BLE ->
# phone -> /v1/relay/bundles/*. This builds the server from a checkout into a temporary
# directory (the checkout is only READ; nothing in it changes), enrols the emulator as a phone
# with a one-time invitation, and runs `fault-matrix --relay`, which runs the shared protocol rows
# (torn uploads, replays, a corrupted chunk, a lost receipt) AND the rows only the relay has
# (reordering, early commit, forged receipts and offers, caller forgery, request replay).
#
#   --server <dir>   the server checkout (default: $CAIRN_SERVER_DIR, then .interop/server,
#                    then ../cairn-vehicle-server)
#   --no-legacy      relay rows only (the default also runs the legacy rows beside them, until
#                    front door #7 removes that listener)
#   --selftest       afterwards prove the run can fail: a wrong pinned receipt key, then a revoked phone
#   --keep           keep the temporary directory (data, logs, emulator work dir) and print it
#
# The server's own required interop job gets the same by calling this script (server-side hook,
# not made here):
#     "$FW/scripts/relay-interop.sh" --server "$ROOT"
#
# Needs: Go, Rust (cargo), curl, git. Nothing is installed or published.
set -euo pipefail

cd "$(dirname "$0")/.."
FW="$PWD"

server="${CAIRN_SERVER_DIR:-}"
legacy=1
keep=0
selftest=0
while [ $# -gt 0 ]; do
  case "$1" in
    --server) server="${2:?--server needs a directory}"; shift ;;
    --no-legacy) legacy=0 ;;
    --keep) keep=1 ;;
    --selftest) selftest=1 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done
if [ -z "$server" ]; then
  for d in "$FW/.interop/server" "$FW/../cairn-vehicle-server"; do
    [ -f "$d/cmd/cairn-server/main.go" ] && { server="$d"; break; }
  done
fi
[ -n "$server" ] && [ -f "$server/cmd/cairn-server/main.go" ] || {
  echo "no server checkout found; pass --server <dir> or set CAIRN_SERVER_DIR" >&2
  exit 2
}
server="$(cd "$server" && pwd)"

WORK="$(mktemp -d)"
SERVER_PID=""
cleanup() {
  [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  if [ "$keep" = 1 ]; then echo "kept: $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

echo "==> building the server from $server (into a temporary directory)"
( cd "$server" && go build -o "$WORK/cairn-server" ./cmd/cairn-server \
                && go build -o "$WORK/cairn-admin" ./cmd/cairn-admin )

echo "==> building the emulator"
( cd "$FW/emulator" && cargo build --release -q )
EMU="$FW/emulator/target/release/cairn-emulator"

# The emulator's PUBLIC test values (its default storage root and a fixed pair of ids); they
# protect nothing. Same as the server's tests/interop/run.sh, so both exercise one identity.
DATA="$WORK/data"; mkdir -p "$DATA"
ROOT_HEX=636169726e2d656d756c61746f722d7075626c69632d746573742d726f6f7421
VEHICLE=606162636465666768696a6b6c6d6e6f
ASSIGNMENT=707172737475767778797a7b7c7d7e7f
MASTER="$WORK/keystore.master"
PORT="${INTEROP_PORT:-18700}"
APP_PORT="${INTEROP_APP_PORT:-18701}"

"$WORK/cairn-server" -data "$DATA" -keystore-master "$MASTER" \
  -enroll 101112131415161718191a1b1c1d1e1f \
  -enroll-key 9f8d4bbc0bf6feeabbabe4d2aff165db4f77fc4b6d46619818af409ddab507bf \
  -enroll-root "$ROOT_HEX" \
  -enroll-name interop-device
"$WORK/cairn-admin" -data "$DATA" vehicle add --id "$VEHICLE" --name "interop car"
"$WORK/cairn-admin" -data "$DATA" assign --assignment-id "$ASSIGNMENT" \
  101112131415161718191a1b1c1d1e1f "$VEHICLE"

# The receipt key a dongle pins at provisioning. The relay has no endpoint for it; this is
# the operator's route (the key is persistent: an ephemeral one would invalidate receipts).
RECEIPT_KEY="$("$WORK/cairn-server" -data "$DATA" -keystore-master "$MASTER" \
  -receipt-key "$DATA/receipt.seed" -print-receipt-key 2>/dev/null)"
[ "${#RECEIPT_KEY}" = 64 ] || { echo "could not read the receipt key (got '$RECEIPT_KEY')" >&2; exit 1; }

# The phone's invitation, scoped to the one vehicle the device is assigned to.
INVITE_OUT="$("$WORK/cairn-admin" -data "$DATA" client invite --role user --vehicles "$VEHICLE" \
  --name "emulator phone")"
INVITE="$(printf '%s\n' "$INVITE_OUT" | tr -d ' ' | grep -E '^[0-9a-f]{4}(-[0-9a-f]{4}){7}$|^[0-9a-f]{32}$' | head -1)"
[ -n "$INVITE" ] || { echo "could not read the invitation code from:" >&2; echo "$INVITE_OUT" >&2; exit 1; }

"$WORK/cairn-server" -data "$DATA" -addr "127.0.0.1:$PORT" -dev \
  -keystore-master "$MASTER" -receipt-key "$DATA/receipt.seed" \
  -app-addr "127.0.0.1:$APP_PORT" >"$WORK/server.log" 2>&1 &
SERVER_PID=$!
# Both listeners must answer, and the server process must still be alive. Probing only the
# legacy port let a server that failed to bind the app port (another server already listening
# there) pass this check through the OTHER server, then fail later with a bare "connection
# refused" from the emulator and nothing pointing at the cause.
up=0
for _ in $(seq 1 50); do
  kill -0 "$SERVER_PID" 2>/dev/null || break
  if curl -sf "http://127.0.0.1:$PORT/api/v2/health" >/dev/null &&
     curl -sf "http://127.0.0.1:$APP_PORT/v1/health" >/dev/null; then up=1; break; fi
  sleep 0.2
done
if [ "$up" != 1 ]; then
  echo "the server did not come up on both ports ($PORT legacy, $APP_PORT app)." >&2
  kill -0 "$SERVER_PID" 2>/dev/null || echo "its process exited; if another server holds one of the ports, set INTEROP_PORT and INTEROP_APP_PORT." >&2
  tail -20 "$WORK/server.log" >&2
  exit 1
fi

args=(fault-matrix --work-dir "$WORK/fault-matrix-relay"
  --root-key "$ROOT_HEX" --vehicle-id "$VEHICLE" --assignment-id "$ASSIGNMENT"
  --relay "http://127.0.0.1:$APP_PORT" --invite-code "$INVITE" --receipt-key "$RECEIPT_KEY"
  --verbose)
[ "$legacy" = 1 ] && args+=(--server "http://127.0.0.1:$PORT")

echo "==> emulator as the phone: protocol rows over /v1/relay/bundles/*"
"$EMU" "${args[@]}"

if [ "$selftest" = 1 ]; then
  # The run above passing proves little unless it can fail. Two ways it must:
  echo "==> selftest 1: a dongle that pinned the WRONG receipt key must reject every receipt"
  wrong=5866666666666666666666666666666666666666666666666666666666666666  # a valid Ed25519 key (the base point), not the server's
  if "$EMU" fault-matrix --work-dir "$WORK/fault-matrix-relay" --root-key "$ROOT_HEX" \
       --vehicle-id "$VEHICLE" --assignment-id "$ASSIGNMENT" --relay "http://127.0.0.1:$APP_PORT" \
       --receipt-key "$wrong" \
       >"$WORK/wrongkey.out" 2>&1; then
    echo "the matrix PASSED with a wrong receipt key; the relay rows do not check receipts" >&2; exit 1
  fi
  grep -A2 -m2 '^  FAIL  relay/' "$WORK/wrongkey.out" | cut -c1-200 | sed 's/^/    /'
  echo "    rejected, as it must"

  echo "==> selftest 2: a revoked phone must be refused on every relay request"
  cid="$(sed -n 's/.*"client_id": *"\([0-9a-f]*\)".*/\1/p' "$WORK/fault-matrix-relay/relay-phone.json")"
  "$WORK/cairn-admin" -data "$DATA" client revoke "$cid" >/dev/null
  if "$EMU" fault-matrix --work-dir "$WORK/fault-matrix-relay" --root-key "$ROOT_HEX" \
       --vehicle-id "$VEHICLE" --assignment-id "$ASSIGNMENT" --relay "http://127.0.0.1:$APP_PORT" \
       --receipt-key "$RECEIPT_KEY" \
       >"$WORK/revoked.out" 2>&1; then
    echo "the matrix PASSED with a revoked phone; the relay is not authenticating the caller" >&2; exit 1
  fi
  grep -A2 -m2 '^  FAIL  relay/' "$WORK/revoked.out" | cut -c1-200 | sed 's/^/    /'
  echo "    refused, as it must"
fi

echo "==> relay interop passed"

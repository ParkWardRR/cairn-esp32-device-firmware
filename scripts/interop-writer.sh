#!/usr/bin/env bash
# Does the server's reader accept what THIS firmware's C code writes?
#
#   scripts/interop-writer.sh                  seal bundles with the C writer, judge them with
#                                              the server's cairn-verify; exit 0 only if accepted
#   scripts/interop-writer.sh --mutation-check as above, then prove the check has teeth: apply each
#                                              mutation below to a COPY of the C sources and require
#                                              cairn-verify to reject what the mutated writer seals
#   scripts/interop-writer.sh --server <dir>   the server checkout (default: $CAIRN_SERVER_DIR,
#                                              then .interop/server, then ../cairn-vehicle-server)
#
# Why this exists (issue #8): the server's interop job exercises the offload protocol and the
# emulator, but nothing handed it bytes the C format code had written, so a wrong CRC polynomial
# or frame layout in lib/cairn_format passed interop and was caught only by this repository's own
# conformance run. The writer (test/host/interop_writer.c) never checks its own output; the Go
# verifier is the independent judge.
#
# The server repository is only READ and built into a temporary directory; nothing in it changes.
# Needs: a C compiler, make, Go. The firmware tree is not modified either: mutations are applied
# to a copy in a temporary directory.
set -euo pipefail

cd "$(dirname "$0")/.."
FW="$PWD"

mutation_check=0
server="${CAIRN_SERVER_DIR:-}"
while [ $# -gt 0 ]; do
  case "$1" in
    --mutation-check) mutation_check=1 ;;
    --server) server="${2:?--server needs a directory}"; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

if [ -z "$server" ]; then
  for d in "$FW/.interop/server" "$FW/../cairn-vehicle-server"; do
    [ -f "$d/cmd/cairn-verify/main.go" ] && { server="$d"; break; }
  done
fi
[ -n "$server" ] && [ -f "$server/cmd/cairn-verify/main.go" ] || {
  echo "no server checkout found; pass --server <dir> or set CAIRN_SERVER_DIR" >&2
  echo "(it needs cmd/cairn-verify; e.g. git clone https://github.com/ParkWardRR/cairn-vehicle-server)" >&2
  exit 2
}
server="$(cd "$server" && pwd)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> building the server's verifier from $server"
( cd "$server" && go build -o "$WORK/cairn-verify" ./cmd/cairn-verify )

# judge <label> <source-root> : build the C writer from <source-root>/test/host, run it, run the
# verifier over what it wrote. Sets VERDICT_RC (the verifier's exit code, or "writer-failed")
# and leaves the verifier's text in $WORK/<label>.verify.
judge() {
  local label="$1" root="$2" out="$WORK/$1-out"
  VERDICT_RC=""
  if ! make -s -C "$root/test/host" BUILD="$WORK/$label-build" INTEROP_OUT="$out" interop-writer \
        >"$WORK/$label.writer" 2>&1; then
    VERDICT_RC="writer-failed"; return 0
  fi
  # shellcheck disable=SC1091
  . "$out/interop.env"
  set +e
  "$WORK/cairn-verify" -device-key "$DEVICE_KEY" -root-key "$ROOT_KEY" -key-version "$KEY_VERSION" \
    "$out/card" >"$WORK/$label.verify" 2>&1
  VERDICT_RC=$?
  set -e
}

echo "==> the C writer, unmodified"
judge baseline "$FW"
tail -1 "$WORK/baseline.writer"
if [ "$VERDICT_RC" != "0" ]; then
  echo "the server's verifier REJECTED the firmware's own output (rc=$VERDICT_RC):" >&2
  [ "$VERDICT_RC" = "writer-failed" ] && cat "$WORK/baseline.writer" >&2 || cat "$WORK/baseline.verify" >&2
  exit 1
fi
tail -1 "$WORK/baseline.verify"
echo "    accepted"

[ "$mutation_check" = 1 ] || { echo "==> interop-writer passed"; exit 0; }

# ─── mutations ────────────────────────────────────────────────────────────────
# name | file (under lib/) | sed script. Each changes ONE thing in the WRITER's encoding. The
# C reader inside the writer is not consulted, so a mutation the C side is self-consistent about
# (which is exactly the case this check is for) is still seen by the Go reader.
MUTATIONS=(
  "frame-header-layout|cairn_format/cf_frame.c|s/put_u32(out + 8, seq)/put_u32(out + 12, seq)/;s/put_u32(out + 12, monotonic_ms)/put_u32(out + 8, monotonic_ms)/"
  "crc32-polynomial|cairn_format/cf_hash.c|s/0xEDB88320u/0xEDB88321u/"
  "merkle-internal-domain|cairn_format/cf_hash.c|s/#define DOMAIN_INTERNAL 0x01/#define DOMAIN_INTERNAL 0x03/"
  "segment-magic|cairn_format/cf_frame.c|s/{ 'C', 'R', 'N', '3' }/{ 'C', 'R', 'N', '4' }/"
)

echo "==> mutation check: each of these must be REJECTED by the server's verifier"
bad=0
for m in "${MUTATIONS[@]}"; do
  IFS='|' read -r name file expr <<<"$m"
  tree="$WORK/tree-$name"
  mkdir -p "$tree/test"
  cp -R lib include src "$tree/"
  cp -R test/host "$tree/test/host"
  rm -rf "$tree/test/host/build"
  sed "$expr" "lib/$file" >"$tree/lib/$file"
  if cmp -s "lib/$file" "$tree/lib/$file"; then
    echo "  ERROR  $name: the mutation did not change lib/$file (the source moved; update the table)" >&2
    bad=1; continue
  fi
  judge "$name" "$tree"
  case "$VERDICT_RC" in
    1)
      echo "  rejected  $name   (cairn-verify exit 1)"
      grep -m2 -E '^ +FAIL ' "$WORK/$name.verify" | cut -c1-170 | sed 's/^/              /' || true
      ;;
    0)
      echo "  ACCEPTED  $name   <-- the mutated writer's output passed the server's verifier;" >&2
      echo "                       this interop check does NOT cover that part of the format" >&2
      bad=1 ;;
    writer-failed)
      echo "  ERROR  $name: the mutated writer did not run, so this proves nothing:" >&2
      sed 's/^/    /' "$WORK/$name.writer" >&2; bad=1 ;;
    *)
      echo "  ERROR  $name: cairn-verify exited $VERDICT_RC (a usage/IO error, not a verdict)" >&2
      sed 's/^/    /' "$WORK/$name.verify" >&2; bad=1 ;;
  esac
done

[ "$bad" = 0 ] || { echo "==> mutation check FAILED" >&2; exit 1; }
echo "==> interop-writer passed (baseline accepted, ${#MUTATIONS[@]} mutations rejected)"

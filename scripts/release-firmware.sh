#!/usr/bin/env bash
# Build a channel image for the dongle, and optionally flash it.
#
#   scripts/release-firmware.sh <beta|stable> [--flash [port]] [--allow-dirty]
#
# --flash uploads to the dongle over the cable. With no port it takes the only
# /dev/cu.usbserial* it finds and refuses if there is more than one, because flashing the
# wrong device is not recoverable by rerunning the command.
# --allow-dirty builds a channel image from an uncommitted tree. The image records itself as
# dirty either way; this only skips the refusal.
#
# What it does, stopping at the first failure:
#   1. refuse a channel that is not beta or stable, and refuse a dirty tree
#   2. fetch the pinned contracts and run the host conformance suites (make host-test,
#      make engines-check)
#   3. build env:cairn with CAIRN_CHANNEL set, which stamps the channel, the commit and the
#      dirty flag into the image (scripts/build_identity.py)
#   4. with --flash: back up NVS first, then upload
#
# NVS holds the device's storage root and signing seed. It is backed up before a flash even
# though a normal upload does not write that region: there is one dongle and no spare, and a
# lost root means every sealed bundle on the card is unreadable.
#
# OTA is NOT how a release reaches the dongle yet. The install path is written and
# host-verified but has never run on hardware (firmware #11), so a channel image goes over
# the cable until that gate is met.
set -euo pipefail

CHANNEL="${1:?usage: release-firmware.sh <beta|stable> [--flash [port]] [--allow-dirty]}"
shift
FLASH=0; PORT=""; ALLOW_DIRTY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --flash)
      FLASH=1
      case "${2:-}" in
        /dev/*) PORT="$2"; shift ;;
      esac
      ;;
    --allow-dirty) ALLOW_DIRTY=1 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
case "$CHANNEL" in
  beta|stable) ;;
  dev) echo "dev is not a channel you release: build with plain 'pio run -e cairn'" >&2; exit 2 ;;
  *) echo "channel must be beta or stable, not '$CHANNEL'" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ -n "$(git status --porcelain)" ]; then
  if [ "$ALLOW_DIRTY" = 1 ]; then
    echo "!!  the tree is dirty; the image will record itself as dirty"
  else
    echo "Refusing a $CHANNEL build from a dirty tree: the image would name a commit that is not its source." >&2
    git status --short >&2
    exit 1
  fi
fi

echo "==> $CHANNEL image from $(git rev-parse --short HEAD)"

echo "==> contracts"
./scripts/fetch-contracts.sh >/dev/null

echo "==> host conformance (make host-test, make engines-check)"
make host-test >/dev/null
make engines-check >/dev/null

echo "==> building env:cairn"
CAIRN_CHANNEL="$CHANNEL" pio run -e cairn -j 2 2>&1 | grep -E "cairn build identity|RAM:|Flash:|SUCCESS|FAILED" || true

if [ "$FLASH" = 0 ]; then
  echo "==> built, not flashed (pass --flash to upload)"
  exit 0
fi

if [ -z "$PORT" ]; then
  # Not mapfile: /bin/bash on macOS is 3.2 and does not have it.
  set -- /dev/cu.usbserial*
  if [ ! -e "$1" ]; then
    echo "no /dev/cu.usbserial* found: is the dongle plugged in?" >&2; exit 1
  elif [ $# -gt 1 ]; then
    echo "more than one serial device; name the port: $*" >&2; exit 1
  fi
  PORT="$1"
fi
echo "==> flashing $PORT"

BACKUPS="$HOME/cairn-backups"
mkdir -p "$BACKUPS"; chmod 700 "$BACKUPS"
NVS="$BACKUPS/nvs-$(date +%Y%m%d-%H%M%S).bin"
ESPTOOL="$(ls -d "$HOME"/.platformio/packages/tool-esptoolpy*/esptool.py 2>/dev/null | head -1 || true)"
if [ -n "$ESPTOOL" ]; then
  echo "==> backing up NVS to $NVS"
  python3 "$ESPTOOL" --port "$PORT" --no-stub read_flash 0x9000 0x5000 "$NVS" >/dev/null
  chmod 600 "$NVS"
else
  echo "could not find esptool.py under ~/.platformio/packages; refusing to flash without an NVS backup" >&2
  exit 1
fi

CAIRN_CHANNEL="$CHANNEL" pio run -e cairn -t upload --upload-port "$PORT"
echo
echo "==> flashed: $CHANNEL, $(git rev-parse --short HEAD)"
echo "    Read the boot banner back to confirm what is running:"
echo "      pio device monitor -p $PORT -b 115200"

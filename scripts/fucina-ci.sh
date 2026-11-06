#!/usr/bin/env bash
# Run this repository's builds and tests on fucina (the mandated build host), in
# rootless podman containers, syncing the working tree first. The dongle is flashed
# from the Mac; only build/test work moves here.
#
#   scripts/fucina-ci.sh                 host tests + engine checks + emulator + firmware image
#   scripts/fucina-ci.sh host           just test/host
#   scripts/fucina-ci.sh emu            just the emulator (cargo test + conformance)
#   scripts/fucina-ci.sh fw             just the PlatformIO image (make firmware)
#   scripts/fucina-ci.sh engines        just make engines-check
#
# The tree lands in ~/builds/cairn-firmware on fucina. Caches live in
# ~/builds/.cache (cargo-registry, pio). The synced .contracts/ satisfies
# fetch-contracts.sh without network.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST=alfa@fucina.alpina.casa
DEST='~/builds/cairn-firmware'
RSYNC_EXCLUDES=(--exclude .pio --exclude build/ --exclude '*.o' --exclude test/host/bin
                --exclude tools/enginegen/target --exclude emulator/target)

stages=("$@"); [ ${#stages[@]} -gt 0 ] || stages=(host engines emu fw)

echo "== sync -> fucina"
rsync -a --delete "${RSYNC_EXCLUDES[@]}" ./ "$HOST:$DEST/"

run() { ssh "$HOST" "$@"; }

# :z — fucina enforces SELinux, and a fresh rsync lands files labelled
# user_home_t, which a container may not read; the shared relabel fixes every
# stage the same way.
GCC='podman run --rm -v ~/builds/cairn-firmware:/w:z -w /w docker.io/library/gcc:13-bookworm'
RUST='podman run --rm -v ~/builds/cairn-firmware:/w:z -v ~/builds/.cache/cargo-registry:/usr/local/cargo/registry:z -w /w docker.io/library/rust:1.90'
PIO='podman run --rm -v ~/builds/cairn-firmware:/w:z -v ~/builds/.cache/pio:/root/.platformio:z -w /w localhost/cairn-pio:1'

for s in "${stages[@]}"; do
  case "$s" in
    host)
      echo "== host tests"
      run "$GCC make -C test/host -j6" ;;
    engines)
      echo "== engines-check"
      run "$RUST bash -c 'make engines-check JOBS=6'" ;;
    emu)
      echo "== emulator"
      run "$RUST bash -c 'cd emulator && cargo test --workspace --quiet && cargo run --release --quiet -- conformance && cargo run --release --quiet -- fault-matrix --work-dir target/fault-matrix'" ;;
    fw)
      echo "== firmware image (env=\${ENV:-cairn})"
      run "$PIO bash -c 'make firmware ENGINES=all JOBS=6 ENV=${ENV:-cairn}'" ;;
    *) echo "unknown stage: $s" >&2; exit 2 ;;
  esac
done
echo "== fucina ci: all stages passed"

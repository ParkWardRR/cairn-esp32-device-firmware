# Top-level build entry points.
#
#   make firmware ENGINES=all                    build the capture image with every engine in engines/
#   make firmware ENGINES=bmw-n20                ... or only the ones named (comma separated)
#   make firmware ENGINES=bmw-n20,bmw-b58 ENV=cairn-selftest
#
#   make engines-gen        regenerate the committed all-engines header (after editing engines/)
#   make engines-check      what CI runs: validate, vectors, committed header up to date, tests
#   make engines-test       the generator's own tests
#   make host-test          the firmware's host suites (test/host), which include the engine tests
#
# The logic is in tools/enginegen (Rust), not in a PlatformIO extra_script: this Makefile
# generates the tables for the selection into build/engines/ and tells PlatformIO where
# to find them through PLATFORMIO_BUILD_FLAGS, so a plain `pio run -e cairn` still works
# and uses the committed all-engines tables.
#
# An unknown or invalid engine, or an invalid profile file, stops here, before PlatformIO
# is started.

ENGINES    ?= all
ENV        ?= cairn
JOBS       ?= 2
ENGINES_DIR := engines
GEN_DIR     := build/engines
GEN_SEL     := $(GEN_DIR)/cairn_engines_sel.h
GEN_COMMIT  := lib/cairn_engine/gen/cairn_engines_gen.h
ENGINEGEN   := tools/enginegen/target/release/enginegen

# The engine profile schema, its normative text and its vectors live in contracts/engine/v1
# (pinned by contracts.lock), not in this repository: two copies of one schema is the drift
# engine/v1 was created to end. Resolution order is the documented one -- CAIRN_CONTRACTS
# wins, otherwise the fetched copy.
CONTRACTS   ?= $(if $(CAIRN_CONTRACTS),$(CAIRN_CONTRACTS),.contracts/contracts)
ENGINE_SPEC := $(CONTRACTS)/engine/v1
VECTORS     := $(ENGINE_SPEC)/vectors/expr.txt

.PHONY: help firmware engines-gen engines-check engines-test engines-validate host-test enginegen clean-engines contracts

help:
	@sed -n '3,12p' Makefile

# Always run cargo: it is a no-op when nothing changed, and it makes a stale binary
# impossible.
enginegen:
	cargo build --release --locked --quiet --manifest-path tools/enginegen/Cargo.toml

# A no-op when .contracts is already at the pinned commit, so this is cheap to depend on.
contracts:
	scripts/fetch-contracts.sh

engines-validate: enginegen contracts
	$(ENGINEGEN) validate --dir $(ENGINES_DIR)
	$(ENGINEGEN) vectors --file $(VECTORS)

# The tables for ENGINES, then the image. PLATFORMIO_BUILD_FLAGS is appended to, not
# replaced: CI sets it for the OTA build.
# Exported through make, not interpolated into a shell string, so a flag with quotes in it
# (-DCAIRN_VEHICLE_ENGINE_ID='"bmw-b58"') reaches PlatformIO unchanged.
firmware: export PLATFORMIO_BUILD_FLAGS := $(PLATFORMIO_BUILD_FLAGS) -I$(abspath $(GEN_DIR))
firmware: enginegen
	$(ENGINEGEN) gen --dir $(ENGINES_DIR) --engines '$(ENGINES)' --out $(GEN_SEL)
	pio run -e $(ENV) -j $(JOBS)

engines-gen: enginegen
	$(ENGINEGEN) gen --dir $(ENGINES_DIR) --engines all --out $(GEN_COMMIT)

engines-check: enginegen contracts
	$(ENGINEGEN) validate --dir $(ENGINES_DIR)
	$(ENGINEGEN) vectors --file $(VECTORS)
	mkdir -p $(GEN_DIR)
	$(ENGINEGEN) gen --dir $(ENGINES_DIR) --engines all --out $(GEN_DIR)/check_all.h
	@diff -u $(GEN_COMMIT) $(GEN_DIR)/check_all.h \
	  || { echo "error: $(GEN_COMMIT) is stale; run 'make engines-gen' and commit it" >&2; exit 1; }
	cargo test --locked --quiet --manifest-path tools/enginegen/Cargo.toml
	@# The firmware's own tests against each selection the build can produce: one engine
	@# (which must refuse the other), the other, and both.
	@set -e; for sel in bmw-n20 bmw-b58 bmw-b58,bmw-n20; do \
	  d=$(GEN_DIR)/check-$$(echo $$sel | tr , _); \
	  $(ENGINEGEN) gen --dir $(ENGINES_DIR) --engines $$sel --out $$d/cairn_engines_sel.h > /dev/null; \
	  $(MAKE) --no-print-directory -C test/host engine-sel ENGINE_GEN_DIR=$(CURDIR)/$$d ENGINE_EXPECT=$$sel; \
	done
	@# An unknown engine, or none, must stop the build.
	@! $(ENGINEGEN) gen --dir $(ENGINES_DIR) --engines bmw-nope --out $(GEN_DIR)/never.h 2> /dev/null \
	  || { echo "error: an unknown engine was accepted" >&2; exit 1; }
	@! $(ENGINEGEN) gen --dir $(ENGINES_DIR) --engines '' --out $(GEN_DIR)/never.h 2> /dev/null \
	  || { echo "error: an empty selection was accepted" >&2; exit 1; }

engines-test: contracts
	cargo test --locked --manifest-path tools/enginegen/Cargo.toml

host-test:
	$(MAKE) -C test/host

clean-engines:
	rm -rf $(GEN_DIR)

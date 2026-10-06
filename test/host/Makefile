# Host builds of the firmware's portable code.
#
# Two suites, and the distinction matters:
#
#   conformance  compiles lib/cairn_format against the committed vectors, so
#                this implementation is shown to agree byte-for-byte with the
#                Go and Rust ones.
#   faults       compiles lib/cairn_store and lib/cairn_prune over a POSIX
#                filesystem, then tears files and forges receipts. These are
#                properties about crash and adversarial behaviour; compiling
#                for ESP32 does not test them and neither does a good drive.
#
# Both compile exactly the sources the device runs — no substitutions — reached
# through cairn_fs rather than reimplemented.
#
#   make           build and run both
#   make asan      the same, under AddressSanitizer and UBSan
#   make clean

CC       ?= cc

# The Cairn contracts (specs and vectors) are not part of this project's code: they are
# the shared protocols. CAIRN_CONTRACTS is the contracts directory itself (the one that
# contains format/, sync/, ble/). Unset, it is found in the monorepo checkout or, once
# this project is its own repository, in the fetched copy at .contracts/contracts.
CAIRN_CONTRACTS ?= $(patsubst %/format,%,$(firstword $(wildcard ../../../../contracts/format ../../.contracts/contracts/format) ../../.contracts/contracts/format))
FIXTURES        ?= $(CAIRN_CONTRACTS)/format/v3/vectors
ENROLL_VECTORS  ?= $(CAIRN_CONTRACTS)/enrolment/v1/vectors/vectors.json

FORMAT_DIR := ../../lib/cairn_format
STORE_DIR  := ../../lib/cairn_store
FS_DIR     := ../../lib/cairn_fs
INCLUDE_DIR := ../../include
LOG_DIR     := ../../lib/cairn_log
PROV_DIR    := ../../lib/cairn_prov
OFFLOAD_DIR := ../../lib/cairn_offload
BOOTTIME_DIR := ../../lib/cairn_boottime
UPLINK_DIR  := ../../lib/cairn_uplink
CONFIG_DIR  := ../../lib/cairn_config
DEVINFO_DIR := ../../lib/cairn_devinfo
CHECKIN_DIR := ../../lib/cairn_checkin

FORMAT_SRC := $(wildcard $(FORMAT_DIR)/*.c)
STORE_SRC  := $(wildcard $(STORE_DIR)/*.c)
FS_SRC     := $(wildcard $(FS_DIR)/*.c)

# The OTA preconditions and version ordering are portable C; the install path
# behind CAIRN_OTA_AVAILABLE is Arduino and is not built here.
OTA_DIR    := ../../lib/cairn_ota

# Likewise the standby decisions; the execution half is in src/ and Arduino.
POWER_DIR  := ../../lib/cairn_power

# Only the portable application sources; the rest of src/ is Arduino C++.
SRC_DIR    := ../../src
# policy.c and cairn_power.c take their engine-dependent values from the profile, so every
# binary that links them links the engine module too.
ENGINE_DIR := ../../lib/cairn_engine
ENGINE_LIB_SRC := $(ENGINE_DIR)/cairn_engine.c $(ENGINE_DIR)/cairn_expr.c
APP_SRC    := $(SRC_DIR)/preroll.c $(SRC_DIR)/policy.c $(OTA_DIR)/cairn_ota.c $(POWER_DIR)/cairn_power.c $(ENGINE_LIB_SRC)

# The conformance runner needs the policy codec, which lives in src/.
BLE_VECTORS     ?= $(CAIRN_CONTRACTS)/ble/v1/vectors/golden/golden-vectors.json
CONFORMANCE_SRC := $(FORMAT_SRC) $(SRC_DIR)/policy.c minijson.c conformance.c
FAULTS_SRC      := $(FORMAT_SRC) $(STORE_SRC) $(FS_SRC) $(APP_SRC) \
                   log_host.c platform_host.c faults.c
BLE_VEC_SRC     := $(FORMAT_SRC) minijson.c ble_vectors.c
# Provisioning needs only the format primitives, its own sources, and the host
# key-value store (so the credential slots run against the real slot code).
PROV_SRC        := $(FORMAT_SRC) $(wildcard $(PROV_DIR)/*.c) $(FS_DIR)/cairn_kv_posix.c minijson.c prov_test.c

# The BLE offload protocol: the module, over the real store and prune code, in
# front of a simulated BLE stack and phone.
OFFLOAD_SRC     := $(FORMAT_SRC) $(STORE_SRC) $(FS_SRC) $(APP_SRC) $(OFFLOAD_DIR)/cairn_offload.c \
                   log_host.c platform_host.c offload_test.c

# A dongle on a pipe, for the Go end-to-end test (server/internal/offloadclient).
OFFLOAD_SIM_SRC := $(FORMAT_SRC) $(STORE_SRC) $(FS_SRC) $(APP_SRC) $(OFFLOAD_DIR)/cairn_offload.c \
                   log_host.c platform_host.c offload_sim.c

# -Wconversion is deliberately absent but the rest is strict: this code does a
# great deal of width narrowing between the wire format and C types, and a
# silent truncation there corrupts a bundle rather than crashing.
# -D_DEFAULT_SOURCE: under strict -std=c11 glibc hides popen and friends, which the
# conformance runner uses; macOS exposes them regardless, so this only ever failed on
# the Linux CI runner. -Wno-format-truncation: gcc cannot prove a 512-byte path buffer
# is big enough for "<dir>/segment-N.seg" and -Werror turned that into a failure;
# these are fixed-size buffers in test code that a truncation would fail loudly, and
# clang (which the Mac uses) has no such check. The firmware proper is built by
# PlatformIO with its own flags and is unaffected.
CFLAGS  ?= -std=c11 -O1 -g -D_DEFAULT_SOURCE \
           -Wall -Wextra -Werror -Wno-format-truncation \
           -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes \
           -Wmissing-prototypes -Wno-unused-parameter \
           -I$(FORMAT_DIR) -I$(STORE_DIR) -I$(FS_DIR) -I$(LOG_DIR) -I$(PROV_DIR) -I$(INCLUDE_DIR) -I$(SRC_DIR) -I$(OTA_DIR) -I$(POWER_DIR) -I$(OFFLOAD_DIR) -I$(BOOTTIME_DIR) -I$(UPLINK_DIR) -I$(CONFIG_DIR) -I$(DEVINFO_DIR) -I$(CHECKIN_DIR) -I.

# Every header the sources can include. Without this a change to a shared header
# (config.h, cairn_platform.h, policy.h) leaves a stale binary in build/ and a
# result that describes yesterday's code — which already produced one false
# failure and could as easily produce a false pass.
HEADERS := $(wildcard $(OFFLOAD_DIR)/*.h $(PROV_DIR)/*.h $(FORMAT_DIR)/*.h $(STORE_DIR)/*.h $(FS_DIR)/*.h $(LOG_DIR)/*.h \
                      $(INCLUDE_DIR)/*.h $(SRC_DIR)/*.h $(OTA_DIR)/*.h $(POWER_DIR)/*.h $(BOOTTIME_DIR)/*.h $(UPLINK_DIR)/*.h $(CONFIG_DIR)/*.h $(DEVINFO_DIR)/*.h $(CHECKIN_DIR)/*.h *.h)

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer

BUILD := build

# Engine profiles (lib/cairn_engine): see engine.mk.
include engine.mk

.PHONY: all run asan clean conformance faults ble-vectors mtprobe prov offload offload-sim boottime uplink config devinfo

all: run

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/conformance: $(CONFORMANCE_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(CONFORMANCE_SRC) -o $@

$(BUILD)/faults: $(FAULTS_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(FAULTS_SRC) -o $@

$(BUILD)/ble-vectors: $(BLE_VEC_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(BLE_VEC_SRC) -lm -o $@

conformance: $(BUILD)/conformance
	$(BUILD)/conformance $(FIXTURES)

faults: $(BUILD)/faults
	$(BUILD)/faults

ble-vectors: $(BUILD)/ble-vectors
	$(BUILD)/ble-vectors $(BLE_VECTORS)

$(BUILD)/prov: $(PROV_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(PROV_SRC) -o $@

prov: $(BUILD)/prov
	CAIRN_ENROLL_VECTORS=$(ENROLL_VECTORS) $(BUILD)/prov

$(BUILD)/offload: $(OFFLOAD_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(OFFLOAD_SRC) -o $@

offload: $(BUILD)/offload
	$(BUILD)/offload

$(BUILD)/offload-sim: $(OFFLOAD_SIM_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(OFFLOAD_SIM_SRC) -o $@

offload-sim: $(BUILD)/offload-sim

$(BUILD)/mtprobe: $(SRC_DIR)/mtprobe_parse.c $(HEADERS) mtprobe_parse_test.c | $(BUILD)
	$(CC) $(CFLAGS) $(SRC_DIR)/mtprobe_parse.c mtprobe_parse_test.c -o $@

mtprobe: $(BUILD)/mtprobe
	$(BUILD)/mtprobe

$(BUILD)/boottime: $(BOOTTIME_DIR)/cairn_boottime.c boottime_test.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(BOOTTIME_DIR)/cairn_boottime.c boottime_test.c -o $@

boottime: $(BUILD)/boottime
	$(BUILD)/boottime

$(BUILD)/uplink: $(UPLINK_DIR)/cairn_uplink.c uplink_test.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(UPLINK_DIR)/cairn_uplink.c uplink_test.c -o $@

uplink: $(BUILD)/uplink
	$(BUILD)/uplink

CONFIG_SRC := $(FORMAT_SRC) $(FS_DIR)/cairn_kv_posix.c $(CONFIG_DIR)/cairn_config.c config_test.c

$(BUILD)/config: $(CONFIG_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(CONFIG_SRC) -o $@

config: $(BUILD)/config
	$(BUILD)/config

# DEVICE_INFO and UPLINK_EVENT against the contract's vectors (ble/v1/device-info.md). The
# vectors arrived in contracts-v0.2.0; a checkout still pinned to 0.1.0 does not have them, so
# the row is built only where they exist and says so where they do not.
DEVINFO_VECTORS ?= $(CAIRN_CONTRACTS)/ble/v1/vectors/device-info/vectors.json
DEVINFO_SRC     := $(DEVINFO_DIR)/cairn_devinfo.c minijson.c devinfo_test.c
DEVINFO_CFLAGS  := $(CFLAGS) -DMJ_MAX_NODES=2500 -DMJ_MAX_STR=1100

.PHONY: devinfo checkin

# The check-in vectors (instructions and the home trigger, ble/v1/checkin.md) are in the same
# file as the device-info ones.
CHECKIN_SRC := $(FORMAT_SRC) $(CHECKIN_DIR)/cairn_checkin.c minijson.c checkin_test.c

$(BUILD)/checkin: $(CHECKIN_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(DEVINFO_CFLAGS) $(CHECKIN_SRC) -o $@

checkin: $(BUILD)/checkin
	$(BUILD)/checkin $(DEVINFO_VECTORS)

$(BUILD)/devinfo: $(DEVINFO_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(DEVINFO_CFLAGS) $(DEVINFO_SRC) -o $@

devinfo: $(BUILD)/devinfo
	$(BUILD)/devinfo $(DEVINFO_VECTORS)

ifneq ($(wildcard $(DEVINFO_VECTORS)),)
run: devinfo checkin
else
$(info device-info vectors not in this contracts pin (needs contracts-v0.2.0): devinfo row skipped)
endif

run: conformance faults ble-vectors mtprobe prov offload boottime uplink config

# Undefined behaviour in the scan path would be a parser reading past a torn
# segment — exactly the case the format exists to handle safely.
asan: | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) $(CONFORMANCE_SRC) -o $(BUILD)/conformance-asan
	$(BUILD)/conformance-asan $(FIXTURES)
	$(CC) $(CFLAGS) $(SAN) $(FAULTS_SRC) -o $(BUILD)/faults-asan
	$(BUILD)/faults-asan
	$(CC) $(CFLAGS) $(SAN) $(BLE_VEC_SRC) -lm -o $(BUILD)/ble-vectors-asan
	$(BUILD)/ble-vectors-asan $(BLE_VECTORS)
	$(CC) $(CFLAGS) $(SAN) $(SRC_DIR)/mtprobe_parse.c mtprobe_parse_test.c -o $(BUILD)/mtprobe-asan
	$(BUILD)/mtprobe-asan
	$(CC) $(CFLAGS) $(SAN) $(PROV_SRC) -o $(BUILD)/prov-asan
	CAIRN_ENROLL_VECTORS=$(ENROLL_VECTORS) $(BUILD)/prov-asan
	$(CC) $(CFLAGS) $(SAN) $(OFFLOAD_SRC) -o $(BUILD)/offload-asan
	$(BUILD)/offload-asan
	$(CC) $(CFLAGS) $(SAN) $(BOOTTIME_DIR)/cairn_boottime.c boottime_test.c -o $(BUILD)/boottime-asan
	$(BUILD)/boottime-asan
	$(CC) $(CFLAGS) $(SAN) $(UPLINK_DIR)/cairn_uplink.c uplink_test.c -o $(BUILD)/uplink-asan
	$(BUILD)/uplink-asan
	$(CC) $(CFLAGS) $(SAN) $(CONFIG_SRC) -o $(BUILD)/config-asan
	$(BUILD)/config-asan
	$(if $(wildcard $(DEVINFO_VECTORS)),$(CC) $(DEVINFO_CFLAGS) $(SAN) $(DEVINFO_SRC) -o $(BUILD)/devinfo-asan && $(BUILD)/devinfo-asan $(DEVINFO_VECTORS))
	$(if $(wildcard $(DEVINFO_VECTORS)),$(CC) $(DEVINFO_CFLAGS) $(SAN) $(CHECKIN_SRC) -o $(BUILD)/checkin-asan && $(BUILD)/checkin-asan $(DEVINFO_VECTORS))
	$(MAKE) asan-lte

clean:
	rm -rf $(BUILD)

# ── the C writer, for the server's reader ───────────────────────────────────────
#
# interop-writer seals bundles with lib/cairn_store + lib/cairn_format and leaves them in
# $(INTEROP_OUT) for the server's verifier (cairn-verify, Go) to judge. Building and running
# it needs nothing but a C compiler; judging its output needs the server repository, which
# is scripts/interop-writer.sh's job (and the server's interop job's), not this Makefile's.
# Appended as separate rules so it adds to run/asan without editing their lines.
INTEROP_OUT    ?= $(BUILD)/interop-out
INTEROP_W_SRC  := $(FORMAT_SRC) $(STORE_SRC) $(FS_SRC) $(APP_SRC) \
                  log_host.c platform_host.c interop_writer.c

.PHONY: interop-writer interop-writer-asan

$(BUILD)/interop-writer: $(INTEROP_W_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INTEROP_W_SRC) -o $@

interop-writer: $(BUILD)/interop-writer
	rm -rf $(INTEROP_OUT)
	$(BUILD)/interop-writer $(INTEROP_OUT)

interop-writer-asan: | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) $(INTEROP_W_SRC) -o $(BUILD)/interop-writer-asan
	rm -rf $(INTEROP_OUT)-asan
	$(BUILD)/interop-writer-asan $(INTEROP_OUT)-asan

run: interop-writer
asan: interop-writer-asan

# LTE digest (#20) and data accounting (#21): see lte.mk.
include lte.mk

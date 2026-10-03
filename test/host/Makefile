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
FIXTURES ?= ../../../../fixtures/format-v2

FORMAT_DIR := ../../lib/cairn_format
STORE_DIR  := ../../lib/cairn_store
FS_DIR     := ../../lib/cairn_fs
INCLUDE_DIR := ../../include
LOG_DIR     := ../../lib/cairn_log

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
APP_SRC    := $(SRC_DIR)/preroll.c $(SRC_DIR)/policy.c $(OTA_DIR)/cairn_ota.c $(POWER_DIR)/cairn_power.c

# The conformance runner needs the policy codec, which lives in src/.
BLE_VECTORS     ?= $(realpath ../../../../..)/cairn-companion-ios-esp32-obd2-gps-ble/docs/golden-vectors.json
CONFORMANCE_SRC := $(FORMAT_SRC) $(SRC_DIR)/policy.c minijson.c conformance.c
FAULTS_SRC      := $(FORMAT_SRC) $(STORE_SRC) $(FS_SRC) $(APP_SRC) \
                   log_host.c platform_host.c faults.c
BLE_VEC_SRC     := $(FORMAT_SRC) minijson.c ble_vectors.c

# -Wconversion is deliberately absent but the rest is strict: this code does a
# great deal of width narrowing between the wire format and C types, and a
# silent truncation there corrupts a bundle rather than crashing.
CFLAGS  ?= -std=c11 -O1 -g \
           -Wall -Wextra -Werror \
           -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes \
           -Wmissing-prototypes -Wno-unused-parameter \
           -I$(FORMAT_DIR) -I$(STORE_DIR) -I$(FS_DIR) -I$(LOG_DIR) -I$(INCLUDE_DIR) -I$(SRC_DIR) -I$(OTA_DIR) -I$(POWER_DIR) -I.

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer

BUILD := build

.PHONY: all run asan clean conformance faults ble-vectors

all: run

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/conformance: $(CONFORMANCE_SRC) $(FORMAT_DIR)/cairn_format.h minijson.h | $(BUILD)
	$(CC) $(CFLAGS) $(CONFORMANCE_SRC) -o $@

$(BUILD)/faults: $(FAULTS_SRC) $(STORE_DIR)/cairn_store.h $(STORE_DIR)/cairn_prune.h $(SRC_DIR)/preroll.h | $(BUILD)
	$(CC) $(CFLAGS) $(FAULTS_SRC) -o $@

$(BUILD)/ble-vectors: $(BLE_VEC_SRC) $(FORMAT_DIR)/cairn_format.h minijson.h | $(BUILD)
	$(CC) $(CFLAGS) $(BLE_VEC_SRC) -lm -o $@

conformance: $(BUILD)/conformance
	$(BUILD)/conformance $(FIXTURES)

faults: $(BUILD)/faults
	$(BUILD)/faults

ble-vectors: $(BUILD)/ble-vectors
	$(BUILD)/ble-vectors $(BLE_VECTORS)

run: conformance faults ble-vectors

# Undefined behaviour in the scan path would be a parser reading past a torn
# segment — exactly the case the format exists to handle safely.
asan: | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) $(CONFORMANCE_SRC) -o $(BUILD)/conformance-asan
	$(BUILD)/conformance-asan $(FIXTURES)
	$(CC) $(CFLAGS) $(SAN) $(FAULTS_SRC) -o $(BUILD)/faults-asan
	$(BUILD)/faults-asan
	$(CC) $(CFLAGS) $(SAN) $(BLE_VEC_SRC) -lm -o $(BUILD)/ble-vectors-asan
	$(BUILD)/ble-vectors-asan $(BLE_VECTORS)

clean:
	rm -rf $(BUILD)

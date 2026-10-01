# Host build of the firmware's format code, checked against the committed
# conformance vectors.
#
# The point of this target is that it compiles exactly the sources the device
# runs — lib/cairn_format/*.c, with no ESP-IDF dependency and no substitutions.
# Passing here is not a proxy for firmware correctness; it is the firmware's
# format implementation agreeing with the Go and Rust ones on real bytes.
#
#   make          build and run
#   make asan     the same, under AddressSanitizer and UBSan
#   make clean

CC      ?= cc
FIXTURES ?= ../../../../fixtures/format-v2

FORMAT_DIR := ../../lib/cairn_format
SOURCES    := $(wildcard $(FORMAT_DIR)/*.c) minijson.c conformance.c

# -Wconversion is deliberately on: this code does a great deal of width
# narrowing between the wire format and C types, and a silent truncation there
# would corrupt a bundle rather than crash.
CFLAGS  ?= -std=c11 -O1 -g \
           -Wall -Wextra -Werror \
           -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes \
           -Wmissing-prototypes -Wno-unused-parameter \
           -I$(FORMAT_DIR) -I.

BUILD := build
BIN   := $(BUILD)/conformance

.PHONY: all run asan clean

all: run

$(BIN): $(SOURCES) $(FORMAT_DIR)/cairn_format.h minijson.h | $(BUILD)
	$(CC) $(CFLAGS) $(SOURCES) -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: $(BIN)
	$(BIN) $(FIXTURES)

# Undefined behaviour in the scan path would be a parser reading past a torn
# segment, which is exactly the case the format exists to handle safely.
asan: $(SOURCES) | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(SOURCES) -o $(BUILD)/conformance-asan
	$(BUILD)/conformance-asan $(FIXTURES)

clean:
	rm -rf $(BUILD)

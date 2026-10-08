# Engine profiles: the evaluator, the generated N20 tables against the hard-coded
# values, the vehicle gate. Included from Makefile; everything here is additive.
#
#   make engine                 against the committed all-engines tables
#   make engine-sel ENGINE_GEN_DIR=<dir with cairn_engines_sel.h> ENGINE_EXPECT=bmw-n20
#                               against a generated selection (make engines-check does this)

# The first rule below must not become the default goal.
.DEFAULT_GOAL := all

ENGINES_YAML   := ../../engines
# The formula vectors belong to contracts/engine/v1, not to this repository: the C
# evaluator is one of the three implementations held to them, so it must read the
# contract's copy and not a local one. CAIRN_CONTRACTS wins, as everywhere else.
CONTRACTS      ?= $(if $(CAIRN_CONTRACTS),$(CAIRN_CONTRACTS),../../.contracts/contracts)
ENGINE_VECTORS := $(CONTRACTS)/engine/v1/vectors/expr.txt
ENGINE_GEN_HDR := $(wildcard $(ENGINE_DIR)/gen/*.h)

# The engine module reaches the binaries that link policy.c and cairn_power.c through
# APP_SRC in the Makefile; the conformance runner links policy.c on its own.
CFLAGS            += -I$(ENGINE_DIR)
CONFORMANCE_SRC   += $(ENGINE_LIB_SRC)
HEADERS           += $(ENGINE_GEN_HDR) $(wildcard $(ENGINE_DIR)/*.h)

ENGINE_SRC := $(FORMAT_SRC) $(ENGINE_LIB_SRC) engine_test.c

.PHONY: engine engine-sel engine-asan

$(BUILD)/engine: $(ENGINE_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(ENGINE_SRC) -o $@

engine: $(BUILD)/engine
	$(BUILD)/engine $(ENGINE_VECTORS) $(ENGINES_YAML)

engine-asan: | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) $(ENGINE_SRC) -o $(BUILD)/engine-asan
	$(BUILD)/engine-asan $(ENGINE_VECTORS) $(ENGINES_YAML)

# A generated selection. Rebuilt every time: the header's directory changes between runs.
engine-sel: | $(BUILD)
	$(CC) $(CFLAGS) -I$(ENGINE_GEN_DIR) $(ENGINE_SRC) -o $(BUILD)/engine-sel
	$(BUILD)/engine-sel $(ENGINE_VECTORS) $(ENGINES_YAML) $(ENGINE_EXPECT)

run: engine
asan: engine-asan

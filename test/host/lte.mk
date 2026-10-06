# Host suites for the LTE work (issues #20 digest, #21 usage accounting).
#
# Included from the Makefile so the shared file changes by one line; everything
# here is additive. Both suites are PROVISIONAL: they test the portable cores in
# lib/cairn_digest and lib/cairn_usage against synthetic data, because
# contracts/digest/v1 and contracts/config/v1 are unreleased and there are no
# contract vectors yet.
#
#   make digest        digest generator + digest-ack-is-not-a-receipt rows + size table
#   make usage         data accounting, caps, power-cut and retry-breaker rows
#   make asan-lte      both, under AddressSanitizer and UBSan (also run by `make asan`)
#   make mutate        prove the key safety rows catch deliberate breakage

DIGEST_DIR := ../../lib/cairn_digest
USAGE_DIR  := ../../lib/cairn_usage

LTE_CFLAGS  = $(CFLAGS) -I$(DIGEST_DIR) -I$(USAGE_DIR)
LTE_HEADERS = $(wildcard $(DIGEST_DIR)/*.h $(USAGE_DIR)/*.h)

# The digest ack test reaches the real prune gate, so it links the real store.
DIGEST_SRC = $(FORMAT_SRC) $(STORE_SRC) $(FS_SRC) $(APP_SRC) $(wildcard $(DIGEST_DIR)/*.c) \
             log_host.c platform_host.c digest_test.c
# The usage counters persist through the real key-value store (host backing).
USAGE_SRC  = $(FORMAT_SRC) $(FS_DIR)/cairn_kv_posix.c $(wildcard $(USAGE_DIR)/*.c) \
             log_host.c platform_host.c usage_test.c

.PHONY: digest usage asan-lte mutate

$(BUILD)/digest: $(DIGEST_SRC) $(HEADERS) $(LTE_HEADERS) | $(BUILD)
	$(CC) $(LTE_CFLAGS) $(DIGEST_SRC) -lm -o $@

digest: $(BUILD)/digest
	$(BUILD)/digest

$(BUILD)/usage: $(USAGE_SRC) $(HEADERS) $(LTE_HEADERS) | $(BUILD)
	$(CC) $(LTE_CFLAGS) $(USAGE_SRC) -lm -o $@

usage: $(BUILD)/usage
	$(BUILD)/usage

run: digest usage

asan-lte: | $(BUILD)
	$(CC) $(LTE_CFLAGS) $(SAN) $(DIGEST_SRC) -lm -o $(BUILD)/digest-asan
	$(BUILD)/digest-asan
	$(CC) $(LTE_CFLAGS) $(SAN) $(USAGE_SRC) -lm -o $(BUILD)/usage-asan
	$(BUILD)/usage-asan

mutate:
	sh mutate.sh

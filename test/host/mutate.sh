#!/bin/sh
# Mutation check for the LTE safety rows (digest #20, usage #21).
#
# For each deliberate breakage of the safety logic, copy lib/cairn_digest and
# lib/cairn_usage to a temp dir, apply the edit there (the real sources are never
# touched), build the suite against the copy, and require it to FAIL. A mutant that
# does not apply or compile is reported INVALID rather than counted as caught, and
# the unmutated suites must pass first, otherwise "failed" would prove nothing.
#
#   sh mutate.sh        (or: make mutate)

set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=$(mktemp -d /tmp/cairn-mutate.XXXXXX)
trap 'rm -rf "$work"' EXIT

caught=0
missed=0
invalid=0

fresh_copy() {
    rm -rf "$work/dig" "$work/use" "$work/build"
    cp -R "$repo/lib/cairn_digest" "$work/dig"
    cp -R "$repo/lib/cairn_usage" "$work/use"
}

build() { # suite
    make -s -C "$here" "$work/build/$1" DIGEST_DIR="$work/dig" USAGE_DIR="$work/use" BUILD="$work/build" >"$work/build.log" 2>&1
}

# mutant <name> <suite> <file under the copy> <perl -0 substitution> <what must catch it>
mutant() {
    name=$1; suite=$2; file=$3; expr=$4; why=$5
    fresh_copy
    before=$(cksum <"$work/$file")
    perl -0pi -e "$expr" "$work/$file"
    after=$(cksum <"$work/$file")
    if [ "$before" = "$after" ]; then
        printf '  INVALID  %-30s edit did not apply\n' "$name"
        invalid=$((invalid + 1))
        return
    fi
    if ! build "$suite"; then
        printf '  INVALID  %-30s does not compile\n' "$name"
        invalid=$((invalid + 1))
        return
    fi
    if "$work/build/$suite" >"$work/run.log" 2>&1; then
        printf '  MISSED   %-30s the %s suite still passes\n' "$name" "$suite"
        missed=$((missed + 1))
    else
        row=$(grep 'FAIL  \[' "$work/run.log" | head -1 | sed 's/^ *//')
        printf '  caught   %-30s %s\n' "$name" "$row"
        caught=$((caught + 1))
    fi
}

echo "baseline: the unmutated suites must pass"
fresh_copy
for s in digest usage; do
    if ! build "$s" || ! "$work/build/$s" >"$work/base.log" 2>&1; then
        echo "  the unmutated $s suite does not pass; fix that first"
        tail -15 "$work/base.log"
        exit 2
    fi
    echo "  $s: pass"
done

echo
echo "digest (#20): budget, ceiling, and the digest ack is not a receipt"
mutant "ceiling-clamp-removed" digest dig/cairn_digest.c \
    's/\s*if \(b > CAIRN_DIGEST_BYTES_HARD_CEILING\) b = CAIRN_DIGEST_BYTES_HARD_CEILING;//' \
    "a config budget above the compiled ceiling"
mutant "budget-not-enforced" digest dig/cairn_digest_wire.c \
    's/writer_t w = \{ out, budget, 0, false \};/writer_t w = { out, 0x7FFFFFFFu, 0, false };/' \
    "output longer than the budget"
mutant "no-reduction-to-fit" digest dig/cairn_digest.c \
    's/if \(cairn_digest_drop_least_important\(d\)\) \{\s*d->route_reduced_by_budget/if (0) {\n            d->route_reduced_by_budget/' \
    "a tight budget is met by reducing points"
mutant "ack-signed-in-receipt-context" digest dig/cairn_digest_ack.c \
    's{(/\* MUTATION-SITE ack-context begin \*/).*?(/\* MUTATION-SITE ack-context end \*/)}{$1
    cairn_receipt_t r; memset(&r, 0, sizeof(r));
    r.receipt_version = CAIRN_RECEIPT_VERSION;
    memcpy(r.receipt_id, a->trip_root, 16); memcpy(r.bundle_id, a->trip_root, 16);
    memcpy(r.content_root, a->trip_root, 32);
    r.server_ingest_utc_ms = a->server_ingest_utc_ms; r.ingest_schema_version = 1;
    memcpy(r.server_key_id, a->server_key_id, 8);
    memcpy(r.signature_algorithm, CAIRN_SIGALG_ED25519, sizeof(CAIRN_SIGALG_ED25519));
    size_t w = 0;
    if (cairn_receipt_signing_bytes(&r, out, cap, &w) != CAIRN_OK) return 0;
    return w;
    $2}s' \
    "a signature on a digest ack would verify as a receipt"
mutant "ack-message-type-unchecked" digest dig/cairn_digest_ack.c \
    's{(/\* MUTATION-SITE ack-type begin \*/).*?(/\* MUTATION-SITE ack-type end \*/)}{$1 $2}s' \
    "message-type confusion"

echo
echo "usage (#21): caps, ceiling, signed-only raises, breaker, persistence"
mutant "monthly-cap-not-enforced" usage use/cairn_usage.c \
    's/if \(used >= cap \|\| used \+ want > cap\) return CAIRN_STOP_CAP_MONTHLY;//' \
    "a monthly cap stops traffic"
mutant "daily-cap-not-enforced" usage use/cairn_usage.c \
    's/if \(used >= cap \|\| used \+ want > cap\) return CAIRN_STOP_CAP_DAILY;//' \
    "a daily cap stops traffic"
mutant "trip-cap-not-enforced" usage use/cairn_usage.c \
    's/if \(used >= cap \|\| used \+ want > cap\) return CAIRN_STOP_CAP_TRIP;//' \
    "a per-trip cap stops traffic"
mutant "ceiling-not-applied" usage use/cairn_usage.c \
    's{(MUTATION-SITE ceiling begin \*/).*?(/\* MUTATION-SITE ceiling end)}{$1\n    return configured;\n    $2}s' \
    "the enforced cap is min(config, ceiling)"
mutant "unsigned-may-loosen" usage use/cairn_usage.c \
    's{(MUTATION-SITE unsigned-loosen begin \*/).*?(/\* MUTATION-SITE unsigned-loosen end)}{$1 (void)auth; (void)config_loosens; $2}s' \
    "an unsigned message cannot raise a limit"
mutant "unsigned-may-raise-ceiling" usage use/cairn_usage.c \
    's{(MUTATION-SITE ceiling-raise begin \*/).*?(/\* MUTATION-SITE ceiling-raise end)}{$1 (void)auth; (void)raises; $2}s' \
    "an unsigned message cannot raise the ceiling"
mutant "breaker-never-opens" usage use/cairn_usage.c \
    's/u->consec_failures >= u->tune\.breaker_threshold/0/' \
    "the retry breaker trips"
mutant "no-max-attempts" usage use/cairn_usage.c \
    's/>= u->tune\.max_attempts_per_trip\)/>= 0xFFFF)/' \
    "max attempts per trip"
mutant "no-backoff" usage use/cairn_usage.c \
    's/u->trips\[ti\]\.next_ok_mono > now->mono_s/0/' \
    "per-trip backoff"
mutant "no-power-cut-margin" usage use/cairn_usage.c \
    's/if \(dirty\) margin \+=/(void)dirty; if (0) margin +=/' \
    "a power cut never under-counts"

echo
echo "caught $caught, missed $missed, invalid $invalid"
[ "$missed" -eq 0 ] && [ "$invalid" -eq 0 ]

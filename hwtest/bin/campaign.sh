#!/bin/bash
# Run a sequence of run-suite.sh runs unattended (e.g. overnight).
#
#   campaign.sh <plan file>
#
# Each plan line is "<image.rbf> <label> [phase...] [SOAK_MINUTES=<n>]", i.e.
# run-suite.sh's arguments; blank lines and # comments are ignored. After each
# run the board is brought back to the official image with recover.sh; if
# that needs a human (replug, bootloader), the campaign stops, since every
# later run would fail anyway. The campaign log goes to
# $RESULTS_DIR/campaign-<time>.log; each run has its own run-suite.sh
# directory, listed at the end.
set -uo pipefail
. "$(dirname "$0")/../lib/common.sh"

[ $# -eq 1 ] && [ -f "$1" ] || { sed -n '2,13p' "$0"; exit 2; }
mapfile -t lines < "$1"
check_good_images

logf="$RESULTS_DIR/campaign-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$RESULTS_DIR"
exec > >(tee -a "$logf") 2>&1
log "campaign plan $1 (TX mode: ${HWTEST_TX})"
sed 's/^/    /' "$1"

n=0
for line in "${lines[@]}"; do
    line="${line%%#*}"
    [ -n "${line//[[:space:]]/}" ] || continue
    n=$((n + 1))
    read -ra words <<< "$line"
    envs=(); args=()
    for w in "${words[@]}"; do
        case "$w" in
            SOAK_MINUTES=*) envs+=("$w") ;;
            *) args+=("${w/#\~/$HOME}") ;;
        esac
    done
    log "=== run $n: ${args[*]} ${envs[*]}"
    env "${envs[@]}" "$HWTEST_DIR/bin/run-suite.sh" "${args[@]}" < /dev/null
    log "=== run $n exit status $?"
    if ! "$HWTEST_DIR/bin/recover.sh" < /dev/null; then
        log "the board cannot be recovered without a human: stopping the campaign"
        exit 3
    fi
done
log "=== campaign done: $n runs"
grep -h "results: " "$logf" | sed 's/^/    /'

#!/bin/bash
# Run a sequence of run-suite.sh runs unattended (e.g. overnight).
#
#   campaign.sh <plan file>
#
# Each plan line is "<image.rbf> <label> [phase...] [VAR=<value>...]", i.e.
# run-suite.sh's arguments plus settings for that run (SOAK_MINUTES,
# FULLSOAK_RX_MIN, FULLSOAK_LOOP_MIN, BIST_MIN, SCHED_S, HOST_BUILD,
# HWTEST_SKIP, HWTEST_FIXES; see run-suite.sh); blank lines and # comments
# are ignored. HWTEST_TX, HWTEST_RF_CH and HWTEST_2CH describe the cabling,
# and HWTEST_FPGA the board, so they apply to the whole campaign: set them in
# its environment, not per line (a plan that sets them on a line is refused
# before anything runs, as is one with an image that is missing or of another
# FPGA variant, or a board of another variant).
# After each run the board is brought back to the official image with
# recover.sh; if that needs a human (replug, bootloader), the campaign
# stops, since every later run would fail anyway. The campaign log goes to
# $RESULTS_DIR/campaign-<time>.log; each run has its own run-suite.sh
# directory, listed at the end.
set -uo pipefail
. "$(dirname "$0")/../lib/common.sh"

[ $# -eq 1 ] && [ -f "$1" ] || { awk 'NR > 1 && /^set -uo/ { exit } NR > 1' "$0"; exit 2; }
mapfile -t lines < "$1"
check_good_images
# A cabling variable on a plan line would reach run-suite.sh as a phase name
# and only fail when that phase came up, hours in
if sed 's/#.*//' "$1" | grep -Eq '(^|[[:space:]])(HWTEST_TX|HWTEST_RF_CH|HWTEST_2CH|HWTEST_FPGA)='; then
    die "$1 sets HWTEST_TX, HWTEST_RF_CH, HWTEST_2CH or HWTEST_FPGA on a plan line: they describe the cabling and the board, so set them in the campaign's environment"
fi
# Every image of the plan must exist and be a $HWTEST_FPGA image, and the
# board must have that FPGA: find out now, not hours in
while read -r img _; do
    img="${img/#\~/$HOME}"
    [ -f "$img" ] || die "$1: no such image: $img"
    check_image_variant "$img"
done < <(sed 's/#.*//' "$1" | grep -v '^[[:space:]]*$')
check_fpga_variant

logf="$RESULTS_DIR/campaign-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$RESULTS_DIR"
exec > >(tee -a "$logf") 2>&1
log "campaign plan $1 (FPGA variant: $HWTEST_FPGA, bladeRF $DEVICE_SERIAL; TX mode: ${HWTEST_TX}, RF channel: ${HWTEST_RF_CH:-2}, two-channel setup: ${HWTEST_2CH:-none})"
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
            SOAK_MINUTES=*|FULLSOAK_RX_MIN=*|FULLSOAK_LOOP_MIN=*|BIST_MIN=*|SCHED_S=*|HOST_BUILD=*|HWTEST_SKIP=*|HWTEST_FIXES=*|HWTEST_FPGA_MIMO=*)
                envs+=("${w/=\~/=$HOME}") ;;
            *) args+=("${w/#\~/$HOME}") ;;
        esac
    done
    log "=== run $n: ${args[*]} ${envs[*]}"
    # run-suite.sh sets up its own LD_PRELOAD and LD_LIBRARY_PATH from
    # HOST_BUILD, so don't hand it the campaign's
    env -u LD_PRELOAD -u LD_LIBRARY_PATH "${envs[@]}" \
        "$HWTEST_DIR/bin/run-suite.sh" "${args[@]}" < /dev/null
    log "=== run $n exit status $?"
    if ! "$HWTEST_DIR/bin/recover.sh" < /dev/null; then
        log "the board cannot be recovered without a human: stopping the campaign"
        exit 3
    fi
done
log "=== campaign done: $n runs"
grep -h "results: " "$logf" | sed 's/^/    /'

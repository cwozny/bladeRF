#!/bin/bash
# Receive-only diagnosis of test_ctrl's FPGA-mode bandwidth failures
# ("Unexpected bandwidth. requested=X, actual=3, readback=X"; see README.md).
#
#   bw-diag.sh <image label>...     e.g. A-official-v0.16.0 C-niosv-q25.1
#
# For each $HWTEST_HOME/images/<label>.rbf: loads it (volatile), runs the full
# bandwidth test in FPGA tuning mode (default seed: the same bandwidths as the
# deep phase) with every Nios packet traced by lib/nios-trace.c, and analyzes
# the trace with nios-trace-analyze.py. Afterwards the official image is
# reloaded. The bandwidth test never enables a channel, and the no-TX guard
# stays preloaded.
set -uo pipefail
. "$(dirname "$0")/../lib/common.sh"

[ $# -ge 1 ] || { sed -n '2,12p' "$0"; exit 2; }
require_no_tx_guard
check_good_images
check_search_path

T="$HOST_BUILD/output"
SO="$T/libhwtest-nios-trace.so"
out="$RESULTS_DIR/bw-diag/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"

cc -shared -fPIC -O2 -Wall -Wextra -Werror \
    -I"$HWTEST_DIR/../host/libraries/libbladeRF/include" -I/usr/include/libusb-1.0 \
    -o "$SO" "$HWTEST_DIR/lib/nios-trace.c" -ldl || die "cannot build the trace shim"

for label in "$@"; do
    img="$HWTEST_HOME/images/$label.rbf"
    [ -f "$img" ] || die "no such image: $img"
    log "=== $label"
    if ! "$BCLI" -l "$img" > "$out/$label.load.log" 2>&1 ||
       ! "$BCLI" -e version > "$out/$label.version.log" 2>&1 ||
       grep -q "FPGA not loaded" "$out/$label.version.log"; then
        log "image does not come up; recovering"
        "$HWTEST_DIR/bin/recover.sh"
        exit 1
    fi
    t0=$SECONDS
    BLADERF_DEFAULT_TUNING_MODE=fpga NIOS_TRACE_FILE="$out/$label.trace" \
        LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$SO" \
        timeout --kill-after=10 2400 "$T/libbladeRF_test_ctrl" -T fpga -t bandwidth \
        > "$out/$label.log" 2>&1
    rc=$?
    log "$label: rc=$rc, $((SECONDS - t0)) s," \
        "$(grep -c 'Unexpected bandwidth' "$out/$label.log") 'Unexpected bandwidth' lines"
    python3 -I "$HWTEST_DIR/bin/nios-trace-analyze.py" "$out/$label.trace" 4 \
        > "$out/$label.analysis.txt" 2>&1
    head -12 "$out/$label.analysis.txt"
done

"$HWTEST_DIR/bin/recover.sh"
log "results in $out"

#!/bin/bash
# Load an FPGA image (volatile) and run the regression suite against it.
#
#   run-suite.sh <image.rbf> <label> [phase...]
#
# Phases (default: smoke host fpga rf):
#   smoke  versions, info, open/close loop: does the Nios answer at all?
#   host   libbladeRF tests with host-side AD9361 control (libbladeRF's
#          default): Nios packet handling, SPI passthrough, timestamps,
#          retune queue, loopbacks, streaming
#   fpga   the same with AD9361 control running on the Nios
#          (BLADERF_DEFAULT_TUNING_MODE=fpga): exercises the AD9361 driver
#          in the firmware
#   rf     rf-check.py against the B200mini, in both tuning modes
#          (transmits: only with HWTEST_TX=1)
#   soak   repeats a short mixed workload for SOAK_MINUTES (default 90,
#          i.e. longer than any IP evaluation time-out)
#
# Unless HWTEST_TX=1 (every bladeRF TX port cabled through >= 30 dB or
# terminated; see README.md), the suite is receive-only and antennas may be
# attached: tests that enable a TX channel are left out (the no-TX guard would
# refuse them anyway), test_ctrl runs case by case without its enable_module
# and threads cases, and the rf phase is skipped. Retunes are then exercised
# with RX retunes scheduled on timestamps (test_scheduled_retune), which also
# runs in TX mode.
#
# Results: $RESULTS_DIR/<label>/<time>/ with one log per test, summary.jsonl
# ({"test", "rc", "seconds"}), rf.jsonl and meta.json. Compare runs with
# compare.py. Only test programs on the allowlist below are ever run; the two
# flash/bootloader tests (test_flash_validate, test_bootloader_recovery) are
# deliberately absent. Afterwards the official image is reloaded.
set -uo pipefail
. "$(dirname "$0")/../lib/common.sh"

[ $# -ge 2 ] || { sed -n '2,24p' "$0"; exit 2; }
image=$(readlink -f "$1"); label=$2; shift 2
phases=("$@"); [ ${#phases[@]} -gt 0 ] || phases=(smoke host fpga rf)
[ -f "$image" ] || die "no such image: $image"
check_good_images
check_search_path

out="$RESULTS_DIR/$label/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out/logs"
T="$HOST_BUILD/output"
mkdir -p "$out/scratch"     # working directory of tests that write files
HOPS="$HWTEST_DIR/lib/hops-rx.txt"
exec > >(tee -a "$out/run.log") 2>&1

tx_ok() { [ "$HWTEST_TX" = 1 ]; }
if tx_ok; then
    log "TX allowed (HWTEST_TX=1): every bladeRF TX port must be cabled through >= 30 dB or terminated, no antennas"
else
    require_no_tx_guard
    log "receive-only (HWTEST_TX is not 1): no TX channel is enabled; TX tests are left out"
fi

cat > "$out/meta.json" <<EOF
{"label": "$label", "image": "$image", "image_md5": "$(md5_of "$image")",
 "phases": "${phases[*]}", "tx": $(tx_ok && echo true || echo false),
 "start": "$(date -Is)", "host": "$(hostname)",
 "host_sw": "$(git -C "$HWTEST_DIR/.." describe --always --dirty 2>/dev/null)"}
EOF

# Output checkers for tests whose exit status does not reflect data errors
check_tuning_fpga() {   # libbladeRF falls back to host tuning if the Nios cannot
    grep -q "Tuning Mode: FPGA" "$1"
}
check_iq_matches() {    # needs "IQ Matches: X / Y" lines, all with X == Y
    awk '/IQ Matches:/ { n++; split($0, a, /IQ Matches:[ ]*/); split(a[2], b, /[ ]*\/[ ]*/);
                         if (b[1] + 0 != b[2] + 0) bad++ }
         END { exit (n == 0 || bad > 0) }' "$1"
}

# runtest <name> <timeout_s> <check> <cmd...>
# A test fails if it exits nonzero, times out, or fails <check>: "-" (none),
# "@function" (a checker above, given the log file), or an extended regex
# that must NOT appear in the output.
runtest() {
    local name=$1 tmo=$2 re=$3; shift 3
    local log="$out/logs/$name.log" t0=$SECONDS rc
    log "--- $name: $*"
    timeout --kill-after=10 "$tmo" "$@" > "$log" 2>&1
    rc=$?
    if [ $rc -eq 0 ] && [ "$re" != "-" ]; then
        if [ "${re:0:1}" = "@" ]; then
            "${re:1}" "$log" || rc=99
        elif grep -Eq "$re" "$log"; then
            rc=99
        fi
    fi
    printf '{"test": "%s", "rc": %d, "seconds": %d}\n' "$name" "$rc" $((SECONDS - t0)) >> "$out/summary.jsonl"
    if [ $rc -eq 0 ]; then
        log "    ok ($((SECONDS - t0)) s)"
    else
        log "    FAILED rc=$rc ($((SECONDS - t0)) s), see $log"
        tail -5 "$log" | sed 's/^/      /'
    fi
    return $rc
}

# (Re)load the image under test and make sure its Nios answers; if not, run
# the recovery ladder and stop: later tests would only produce noise.
load_image() {
    if ! runtest "load-$1" 120 - "$BCLI" -l "$image" ||
       ! runtest "version-$1" 60 "Unknown \(FPGA not loaded\)" "$BCLI" -e version; then
        log "image under test does not come up; recovering and stopping"
        "$HWTEST_DIR/bin/recover.sh"
        exit 1
    fi
}

# RX retunes scheduled on timestamps through the Nios retune queue, with quick
# tune profiles (24 frequencies from 300 MHz to 3.75 GHz: more than the
# AD9361's 8 fast-lock slots, so the Nios also swaps profiles in from its RAM)
scheduled_retune_rx() {     # scheduled_retune_rx <test name>
    runtest "$1" 300 - env -C "$out/scratch" \
        "$T/libbladeRF_test_scheduled_retune" rx "$HOPS" quick
    local rc=$?
    rm -f "$out/scratch/samples.bin"
    return $rc
}

# test_ctrl cases that do not enable a TX channel (all but enable_module and
# threads; xb200 needs an XB-200)
CTRL_RX_CASES="sampling lpf_mode loopback rx_mux correction samplerate bandwidth gain frequency"

# Tests that pass in either tuning mode. Fail regexes from each test's output.
common_tests() {
    local m=$1 c
    if tx_ok; then
        runtest "ctrl-$m" 900 "FAILED|Failed|failure" "$T/libbladeRF_test_ctrl" -T "$m"
    else
        for c in $CTRL_RX_CASES; do
            runtest "ctrl_$c-$m" 600 "FAILED|Failed|failure|Invalid test" \
                "$T/libbladeRF_test_ctrl" -T "$m" -t "$c"
        done
    fi
    runtest "open-$m" 300 - "$T/libbladeRF_test_open" -c 25
    if tx_ok; then
        runtest "quick_retune-$m" 300 - "$T/libbladeRF_test_quick_retune" -i 200
    fi
    scheduled_retune_rx "scheduled_retune_rx-$m"
    runtest "freq_hop_rx-$m" 600 - "$T/libbladeRF_test_freq_hop" --rx -i 500
    runtest "repeated_stream_rx-$m" 300 - "$T/libbladeRF_test_repeated_stream" --rx -r 100
    if tx_ok; then
        runtest "repeated_stream_tx-$m" 300 - "$T/libbladeRF_test_repeated_stream" --tx -r 100
    fi
    runtest "rx_meta-$m" 300 - "$T/libbladeRF_test_rx_meta"
    runtest "rx_discont-$m" 300 "Expected.*Got" "$T/libbladeRF_test_rx_discont" -i 2000
    runtest "timestamps_rx_gaps-$m" 300 "[1-9][0-9]* test cases failed" \
        "$T/libbladeRF_test_timestamps" -t rx_gaps -f
    runtest "timestamps_rx_scheduled-$m" 300 "Test failed" \
        "$T/libbladeRF_test_timestamps" -t rx_scheduled -f
    if tx_ok; then
        runtest "timestamps_tx_onoff_nowsched-$m" 300 "Test failed|failed" \
            "$T/libbladeRF_test_timestamps" -t tx_onoff_nowsched -f
        runtest "digital_loopback_fw-$m" 300 @check_iq_matches "$T/libbladeRF_test_digital_loopback" -l fw -c 100
        runtest "digital_loopback_fpga-$m" 300 @check_iq_matches "$T/libbladeRF_test_digital_loopback" -l fpga -c 100
    fi
    runtest "unused_sync-$m" 120 - "$T/libbladeRF_test_unused_sync"
}

for phase in "${phases[@]}"; do
    log "===== phase $phase ($label)"
    case "$phase" in
    smoke)
        load_image smoke
        runtest info 60 - "$BCLI" -e info -e version -e "print"
        runtest flash_id 60 - "$T/libbladeRF_test_flash_id"
        runtest open-smoke 300 - "$T/libbladeRF_test_open" -c 10
        ;;
    host)
        load_image host
        export BLADERF_DEFAULT_TUNING_MODE=host
        common_tests host
        if tx_ok; then
            runtest digital_loopback_rfic-host 300 @check_iq_matches "$T/libbladeRF_test_digital_loopback" -l rfic -c 100
        fi
        runtest tune_timing-host 600 - "$T/libbladeRF_test_tune_timing" -i 100
        runtest fpga_load 300 - "$T/libbladeRF_test_fpga_load" -d "*" -f "$image"
        # Nios round-trip latency; writes AD9361 register 0x000 (volatile), so
        # reload the image afterwards
        runtest peripheral_timing 600 - "$T/libbladeRF_test_peripheral_timing"
        load_image after-peripheral-timing
        unset BLADERF_DEFAULT_TUNING_MODE
        ;;
    fpga)
        load_image fpga
        export BLADERF_DEFAULT_TUNING_MODE=fpga
        runtest tuning_mode-fpga 60 @check_tuning_fpga "$BCLI" -e "print tuning_mode"
        common_tests fpga
        unset BLADERF_DEFAULT_TUNING_MODE
        ;;
    rf)
        if ! tx_ok; then
            log "rf phase skipped: it transmits (needs HWTEST_TX=1 and the cabling in README.md)"
            continue
        fi
        load_image rf
        for m in host fpga; do
            BLADERF_DEFAULT_TUNING_MODE=$m runtest "rf-$m" 1800 - \
                python3 "$HWTEST_DIR/bin/rf-check.py" all --out "$out/rf-$m.jsonl"
        done
        cat "$out"/rf-*.jsonl > "$out/rf.jsonl" 2>/dev/null
        ;;
    soak)
        load_image soak
        end=$((SECONDS + ${SOAK_MINUTES:-90} * 60)); i=0
        while [ $SECONDS -lt $end ]; do
            i=$((i + 1))
            m=$([ $((i % 2)) -eq 0 ] && echo fpga || echo host)
            BLADERF_DEFAULT_TUNING_MODE=$m runtest "soak-$i-open-$m" 300 - "$T/libbladeRF_test_open" -c 5
            if tx_ok; then
                BLADERF_DEFAULT_TUNING_MODE=$m runtest "soak-$i-retune-$m" 300 - "$T/libbladeRF_test_quick_retune" -i 50
            fi
            BLADERF_DEFAULT_TUNING_MODE=$m scheduled_retune_rx "soak-$i-sched_retune_rx-$m"
            BLADERF_DEFAULT_TUNING_MODE=$m runtest "soak-$i-stream-$m" 300 - "$T/libbladeRF_test_repeated_stream" --rx -r 20
        done
        ;;
    *)
        die "unknown phase $phase" ;;
    esac
done

log "===== done; reloading the official image"
"$HWTEST_DIR/bin/recover.sh" > "$out/logs/final-recover.log" 2>&1 || log "final recover.sh failed, see logs"
python3 - "$out" <<'EOF'
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1] + "/summary.jsonl")]
bad = [r["test"] for r in rows if r["rc"] != 0]
print("%d tests, %d failed%s" % (len(rows), len(bad), (": " + ", ".join(bad)) if bad else ""))
EOF
log "results: $out"

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
#   soak   repeats a short mixed workload for SOAK_MINUTES (default 90,
#          i.e. longer than any IP evaluation time-out)
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
exec > >(tee -a "$out/run.log") 2>&1

cat > "$out/meta.json" <<EOF
{"label": "$label", "image": "$image", "image_md5": "$(md5_of "$image")",
 "phases": "${phases[*]}", "start": "$(date -Is)", "host": "$(hostname)",
 "host_sw": "$(git -C "$HWTEST_DIR/.." describe --always --dirty 2>/dev/null)"}
EOF

# Output checkers for tests whose exit status does not reflect data errors
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
    if ! runtest "load-$1" 120 - bcli -l "$image" ||
       ! runtest "version-$1" 60 "Unknown \(FPGA not loaded\)" bcli -e version; then
        log "image under test does not come up; recovering and stopping"
        "$HWTEST_DIR/bin/recover.sh"
        exit 1
    fi
}

# Tests that pass in either tuning mode. Fail regexes from each test's output.
common_tests() {
    local m=$1
    runtest "ctrl-$m" 900 "FAILED|Failed|failure" "$T/libbladeRF_test_ctrl" -T "$m"
    runtest "open-$m" 300 - "$T/libbladeRF_test_open" -c 25
    runtest "quick_retune-$m" 300 - "$T/libbladeRF_test_quick_retune" -i 200
    runtest "freq_hop_rx-$m" 600 - "$T/libbladeRF_test_freq_hop" --rx -i 500
    runtest "repeated_stream_rx-$m" 300 - "$T/libbladeRF_test_repeated_stream" --rx -r 100
    runtest "repeated_stream_tx-$m" 300 - "$T/libbladeRF_test_repeated_stream" --tx -r 100
    runtest "rx_meta-$m" 300 - "$T/libbladeRF_test_rx_meta"
    runtest "rx_discont-$m" 300 "Expected.*Got" "$T/libbladeRF_test_rx_discont" -i 2000
    runtest "timestamps_rx_gaps-$m" 300 "[1-9][0-9]* test cases failed" \
        "$T/libbladeRF_test_timestamps" -t rx_gaps -f
    runtest "timestamps_rx_scheduled-$m" 300 "Test failed" \
        "$T/libbladeRF_test_timestamps" -t rx_scheduled -f
    runtest "timestamps_tx_onoff_nowsched-$m" 300 "Test failed|failed" \
        "$T/libbladeRF_test_timestamps" -t tx_onoff_nowsched -f
    runtest "digital_loopback_fw-$m" 300 @check_iq_matches "$T/libbladeRF_test_digital_loopback" -l fw -c 100
    runtest "digital_loopback_fpga-$m" 300 @check_iq_matches "$T/libbladeRF_test_digital_loopback" -l fpga -c 100
    runtest "unused_sync-$m" 120 - "$T/libbladeRF_test_unused_sync"
}

for phase in "${phases[@]}"; do
    log "===== phase $phase ($label)"
    case "$phase" in
    smoke)
        load_image smoke
        runtest info 60 - bcli -e info -e version -e "print"
        runtest flash_id 60 - "$T/libbladeRF_test_flash_id"
        runtest open-smoke 300 - "$T/libbladeRF_test_open" -c 10
        ;;
    host)
        load_image host
        export BLADERF_DEFAULT_TUNING_MODE=host
        common_tests host
        runtest digital_loopback_rfic-host 300 @check_iq_matches "$T/libbladeRF_test_digital_loopback" -l rfic -c 100
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
        runtest tuning_mode-fpga 60 "host" bcli -e "print tuning_mode"
        common_tests fpga
        unset BLADERF_DEFAULT_TUNING_MODE
        ;;
    rf)
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
            BLADERF_DEFAULT_TUNING_MODE=$m runtest "soak-$i-retune-$m" 300 - "$T/libbladeRF_test_quick_retune" -i 50
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

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
#   deep   test_ctrl's full frequency, bandwidth and sample rate sweeps (the
#          other phases run its -f variants) with AD9361 control on the
#          Nios: ~1 hour with the official image
#   rf     rf-check.py against the B200mini, in both tuning modes
#          (transmits: only with HWTEST_TX=1)
#   soak   repeats a short mixed workload for SOAK_MINUTES (default 90,
#          i.e. longer than any IP evaluation time-out)
#
# Unless HWTEST_TX=1 (every bladeRF TX port cabled through >= 20 dB or
# terminated, no antennas, TX gain capped; see README.md), the suite is
# receive-only and antennas may be attached: tests that enable a TX channel
# are left out (the no-TX guard would refuse them anyway), test_ctrl runs
# case by case without its enable_module and threads cases, and the rf phase
# is skipped. Retunes are then exercised with RX retunes scheduled on
# timestamps (test_scheduled_retune), which also runs in TX mode.
#
# Environment: HOST_BUILD selects the libbladeRF build under test (default
# $HWTEST_HOME/host-build); HWTEST_SKIP=<test>,<test>,... leaves those tests
# out (logged, not in summary.jsonl); HWTEST_FIXES=1 says HOST_BUILD has the
# libbladeRF fixes on branch fixes and also runs what fails with every image
# without them (see README.md): two-channel receive in FPGA tuning mode,
# test_fpga_load in host tuning mode and, with HWTEST_TX=1, TX on/off during a
# two-channel receive. HWTEST_FPGA_MIMO=1 is its older name.
#
# Results: $RESULTS_DIR/<label>/<time>/ with one log per test, summary.jsonl
# ({"test", "rc", "seconds"}), rf.jsonl and meta.json. Compare runs with
# compare.py. Only test programs on the allowlist below are ever run; the two
# flash/bootloader tests (test_flash_validate, test_bootloader_recovery) are
# deliberately absent. Afterwards the official image is reloaded.
set -uo pipefail
. "$(dirname "$0")/../lib/common.sh"

[ $# -ge 2 ] || { sed -n '2,40p' "$0"; exit 2; }
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
FIXES=${HWTEST_FIXES:-${HWTEST_FPGA_MIMO:-0}}
fixes_ok() { [ "$FIXES" = 1 ]; }
if tx_ok; then
    require_tx_cap
    log "TX allowed (HWTEST_TX=1, bladeRF TX gain capped at 60 dB): every bladeRF TX port must be cabled through >= 20 dB or terminated, no antennas"
else
    require_no_tx_guard
    log "receive-only (HWTEST_TX is not 1): no TX channel is enabled; TX tests are left out"
fi

cat > "$out/meta.json" <<EOF
{"label": "$label", "image": "$image", "image_md5": "$(md5_of "$image")",
 "phases": "${phases[*]}", "tx": $(tx_ok && echo true || echo false),
 "start": "$(date -Is)", "host": "$(hostname)",
 "host_sw": "$(git -C "$HWTEST_DIR/.." describe --always --dirty 2>/dev/null)",
 "host_build": "$HOST_BUILD", "skip": "${HWTEST_SKIP:-}", "fixes": "$FIXES"}
EOF

# Output checkers for tests whose exit status does not reflect data errors
check_tuning_fpga() {   # libbladeRF falls back to host tuning if the Nios cannot
    grep -q "Tuning Mode: FPGA" "$1"
}
check_rx2_complete() {  # rx2_txtoggle's receive got all its samples, without errors
    ! grep -Eq "rror|imed out" "$1" &&
        [ "$(stat -c %s "$out/scratch/rx2.bin" 2>/dev/null || echo 0)" -eq $((RX2_N * 4)) ]
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
    if [[ ",${HWTEST_SKIP:-}," == *",$name,"* ]]; then
        log "--- $name: skipped (HWTEST_SKIP)"
        return 0
    fi
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
        if ! usb_present "$BLADERF2_USB_ID" && ! usb_present "$FX3_BOOTLOADER_USB_ID"; then
            # The board has dropped off USB (seen once, during ctrl_threads-fpga):
            # nothing after this can pass, and only a replug brings it back.
            log "bladeRF is no longer on USB; stopping. Replug it."
            exit 2
        fi
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

# Two-channel receive (3 s on RX1+RX2 at 5 Msps) while TX1 sends 0.2 s of
# zero samples into its load: the receive must get all its samples. With
# Nuand's libbladeRF the end of the transmit switches both RX channels off in
# host tuning mode, and FPGA tuning mode cannot receive on both channels at
# all (README.md), so this needs HWTEST_FIXES=1 (and HWTEST_TX=1).
RX2_N=30000000      # bladeRF-cli counts the samples of both channels
rx2_txtoggle() {    # rx2_txtoggle <test name>
    local tx="$out/scratch/tx-zeros.bin"
    head -c $((1000000 * 4)) /dev/zero > "$tx"
    rm -f "$out/scratch/rx2.bin"
    runtest "$1" 120 @check_rx2_complete "$BCLI" \
        -e "set frequency rx 915M" -e "set frequency tx 920M" \
        -e "set samplerate rx 5M" -e "set samplerate tx 5M" -e "set gain tx1 0" \
        -e "rx config file=$out/scratch/rx2.bin format=bin n=$RX2_N channel=1,2 timeout=3000" \
        -e "tx config file=$tx format=bin repeat=1 channel=1" \
        -e "rx start" -e "tx start" -e "tx wait 5s" -e "rx wait 15s"
    local rc=$?
    rm -f "$tx" "$out/scratch/rx2.bin"
    return $rc
}

# test_ctrl cases, run one by one with -f (its quick variants; the deep phase
# runs the full frequency, bandwidth and sample rate sweeps). In receive-only
# mode enable_module and threads are left out (they enable TX channels);
# xb200 needs an XB-200. loopback only runs in host tuning mode: it sets the
# RFIC BIST loopback, which libbladeRF only supports in host tuning mode.
# threads only runs in FPGA tuning mode: test_ctrl itself refuses host tuning
# mode on the bladeRF 2.0 ("bug #705").
ctrl_cases() {      # ctrl_cases <tuning mode>
    local c
    for c in sampling lpf_mode enable_module loopback rx_mux correction \
             samplerate bandwidth gain frequency threads; do
        case "$c" in
            enable_module) tx_ok || continue ;;
            threads) tx_ok && [ "$1" = fpga ] || continue ;;
            loopback) [ "$1" = host ] || continue ;;
        esac
        echo "$c"
    done
}
CTRL_FAIL_RE="FAILED|Failed|failure|Invalid test"

# Tests that pass in either tuning mode. Fail regexes from each test's output.
common_tests() {
    local m=$1 c
    for c in $(ctrl_cases "$m"); do
        # threads hung until the 600 s time-out when it failed
        runtest "ctrl_$c-$m" "$([ "$c" = threads ] && echo 300 || echo 600)" \
            "$CTRL_FAIL_RE" "$T/libbladeRF_test_ctrl" -T "$m" -t "$c" -f
    done
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
    # Receive with metadata and the FPGA's sample counter, in 16-bit and 8-bit
    # sample formats, rx_meta also on both RX channels (the receive-only
    # combinations of Nuand's test_regression). rx_meta reports overruns but
    # still exits 0. Both RX channels only in host tuning mode unless
    # HWTEST_FIXES=1: with Nuand's libbladeRF that fails in FPGA tuning
    # mode with every image (see README.md).
    runtest "rx_meta-$m" 300 "Overrun detected" "$T/libbladeRF_test_rx_meta"
    runtest "rx_meta_8bit-$m" 300 "Overrun detected" "$T/libbladeRF_test_rx_meta" -b 8
    if [ "$m" = host ] || fixes_ok; then
        runtest "rx_meta_mimo-$m" 300 "Overrun detected" "$T/libbladeRF_test_rx_meta" -m
        runtest "rx_meta_8bit_mimo-$m" 300 "Overrun detected" "$T/libbladeRF_test_rx_meta" -b 8 -m
    fi
    runtest "rx_discont-$m" 300 "Expected.*Got" "$T/libbladeRF_test_rx_discont" -i 2000
    runtest "rx_discont_8bit-$m" 300 "Expected.*Got" "$T/libbladeRF_test_rx_discont" -i 2000 -b 8
    runtest "timestamps_rx_gaps-$m" 300 "[1-9][0-9]* test cases failed" \
        "$T/libbladeRF_test_timestamps" -t rx_gaps -f
    runtest "timestamps_rx_scheduled-$m" 300 "Test failed" \
        "$T/libbladeRF_test_timestamps" -t rx_scheduled -f
    if tx_ok; then
        runtest "timestamps_tx_onoff_nowsched-$m" 300 "Test failed|failed" \
            "$T/libbladeRF_test_timestamps" -t tx_onoff_nowsched -f
        runtest "digital_loopback_fw-$m" 300 @check_iq_matches "$T/libbladeRF_test_digital_loopback" -l fw -c 100
        runtest "digital_loopback_fpga-$m" 300 @check_iq_matches "$T/libbladeRF_test_digital_loopback" -l fpga -c 100
        if fixes_ok; then
            rx2_txtoggle "rx2_txtoggle-$m"
        fi
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
        # fpga_load also runs in the fpga phase. In host tuning mode it fails
        # with every image, including the official one, unless HOST_BUILD has
        # the fixes: Nuand's libbladeRF keeps its host-side AD9361 state
        # across an FPGA reload, with the AD9361 held in reset (README.md)
        if fixes_ok; then
            runtest fpga_load-host 300 - "$T/libbladeRF_test_fpga_load" -d "*" -f "$image"
        fi
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
        # Loads the image through libbladeRF on an open device, twice, and
        # sets the sample rate through the Nios
        runtest fpga_load 300 - "$T/libbladeRF_test_fpga_load" -d "*" -f "$image"
        unset BLADERF_DEFAULT_TUNING_MODE
        ;;
    deep)
        # With the official image: frequency ~8 min, bandwidth ~11 min,
        # sample rate ~35 min. The time-outs only catch hangs.
        load_image deep
        export BLADERF_DEFAULT_TUNING_MODE=fpga
        runtest tuning_mode-deep 60 @check_tuning_fpga "$BCLI" -e "print tuning_mode"
        runtest ctrl_frequency_full-fpga 1800 "$CTRL_FAIL_RE" "$T/libbladeRF_test_ctrl" -T fpga -t frequency
        runtest ctrl_bandwidth_full-fpga 2400 "$CTRL_FAIL_RE" "$T/libbladeRF_test_ctrl" -T fpga -t bandwidth
        runtest ctrl_samplerate_full-fpga 6000 "$CTRL_FAIL_RE" "$T/libbladeRF_test_ctrl" -T fpga -t samplerate
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
            BLADERF_DEFAULT_TUNING_MODE=$m runtest "soak-$i-ctrl_frequency-$m" 300 "$CTRL_FAIL_RE" \
                "$T/libbladeRF_test_ctrl" -T "$m" -t frequency -f -s "$i"
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

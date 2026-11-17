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
#   twoch  twoch-check.py against the B200mini through a 2-way splitter:
#          RX1 and RX2 at once (HWTEST_2CH=rx), TX1 and TX2 at once
#          (HWTEST_2CH=tx), or both, in both tuning modes (transmits: only
#          with HWTEST_TX=1; skipped without HWTEST_2CH; two-channel receive
#          in FPGA tuning mode only with HWTEST_FIXES=1)
#   soak   repeats a short mixed workload for SOAK_MINUTES (default 90,
#          i.e. longer than any IP evaluation time-out)
#   formats  every sample format (SC16, SC16 meta, SC8, SC8 meta, packed),
#          one and two channels, at full rate (see below), with every sample
#          checked: RX against the FPGA's sample counter, TX against the
#          B200mini, and TX+RX through the FPGA's digital loopback (SC16, SC8;
#          HWTEST_TX=1 for the last two)
#   oc     the AD9361 overclocked (BLADERF_FEATURE_OVERSAMPLE, 122.88 Msps,
#          8-bit): Nuand's test_oversample on both RX channels, the counter
#          check, and pulses both ways against the B200mini (HWTEST_TX=1)
#   trigger  one bladeRF as trigger master (J51-1): the RX trigger holds the
#          samples back until it fires
#   fullsoak  the counter check at 61.44 Msps for FULLSOAK_RX_MIN minutes
#          (default 60), then the loopback check at 30.72 Msps for
#          FULLSOAK_LOOP_MIN minutes (default 30)
#   sched  a recorder's scheduled 100 ms bursts (timestamps), every sample
#          checked against the FPGA's counter (SCHED_S seconds per case)
#   bist   the AD9361's built-in PRBS at its RX data port, every sample
#          checked: the AD9361-to-FPGA LVDS interface alone, at full rate
#          (BIST_MIN minutes per case, default 2) and overclocked
#
# Unless HWTEST_TX=1 (every bladeRF TX port cabled through >= 20 dB or
# terminated, no antennas, TX gain capped; see README.md), the suite is
# receive-only and antennas may be attached: tests that enable a TX channel
# are left out (the no-TX guard would refuse them anyway), test_ctrl runs
# case by case without its enable_module and threads cases, and the rf and
# twoch phases are skipped. Retunes are then exercised with RX retunes
# scheduled on timestamps (test_scheduled_retune), which also runs in TX mode.
#
# Environment: HWTEST_FPGA=A5|A9 is the FPGA variant of the board (default A5,
# the xA5; lib/fpga-variant.sh): the run stops unless the board and the image
# are that variant. HOST_BUILD selects the libbladeRF build under test (default
# $HWTEST_HOME/host-build); HWTEST_SKIP=<test>,<test>,... leaves those tests
# out (logged, not in summary.jsonl); HWTEST_FIXES=1 says HOST_BUILD has the
# libbladeRF fixes of branch host-fixes and also runs what fails with every
# image without them (see README.md): two-channel receive in FPGA tuning mode,
# test_fpga_load in host tuning mode and, with HWTEST_TX=1, TX on/off during a
# two-channel receive. HWTEST_FPGA_MIMO=1 is its older name. HWTEST_RF_CH=1
# says channel 1 (RX1, TX1) faces the B200mini instead of channel 2 (the
# default), with channel 2's ports terminated: the RF phases then test
# channel 1 (README.md, "Cabling for TX runs"). HWTEST_2CH=rx|tx|both says
# which two-channel setup is cabled (the same section): the twoch phase runs
# twoch-check.py rx, tx or both; unset, it skips the phase.
#
# Results: $RESULTS_DIR/<label>/<time>/ with one log per test, summary.jsonl
# ({"test", "rc", "seconds"}), rf.jsonl, twoch.jsonl and meta.json. Compare
# runs with compare.py. Only test programs on the allowlist below are ever
# run; the two flash/bootloader tests (test_flash_validate,
# test_bootloader_recovery) are deliberately absent. Afterwards the official
# image is reloaded.
set -uo pipefail
. "$(dirname "$0")/../lib/common.sh"

[ $# -ge 2 ] || { awk 'NR > 1 && /^set -uo/ { exit } NR > 1' "$0"; exit 2; }
image=$(readlink -f "$1"); label=$2; shift 2
phases=("$@"); [ ${#phases[@]} -gt 0 ] || phases=(smoke host fpga rf)
[ -f "$image" ] || die "no such image: $image"
check_good_images
check_search_path
check_image_variant "$image"
check_fpga_variant

out="$RESULTS_DIR/$label/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out/logs"
T="$HOST_BUILD/output"
mkdir -p "$out/scratch"     # working directory of tests that write files
HOPS="$HWTEST_DIR/lib/hops-rx.txt"
exec > >(tee -a "$out/run.log") 2>&1

tx_ok() { [ "$HWTEST_TX" = 1 ]; }
FIXES=${HWTEST_FIXES:-${HWTEST_FPGA_MIMO:-0}}
fixes_ok() { [ "$FIXES" = 1 ]; }
RF_CH=${HWTEST_RF_CH:-2}
case "$RF_CH" in 1|2) ;; *) die "HWTEST_RF_CH must be 1 or 2, not $RF_CH" ;; esac
export HWTEST_RF_CH=$RF_CH
TWOCH=${HWTEST_2CH:-}
case "$TWOCH" in ""|rx|tx|both) ;; *) die "HWTEST_2CH must be rx, tx or both (or unset), not $TWOCH" ;; esac
if tx_ok; then
    require_tx_cap
    log "TX allowed (HWTEST_TX=1, bladeRF TX gain capped at 60 dB): every bladeRF TX port must be cabled through >= 20 dB or terminated, no antennas"
    log "RF tests use channel $RF_CH (HWTEST_RF_CH): RX$RF_CH and TX$RF_CH face the B200mini, the other channel's ports are terminated"
    [ -z "$TWOCH" ] || log "two-channel setup cabled: $TWOCH (HWTEST_2CH; README.md, \"Cabling for TX runs\")"
else
    require_no_tx_guard
    log "receive-only (HWTEST_TX is not 1): no TX channel is enabled; TX tests are left out"
fi

cat > "$out/meta.json" <<EOF
{"label": "$label", "image": "$image", "image_md5": "$(md5_of "$image")",
 "phases": "${phases[*]}", "tx": $(tx_ok && echo true || echo false), "rf_ch": $RF_CH, "twoch": "$TWOCH",
 "fpga": "$HWTEST_FPGA", "serial": "$DEVICE_SERIAL",
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
# It runs last in its phase, after an image load, and sets the RX mux and
# the loopback first: the FPGA keeps its RX mux setting after the program
# that set it exits (libbladeRF's open does not reset it), and
# digital_loopback_fpga leaves it at DIGITAL_LOOPBACK, which feeds the TX
# samples to the receive. And a transmit by another program makes the next
# digital_loopback_rfic fail (README.md). bladeRF-cli exits 0 even when the
# receive times out, hence the checker.
RX2_N=30000000      # bladeRF-cli counts the samples of both channels
rx2_txtoggle() {    # rx2_txtoggle <test name>
    local tx="$out/scratch/tx-zeros.bin"
    head -c $((1000000 * 4)) /dev/zero > "$tx"
    rm -f "$out/scratch/rx2.bin"
    runtest "$1" 120 @check_rx2_complete "$BCLI" \
        -e "set loopback none" -e "set rx_mux baseband" \
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
        if tx_ok && fixes_ok; then
            rx2_txtoggle rx2_txtoggle-host
        fi
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
        if tx_ok && fixes_ok; then
            rx2_txtoggle rx2_txtoggle-fpga
        fi
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
    twoch)
        # Both RX channels, or both TX channels, at once through a 2-way
        # splitter (twoch-check.py; README.md, "Cabling for TX runs"):
        # HWTEST_2CH says which setup is cabled. Two-channel receive in FPGA
        # tuning mode only with HWTEST_FIXES=1, as in the formats phase:
        # Nuand's libbladeRF cannot receive on both channels there.
        if ! tx_ok; then
            log "twoch phase skipped: it transmits (needs HWTEST_TX=1 and the cabling in README.md)"
            continue
        fi
        if [ -z "$TWOCH" ]; then
            log "twoch phase skipped: HWTEST_2CH is not set (rx, tx or both: the two-channel setup that is cabled, see README.md)"
            continue
        fi
        load_image twoch
        for m in host fpga; do
            for d in rx tx; do
                [ "$TWOCH" = both ] || [ "$TWOCH" = "$d" ] || continue
                if [ "$d" = rx ] && [ "$m" = fpga ] && ! fixes_ok; then
                    log "--- twoch_rx-fpga: left out (two-channel receive in FPGA tuning mode needs HWTEST_FIXES=1)"
                    continue
                fi
                BLADERF_DEFAULT_TUNING_MODE=$m runtest "twoch_$d-$m" 1800 - \
                    python3 "$HWTEST_DIR/bin/twoch-check.py" "$d" --out "$out/twoch-$m.jsonl"
            done
        done
        cat "$out"/twoch-*.jsonl > "$out/twoch.jsonl" 2>/dev/null
        ;;
    formats)
        # Full rate: the highest rate at which a format fits the USB 3 load
        # of Nuand's usual maximum, one channel of SC16 at 61.44 Msps
        # (245.76 MB/s), but at most the AD9361's 61.44 Msps. (libbladeRF
        # warns above 80 Msps in total over both channels: SC8 and packed
        # with two channels exceed that.) Two channels only in host tuning
        # mode unless HWTEST_FIXES=1 (Nuand's libbladeRF cannot receive on
        # both channels in FPGA tuning mode, see README.md).
        #  - fmt_rx: every sample from the FPGA's counter checked
        #    (hwtest-stream rxcheck; after 0.25 s of warm-up, see rx_order)
        #  - fmt_loop: TX through the FPGA's digital loopback back to RX,
        #    every sample checked; SC16 and SC8 only (libbladeRF's
        #    asynchronous interface), at 4 Msps (the loopback is not paced
        #    by the sample clock and overloads USB at high rates) and, with
        #    one channel, at 30.72 (SC16) and 40.96 Msps (SC8)
        #  - fmt_txrf: TX at full rate, the B200mini checks the tone
        #  - rx_order: libbladeRF's start-up reordering, 10 x 1 s with no
        #    warm-up (informational: counts in the log, never fails)
        load_image formats
        for m in host fpga; do
            export BLADERF_DEFAULT_TUNING_MODE=$m
            for spec in "sc16 x1 61.44M" "sc16meta x1 61.44M" "sc8 x1 61.44M" \
                        "sc8meta x1 61.44M" "packed x1 61.44M" \
                        "sc16 x2 30.72M" "sc16meta x2 30.72M" "sc8 x2 61.44M" \
                        "sc8meta x2 61.44M" "packed x2 40.96M"; do
                read -r f l full <<< "$spec"
                if [ "$l" = x2 ] && [ "$m" = fpga ] && ! fixes_ok; then
                    continue
                fi
                runtest "fmt_rx_${f}_$l-$m" 120 - "$T/hwtest-stream" rxcheck -f "$f" -l "$l" -r "$full" -t 20
                if tx_ok; then
                    runtest "fmt_txrf_${f}_$l-$m" 300 - python3 "$HWTEST_DIR/bin/pulse-check.py" \
                        tx-fmt --format "$f" --layout "$l" --rate "${full%M}e6" --out "$out/fmt-$m.jsonl"
                fi
            done
            if tx_ok; then
                for spec in "sc16 x1 4M" "sc8 x1 4M" "sc16 x2 4M" "sc8 x2 4M" \
                            "sc16 x1 30.72M" "sc8 x1 40.96M"; do
                    read -r f l r <<< "$spec"
                    if [ "$l" = x2 ] && [ "$m" = fpga ] && ! fixes_ok; then
                        continue
                    fi
                    runtest "fmt_loop_${f}_${l}_${r%M}M-$m" 120 - "$T/hwtest-stream" loopback -f "$f" -l "$l" -r "$r" -t 10
                done
            fi
        done
        export BLADERF_DEFAULT_TUNING_MODE=host
        runtest rx_order-host 300 - bash -c 'for i in 1 2 3 4 5 6 7 8 9 10; do
            "$0" rxcheck -f sc16 -r 61.44M -t 1 -W 0 | grep -E "coverage|RESULT"; done; true' "$T/hwtest-stream"
        unset BLADERF_DEFAULT_TUNING_MODE
        cat "$out"/fmt-*.jsonl > "$out/fmt.jsonl" 2>/dev/null
        ;;
    oc)
        # The overclock setting stays in the AD9361 after the programs exit:
        # reload the image afterwards.
        load_image oc
        for m in host fpga; do
            export BLADERF_DEFAULT_TUNING_MODE=$m
            runtest "oc_oversample_rx1-$m" 120 - "$T/libbladeRF_test_oversample" -c 1
            runtest "oc_oversample_rx2-$m" 120 - "$T/libbladeRF_test_oversample" -c 2
            runtest "oc_rx_sc8-$m" 120 - "$T/hwtest-stream" rxcheck -f sc8 -O -r 122.88M -t 20
            runtest "oc_rx_sc8meta-$m" 120 - "$T/hwtest-stream" rxcheck -f sc8meta -O -r 122.88M -t 20
            if tx_ok; then
                runtest "oc_pulse_rx-$m" 900 - python3 "$HWTEST_DIR/bin/pulse-check.py" oc-rx --out "$out/oc-$m.jsonl"
                runtest "oc_pulse_tx-$m" 900 - python3 "$HWTEST_DIR/bin/pulse-check.py" oc-tx --out "$out/oc-$m.jsonl"
                runtest "oc_pulse_tx_meta-$m" 900 - python3 "$HWTEST_DIR/bin/pulse-check.py" oc-tx \
                    --oc-format sc8meta --out "$out/oc-$m.jsonl"
            fi
        done
        unset BLADERF_DEFAULT_TUNING_MODE
        cat "$out"/oc-*.jsonl > "$out/oc.jsonl" 2>/dev/null
        load_image after-oc
        ;;
    trigger)
        # The trigger gates ADC samples (the counter modes bypass it)
        load_image trigger
        for m in host fpga; do
            BLADERF_DEFAULT_TUNING_MODE=$m runtest "trigger_rx-$m" 60 - "$T/hwtest-stream" trigger rx -f sc16meta -r 10M
        done
        ;;
    fullsoak)
        load_image fullsoak
        export BLADERF_DEFAULT_TUNING_MODE=host
        rxs=$((${FULLSOAK_RX_MIN:-60} * 60)); loops=$((${FULLSOAK_LOOP_MIN:-30} * 60))
        runtest fullsoak_rx_sc16-host $((rxs + 300)) - "$T/hwtest-stream" rxcheck -f sc16 -r 61.44M -t "$rxs"
        if tx_ok; then
            runtest fullsoak_loop_sc16-host $((loops + 300)) - "$T/hwtest-stream" loopback -f sc16 -r 30.72M -t "$loops"
        fi
        unset BLADERF_DEFAULT_TUNING_MODE
        ;;
    sched)
        # A recorder's pattern of scheduled bursts (hwtest-stream rxsched):
        # 100 ms bursts, each scheduled 5 ms after the current RX timestamp
        # and written to a file before the next one is scheduled, with the
        # FPGA's counter as the source: every burst must start at its
        # timestamp, arrive complete and have every sample in place. The
        # stream runs on between bursts, so the FPGA overruns there, as it
        # does for such a recorder. Host tuning mode, SCHED_S seconds per
        # case (default 60), the full rate of each format and layout.
        load_image sched
        export BLADERF_DEFAULT_TUNING_MODE=host
        ss=${SCHED_S:-60}
        for spec in "sc16meta x1 61.44M" "sc8meta x1 61.44M" "sc16meta x2 30.72M" "sc8meta x2 61.44M"; do
            read -r f l r <<< "$spec"
            runtest "sched_${f}_$l-host" $((ss + 120)) - "$T/hwtest-stream" rxsched -f "$f" -l "$l" -r "$r" \
                -t "$ss" -o "$out/scratch/sched.bin"
        done
        rm -f "$out/scratch/sched.bin"
        unset BLADERF_DEFAULT_TUNING_MODE
        ;;
    bist)
        # The AD9361 replaces its received samples with its BIST pattern at
        # the RX data port (hwtest-stream bist), so every sample crosses the
        # AD9361-to-FPGA LVDS interface with a known value: the xA5's one
        # timing path with negative setup slack (Nuand's image -0.154 ns,
        # the Nios V build -0.110 ns; for the xA9 the Nios V build meets
        # timing there) is the capture of that interface. The
        # AD9361 sends both channels over it whichever are streamed (2R2T
        # framing: four DATA_CLK cycles per sample, see
        # axi_ad9361_dev_if_alt.v), so its clock is 4 x the sample rate.
        # Host tuning mode only (in FPGA tuning mode the AD9361's state
        # outlives the program); the image is reloaded afterwards.
        #  - bist_prbs: the PRBS at 61.44 Msps (interface clock 245.76 MHz)
        #    on each channel (SC16) and on both (SC16 at 30.72 Msps, the most
        #    two channels of SC16 carry over USB; SC8 at 61.44 Msps)
        #  - bist_tone: the BIST tone instead (a different bit pattern)
        #  - bist_oc: overclocked (BLADERF_FEATURE_OVERSAMPLE, SC8) at
        #    76.8 Msps (307.2 MHz) on each channel, the edge of what the
        #    interface carries (errors in one bit with every image), and at
        #    122.88 Msps (491.52 MHz), garbage with every image, Nuand's
        #    included: these cases fail everywhere; they measure the margin,
        #    not a regression (README.md, bist)
        load_image bist
        export BLADERF_DEFAULT_TUNING_MODE=host
        bs=$((${BIST_MIN:-2} * 60))
        runtest bist_prbs_sc16_rx1-host $((bs + 120)) - "$T/hwtest-stream" bist -f sc16 -c 1 -r 61.44M -t "$bs"
        runtest bist_prbs_sc16_rx2-host $((bs + 120)) - "$T/hwtest-stream" bist -f sc16 -c 2 -r 61.44M -t "$bs"
        runtest bist_prbs_sc16_x2-host $((bs + 120)) - "$T/hwtest-stream" bist -f sc16 -l x2 -r 30.72M -t "$bs"
        runtest bist_prbs_sc8_x2-host 120 - "$T/hwtest-stream" bist -f sc8 -l x2 -r 61.44M -t 20
        runtest bist_tone_sc16_rx1-host 120 - "$T/hwtest-stream" bist -f sc16 -c 1 -r 61.44M -B tone -t 20
        for c in 1 2; do
            runtest "bist_oc_76M8_sc8_rx$c-host" 120 - "$T/hwtest-stream" bist -f sc8 -c "$c" -O -r 76.8M -t 20
        done
        runtest bist_oc_122M88_sc8_rx1-host 120 - "$T/hwtest-stream" bist -f sc8 -c 1 -O -r 122.88M -t 20
        unset BLADERF_DEFAULT_TUNING_MODE
        load_image after-bist
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

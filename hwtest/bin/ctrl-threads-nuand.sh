#!/bin/bash
# ctrl-threads-nuand.sh: test_ctrl's threads case run the way Nuand's release checklist
# runs it (doc/development/test_checklist.md: bladeRF2, FPGA-based tuning): Nuand's test program,
# libbladeRF from $HOST_BUILD (default: the main build, Nuand's libbladeRF), NO harness preload (no
# TX cap, no no-TX guard) and no harness time limit; only a watchdog against a real hang
# (WATCHDOG seconds, default 2700). FAST=1 (default) adds -f, the fast variant; FAST=0 runs the
# full test as the checklist does. IMAGES (default "C A"; A, B or C, from images/, or images-xA9/ with
# HWTEST_FPGA=A9 for an xA9: lib/fpga-variant.sh; the board must be that variant). TRACE=1 preloads lib/nios-trace.c
# (observation only: it logs every Nios packet, frequency and bandwidth call and failed sync call
# with its timing, and how the sample transfers complete, and changes nothing) and analyzes the
# trace with nios-trace-threads.py. The shim flushes its log every second, so a run the watchdog
# stops loses about a second of it; a full run's trace takes about 2 GB.
#
# Without the TX cap the bladeRF may transmit at full power (+8 dBm), so this needs every bladeRF TX
# port into a 50 ohm load or through at least 40 dB to any receiver (the B200mini's inputs take
# -15 dBm at most; the transmit cabling's 20 dB is not enough), and HWTEST_TX_UNCAPPED_OK=1 to
# confirm it. As run on 2026-11-15: TX1 -> 20 dB -> 20 dB -> B200mini RX2, TX2/RX1/RX2 on loads.
# The test streams through the FX3's firmware loopback. After every run: still on USB? If not,
# stop (exit 3) for a power cycle. Then recover.sh loads the official image.
set -u
H="$(cd "$(dirname "$0")/.." && pwd)"
if [ "${HWTEST_TX_UNCAPPED_OK:-}" != 1 ]; then
    echo "ctrl-threads-nuand.sh transmits without the TX cap: set HWTEST_TX_UNCAPPED_OK=1 only with" >&2
    echo "every bladeRF TX port into a 50 ohm load or through >= 40 dB to any receiver" >&2
    exit 2
fi
HOST_BUILD=${HOST_BUILD:-$HOME/workarea/hwtest-data/host-build}
export HOST_BUILD
T=$HOST_BUILD/output
. "$H/lib/fpga-variant.sh"      # HWTEST_FPGA (A5, the default, or A9) -> IMAGES_DIR
IMG=$IMAGES_DIR
OUT=~/workarea/hwtest-data/results/ctrlthreads-nuand${LABEL:+-$LABEL}-$(date +%m%d-%H%M)
mkdir -p $OUT
args="-T fpga -t threads"
[ "${FAST:-1}" = 1 ] && args="$args -f"
SO=
if [ "${TRACE:-0}" = 1 ]; then
    SO=$T/libhwtest-nios-trace.so
    cc -shared -fPIC -O2 -Wall -Wextra -Werror \
        -I"$H/../host/libraries/libbladeRF/include" -I/usr/include/libusb-1.0 \
        -o "$SO" "$H/lib/nios-trace.c" -ldl || { echo "cannot build the trace shim"; exit 2; }
fi
echo "libbladeRF_test_ctrl $args; host build $HOST_BUILD ($(ldd $T/libbladeRF_test_ctrl | grep -o '/[^ ]*libbladeRF.so[^ ]*' | head -1)); LD_PRELOAD='${LD_PRELOAD:-}'${SO:+; traced}; out $OUT"
img() {
    case "$1" in
        A) echo $IMG/A-official-v0.16.0.rbf ;;
        B) echo $IMG/B-nios2-q23.1.1.rbf ;;
        C) echo $IMG/C-niosv-q25.1.rbf ;;
        *) echo "unknown image $1" >&2; exit 2 ;;
    esac
}
on_usb() { lsusb -d 2cf0:5250 > /dev/null 2>&1; }
for i in ${IMAGES:-C A}; do
    f=$(img $i) || exit 2
    rbf_size_ok "$f" || { echo "$f is missing or not an $HWTEST_FPGA image (HWTEST_FPGA)"; exit 2; }
done
kle=$(device_info "$H/bin/bladerf-cli-safe" | info_fpga_kle)
[ "$kle" = "$FPGA_KLE" ] || {
    echo "the bladeRF's FPGA is ${kle:-not readable} (KLE), not the $FPGA_KLE KLE of HWTEST_FPGA=$HWTEST_FPGA"; exit 2; }
for i in ${IMAGES:-C A}; do
    on_usb || { echo "[$(date +%T)] bladeRF is not on USB before $i: stopping"; exit 3; }
    $H/bin/bladerf-cli-safe -l "$(img $i)" > $OUT/load-$i.log 2>&1 || { echo "load $i failed"; exit 2; }
    log=$OUT/ctrl_threads-fpga-$i.log
    t0=$SECONDS
    if [ -n "$SO" ]; then
        env -u LD_PRELOAD NIOS_TRACE_FILE="${log%.log}.trace" LD_PRELOAD="$SO" \
            timeout ${WATCHDOG:-2700} "$T/libbladeRF_test_ctrl" $args > $log 2>&1
    else
        env -u LD_PRELOAD timeout ${WATCHDOG:-2700} "$T/libbladeRF_test_ctrl" $args > $log 2>&1
    fi
    rc=$?
    verdict=PASS
    [ $rc -ne 0 ] && verdict=FAIL
    [ $rc -eq 124 ] && verdict="HUNG (watchdog at ${WATCHDOG:-2700} s)"
    echo "[$(date +%T)] $i: $verdict rc=$rc, $((SECONDS - t0)) s; joined: $(grep -c 'Joined' $log); summary: $(grep -E '^ *threads [0-9]+$' $log | tr -s ' ')"
    echo "    errors: $(grep -c -E 'ERROR' $log), gain readback $(grep -c 'Erroneous overall gain' $log), rfic time-outs $(grep -c -E '_rfic_cmd_(read|write).*timed out' $log), stream failures $(grep -c -E '(RX|TX) failed' $log)"
    if [ -n "$SO" ] && [ -s "${log%.log}.trace" ]; then
        python3 -I "$H/bin/nios-trace-threads.py" "${log%.log}.trace" 8 > "${log%.log}.analysis.txt" 2>&1
        sed -n '2,20p' "${log%.log}.analysis.txt" | grep -v '^$' | sed -n '1,16p' | sed 's/^/    /'
    fi
    sleep 3
    if ! on_usb; then
        echo "[$(date +%T)] bladeRF DROPPED OFF USB after $i: stopping, power-cycle it"
        exit 3
    fi
done
$H/bin/recover.sh > $OUT/recover.log 2>&1
echo "recover rc=$?"
echo "out: $OUT"

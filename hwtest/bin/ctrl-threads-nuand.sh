#!/bin/bash
# ctrl-threads-nuand.sh: test_ctrl's threads case run the way Nuand's release checklist
# runs it (doc/development/test_checklist.md: bladeRF2, FPGA-based tuning): Nuand's test program,
# libbladeRF from $HOST_BUILD (default: the main build, Nuand's libbladeRF), NO harness preload (no
# TX cap, no no-TX guard) and no harness time limit; only a watchdog against a real hang
# (WATCHDOG seconds, default 2700). FAST=1 (default) adds -f, the fast variant; FAST=0 runs the
# full test as the checklist does. IMAGES (default "C A").
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
IMG=~/workarea/hwtest-data/images
OUT=~/workarea/hwtest-data/results/ctrlthreads-nuand${LABEL:+-$LABEL}-$(date +%m%d-%H%M)
mkdir -p $OUT
args="-T fpga -t threads"
[ "${FAST:-1}" = 1 ] && args="$args -f"
echo "libbladeRF_test_ctrl $args; host build $HOST_BUILD ($(ldd $T/libbladeRF_test_ctrl | grep -o '/[^ ]*libbladeRF.so[^ ]*' | head -1)); LD_PRELOAD='${LD_PRELOAD:-}'; out $OUT"
img() { [ "$1" = A ] && echo $IMG/A-official-v0.16.0.rbf || echo $IMG/C-niosv-q25.1.rbf; }
on_usb() { lsusb -d 2cf0:5250 > /dev/null 2>&1; }
for i in ${IMAGES:-C A}; do
    on_usb || { echo "[$(date +%T)] bladeRF is not on USB before $i: stopping"; exit 3; }
    $H/bin/bladerf-cli-safe -l "$(img $i)" > $OUT/load-$i.log 2>&1 || { echo "load $i failed"; exit 2; }
    log=$OUT/ctrl_threads-fpga-$i.log
    t0=$SECONDS
    env -u LD_PRELOAD timeout ${WATCHDOG:-2700} "$T/libbladeRF_test_ctrl" $args > $log 2>&1
    rc=$?
    verdict=PASS
    [ $rc -ne 0 ] && verdict=FAIL
    [ $rc -eq 124 ] && verdict="HUNG (watchdog at ${WATCHDOG:-2700} s)"
    echo "[$(date +%T)] $i: $verdict rc=$rc, $((SECONDS - t0)) s; joined: $(grep -c 'Joined' $log); summary: $(grep -E '^ *threads [0-9]+$' $log | tr -s ' ')"
    echo "    errors: $(grep -c -E 'ERROR' $log), gain readback $(grep -c 'Erroneous overall gain' $log), rfic time-outs $(grep -c -E '_rfic_cmd_(read|write).*timed out' $log), stream failures $(grep -c -E '(RX|TX) failed' $log)"
    sleep 3
    if ! on_usb; then
        echo "[$(date +%T)] bladeRF DROPPED OFF USB after $i: stopping, power-cycle it"
        exit 3
    fi
done
$H/bin/recover.sh > $OUT/recover.log 2>&1
echo "recover rc=$?"
echo "out: $OUT"

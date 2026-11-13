# bladeRF 2.0 micro hardware regression harness

Scripts for comparing FPGA images (e.g. Nios II vs Nios V builds) on a real
bladeRF 2.0 micro xA5, unattended, without any risk of bricking it.

## Safety rules

* The harness **never writes the bladeRF's SPI flash**. FPGA images are only
  loaded into the FPGA (volatile), so as long as the flash holds no FPGA
  autoload image (inventory.sh shows the FPGA unconfigured after power-up), a
  power cycle always returns the board to a clean state. The xA5 image fits
  in a 64 Mbit flash, so do not set up FPGA autoload (`bladeRF-cli -L`) on the
  test board.
* All bladeRF-cli use goes through `bin/bladerf-cli-safe`, which refuses
  flash-writing commands and options (`flash_image`, `flash_restore`,
  `flash_init_cal`, `erase`, `jump_to_boot`, `load fx3`, `-f`, `-L`, ...)
  and interactive mode.
* RF: nothing transmits unless `HWTEST_TX=1` says that every bladeRF TX port
  is cabled as below (at least 30 dB between every transmitter and receiver,
  no antennas) or terminated in 50 ohms. Without it the harness is
  receive-only and antennas may stay attached: a preloaded guard
  (`lib/no-tx-guard.c`) makes libbladeRF refuse to enable any TX channel for
  every program the harness runs, `bladerf-cli-safe` refuses `tx`/`trigger`,
  `rf-check.py` refuses to run, and `run-suite.sh` leaves out every test
  that enables TX. Bias-tees stay off either way.

## One-time setup (XPS 13)

    sudo bash hwtest/setup/sudo-setup.sh     # packages + udev rules
    hwtest/bin/build-host.sh                 # libbladeRF, bladeRF-cli, tests

Data (host build, backups, results) goes to `~/workarea/hwtest-data/`.

## Receive-only runs (default)

With antennas on the bladeRF, or anything else that is not the cabling below,
leave `HWTEST_TX` unset. `run-suite.sh` then runs the smoke, host, fpga and
soak phases without their TX tests (TX streaming, TX timestamps, TX quick
retune, digital loopbacks, test_ctrl's `enable_module` and `threads` cases)
and skips the rf phase. Retunes are covered by RX retunes scheduled on
timestamps with quick-tune profiles (`test_scheduled_retune`, 24 frequencies
from `lib/hops-rx.txt`), which also runs in TX mode. Compare receive-only runs
only with receive-only runs (`tx` in meta.json).

## Phases and test notes

`run-suite.sh <image.rbf> <label> [phase...]` runs the phases listed at the
top of the script. The smoke, host and fpga phases take about 10 minutes and
run test_ctrl's quick (`-f`) variants. The deep phase runs test_ctrl's full
frequency, bandwidth and sample rate sweeps with the AD9361 driver on the
Nios (FPGA tuning mode), about an hour with the official image; the soak
phase repeats a mixed workload for `SOAK_MINUTES`.

Things that fail with every image, including the official one, so the
harness does not run them that way:

* test_ctrl's `loopback` case in FPGA tuning mode: libbladeRF only supports
  the RFIC BIST loopback in host tuning mode.
* `test_fpga_load` in host tuning mode: libbladeRF keeps its host-side AD9361
  state across an FPGA reload on an open device, so the first sample rate
  change after the reload times out in the AD9361's RX baseband filter
  calibration (`Calibration TIMEOUT (0x16, 0x80)`). The fpga phase runs it in
  FPGA tuning mode, where the Nios starts from scratch after the reload.
* The full test_ctrl sweeps in host tuning mode take over 1.5 hours per image
  (the sample rate sweep alone about 50 minutes).
* `test_rx_meta -m` (both RX channels) in FPGA tuning mode: right after the
  second RX channel is enabled, the stream jumps ("Sample discontinuity
  detected"), overruns, and stops ("Transfer timed out for RX buffer", "RX
  "now" failed: Operation timed out"). It passes in host tuning mode, where
  the harness runs it.

A real libbladeRF bug the deep phase hits with the Nios II images:
test_ctrl's full bandwidth test in FPGA tuning mode fails now and then with
"Unexpected bandwidth. requested=X, actual=3, readback=X". Many bandwidth
changes keep the Nios II busy for 250-290 ms, longer than libbladeRF's 250 ms
time-out for a Nios reply (PERIPHERAL_TIMEOUT_MS): ~1,300 status polls time
out per run. Usually the Nios is still busy when the next poll arrives, and
it sends one reply for both polls. But when it finishes in the ~0.2 ms between
libbladeRF giving up on a poll and the next poll arriving, it answers both,
and from then on libbladeRF reads each reply as the answer to its next request
(it never checks a reply's address): the bandwidth read gets a status word (3).
With the Nios V image the longest wait for a reply was 242 ms, just inside the
time-out. `bin/bw-diag.sh <image labels>` reproduces it with every Nios packet
logged and timed (LD_PRELOAD shim `lib/nios-trace.c`). `lib/late-reply.c`
(LD_PRELOAD) reproduces it deterministically by delaying every Nth RFIC
status response, e.g. `LATE_REPLY_EVERY=20` with `test_ctrl -T fpga -t
bandwidth -f`: 17 failures without the fix on branch `fixes`, none with it.

Two fixes to `test_scheduled_retune`, which failed with the official image
too ("Requested timestamp is in the past"): it read the start timestamp
before the initial full tune, which can take longer (~60 ms) than its 20 ms
start-up delay, so it now tunes first; and with 16 RX buffers (8 in flight)
at 5 Msps the sync worker overran every few hundred iterations, so it now
uses 64. (libbladeRF has started RX streaming at `bladerf_enable_module`
since 712d49e2, so a few overruns at start-up are normal.)

`campaign.sh <plan>` runs a list of run-suite.sh runs unattended, one per
line (`<image.rbf> <label> [phase...] [SOAK_MINUTES=<n>]`), recovering the
board after each.

## Cabling for TX runs (`HWTEST_TX=1`)

    bladeRF TX1  -> 30 dB -> bladeRF RX1        self-loop
    bladeRF TX2  -> 30 dB -> B200mini RX2       bladeRF TX measured by the USRP
    B200mini TX/RX -> 30 dB -> bladeRF RX2      USRP tone measured by the bladeRF

At least 30 dB per link and no antennas anywhere: that keeps every receiver
at or below -15 dBm (Ettus's limit for the B200mini's RF inputs) even with a
transmitter at full power (bladeRF 2.0 micro +8 dBm, B200mini about +10 dBm).
One 20 dB pad is not enough; two in series (40 dB) are. Nothing measures the
TX1 -> RX1 loop yet, so a 50 ohm load on TX1 will do instead.

## Scripts

| Script | What it does |
| :----- | :----------- |
| `bin/build-host.sh` | Builds libbladeRF, bladeRF-cli, the test programs and the no-TX guard |
| `bin/inventory.sh` | First contact: versions, device info, full read-only flash backup |
| `bin/run-suite.sh` | Loads an image (volatile) and runs the test phases against it |
| `bin/campaign.sh` | Runs a plan file of run-suite.sh runs unattended |
| `bin/recover.sh` | Recovery ladder: reload the official image, then progressively stronger soft resets; asks for a replug only as a last resort |
| `bin/bladerf-reset.py` | FX3 reboot (`bladerf_device_reset`) or USB port reset, without talking to the FPGA |
| `bin/rf-check.py` | CW tone checks against the B200mini across bands and sample rates |
| `bin/compare.py` | Compares result directories and flags regressions |
| `bin/bw-diag.sh` | Runs the FPGA-mode bandwidth test with every Nios packet traced (`lib/nios-trace.c`) |
| `bin/nios-trace-analyze.py` | Flags Nios time-outs and replies that belong to another request in a trace |

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
  is cabled as below (at least 20 dB between every transmitter and receiver,
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

Settings, from the environment or per run in a campaign plan: `HOST_BUILD`
(the libbladeRF build under test, default `host-build`), `HWTEST_SKIP=<test>,...`
(left out, logged), `HWTEST_FIXES=1` (`HOST_BUILD` has the libbladeRF fixes on
branch `fixes`: also run the tests below that fail with every image without
them; `HWTEST_FPGA_MIMO=1` is its older name) and `SOAK_MINUTES`.

Things that fail with every image, including the official one, so the
harness does not run them that way:

* test_ctrl's `loopback` case in FPGA tuning mode: libbladeRF only supports
  the RFIC BIST loopback in host tuning mode.
* `test_fpga_load` in host tuning mode: after an FPGA load on an open device
  libbladeRF keeps its host-side AD9361 state and skips the AD9361 set-up,
  but the new image starts with its RFFE control register at the reset value
  (`gpio_rffe_0`, 0x00018000), which holds the AD9361 in reset. The first
  sample rate change then times out in the AD9361's RX baseband filter
  calibration (`Calibration TIMEOUT (0x16, 0x80)`). The fpga phase runs it in
  FPGA tuning mode, where the Nios starts from scratch after the reload.
  Branch `fixes` sets the AD9361 up again after a load, and then it passes
  with all three images (`fpga_load-host`, run with `HWTEST_FIXES=1`).
* The full test_ctrl sweeps in host tuning mode take over 1.5 hours per image
  (the sample rate sweep alone about 50 minutes).
* `test_rx_meta -m` (both RX channels) in FPGA tuning mode: right after the
  second RX channel is enabled, the stream jumps ("Sample discontinuity
  detected"), overruns, and stops ("Transfer timed out for RX buffer", "RX
  "now" failed: Operation timed out"). It passes in host tuning mode, where
  the harness runs it. In FPGA tuning mode libbladeRF enables the two RX
  channels one at a time, and enabling the second resets the receive path
  under the running stream; host tuning mode enables both at once. Branch
  `fixes` does the same in FPGA tuning mode, and then it passes
  (`HWTEST_FIXES=1`).
* TX on or off during a two-channel receive, in host tuning mode: libbladeRF
  decides whether to switch both channels of a direction together from the
  RX stream layout, for TX too, so with an RX_X2 receive running, enabling
  or disabling TX also switches both RX channels. When a transmit ends the
  receive stops ("Transfer timed out for RX buffer"; 3 s on RX1+RX2 while
  TX1 sent 0.2 s got 2.2 M of 30 M samples), and a TX_X2 layout never
  enables both TX channels. Branch `fixes` uses the layout of the channel's
  own direction (`rx2_txtoggle-host` and `-fpga`, run with `HWTEST_FIXES=1`
  and `HWTEST_TX=1`).
* test_ctrl's `threads` case in host tuning mode: test_ctrl refuses it on
  the bladeRF 2.0 (Nuand issue #705). The harness runs it in FPGA tuning
  mode only, but there it failed both times with the official image
  ("Erroneous overall gain readback", then RX/TX transfer time-outs until it
  was killed), and the second time the bladeRF dropped off USB 25 s in and
  only came back when it was unplugged and plugged in again. Unattended
  campaigns leave it out (`HWTEST_SKIP=ctrl_threads-fpga`).

`test_rx_meta`'s two-channel variants overrun now and then, in either tuning
mode and with either libbladeRF build (9 of 180 runs, one in 20): it streams
4 Msps through 16 buffers of 4096 samples, about 16 ms, so a host stall
longer than that overruns ("Overrun detected", or in its scheduled part
"Requested timestamp is in the past"). run-suite.sh counts that as a failure.

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
line (`<image.rbf> <label> [phase...] [VAR=<value>...]` with the settings
above), recovering the board after each. A failed test that leaves the bladeRF
off USB ends the run and the campaign: only a replug brings it back.

## Cabling for TX runs (`HWTEST_TX=1`)

    bladeRF TX1, RX1 -> 50 ohm loads            (nothing measures a TX1 -> RX1 loop yet)
    bladeRF TX2  -> 20 dB -> B200mini RX2       bladeRF TX measured by the USRP
    B200mini TX/RX -> 20 dB -> bladeRF RX2      USRP tone measured by the bladeRF

At least 20 dB per link and no antennas anywhere. At full power (bladeRF 2.0
micro +8 dBm, B200mini about +10 dBm) 20 dB would not keep every receiver at
or below -15 dBm (Ettus's limit for the B200mini's RF inputs), so both
transmitters are capped: with `HWTEST_TX=1` every harness program has
`lib/tx-cap.c` preloaded, which keeps the bladeRF's overall TX gain at or
below 60 dB (about 0 dBm, so -20 dBm at the B200mini), and `rf-check.py`
never sets the B200mini's TX gain above 65 dB (about -15 dBm out). Without
the caps it would take 30 dB per link.

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

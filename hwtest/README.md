# bladeRF 2.0 micro hardware regression harness

Scripts for comparing FPGA images (e.g. Nios II vs Nios V builds) on a real
bladeRF 2.0 micro xA5, unattended, without any risk of bricking it.
[TESTING.md](TESTING.md) records what was tested with them, how, and the
results.

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
them; `HWTEST_FPGA_MIMO=1` is its older name), `SOAK_MINUTES`,
`FULLSOAK_RX_MIN`, `FULLSOAK_LOOP_MIN` and `BIST_MIN`.

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

Test order matters in two cases (`HWTEST_TX=1` runs of 2026-11-13):

* The FPGA keeps its RX mux setting until the next image load: libbladeRF's
  open does not reset it, and `test_digital_loopback -l fpga` leaves it at
  DIGITAL_LOOPBACK. A program that receives afterwards without setting the
  mux gets the TX samples: a bladeRF-cli receive then stopped when its
  transmit ended (1,048,576 of 30,000,000 samples) and bladeRF-cli still
  exited 0.
* `test_digital_loopback -l rfic` fails the first time it runs after an
  image load or after another program transmitted (bladeRF-cli: 8-31 % IQ
  matches, "RX Mismatch detected in block 0"), with Nuand's libbladeRF and
  with the fixes. It passes when run again, and after receive-only programs
  or test_digital_loopback in another mode; after test_rx_meta about 20 of
  its 16,384 samples mismatch. In this mode the test writes and checks only
  every other sample ("The AD9361 tends to swap channels when
  loopbacking"), so its result depends on which sample the loopback starts
  with.

So run-suite.sh runs `digital_loopback_rfic-host` after
`digital_loopback_fpga-host`, with only `unused_sync-host` in between,
where it has passed every time, and the TX on/off test (`rx2_txtoggle-*`)
last in its phase, after an image load, with the RX mux and the loopback set
first.

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
On the hardware, with the fix the full bandwidth sweep passed with all
three images (2026-11-13, `HWTEST_TX=1` runs `*-tx-fixes`), and it is no
slower: traced back to back with the Nios V image, 370 s without the fix and
377 s with it, with the same distribution of call times. The sweep's
duration depends on how it starts: 270-305 s in the overnight deep runs,
where it follows the frequency sweep, but 370-415 s whenever it started
right after an image load, with either libbladeRF build. Compare its time
only between runs that start the same way.

Two fixes to `test_scheduled_retune`, which failed with the official image
too ("Requested timestamp is in the past"): it read the start timestamp
before the initial full tune, which can take longer (~60 ms) than its 20 ms
start-up delay, so it now tunes first; and with 16 RX buffers (8 in flight)
at 5 Msps the sync worker overran every few hundred iterations, so it now
uses 64. (libbladeRF has started RX streaming at `bladerf_enable_module`
since 712d49e2, so a few overruns at start-up are normal.)

### Sample formats, overclocking, triggers, the LVDS interface (formats, oc, trigger, fullsoak, bist)

These phases use `lib/hwtest-stream.c` (built by build-host.sh) and
`bin/pulse-check.py`:

* **rxcheck** receives the FPGA's 32-bit counter (`BLADERF_RX_MUX_32BIT_
  COUNTER`) in a sample format and checks every sample of every channel.
  What arrives depends on the format: SC16 carries all 32 bits (I low, Q
  high), SC8 the low 16 (I bits 7..0, Q 15..8), the packed format only I's
  low 12 bits; with two channels both carry the same value. SC16 and the meta
  formats identify every sample exactly; SC8 and packed cannot tell late
  samples from lost ones, so any jump fails there.
* libbladeRF sometimes delivers whole receive buffers **out of order** at the
  start of a stream (in the first ~10 ms; seen with the official image in
  about two of three 61.44 Msps runs; with SC16 meta and two channels it once
  also duplicated 528 samples per channel). Nothing is lost, but the order is
  wrong, which is libbladeRF's doing (712d49e2 added a reorder queue for
  this). rxcheck therefore records which stretches of samples arrived:
  stretches that never arrive or arrive twice fail, late ones only warn. It
  also skips the first 0.25 s (`-W`); `rx_order-host` runs 10 starts with no
  warm-up to count the reordering per image.
* **loopback** sends a pattern that numbers every sample (and tags the
  channel) through the FPGA's digital loopback (`BLADERF_RX_MUX_DIGITAL_
  LOOPBACK`) and checks every sample that comes back. It uses libbladeRF's
  asynchronous interface, like Nuand's `test_digital_loopback`: with the
  synchronous interface on both sides no sample ever came back (TX and RX
  transfers timed out whatever the order of enabling, though the Nios
  register writes were the same as test_digital_loopback's). So it covers SC16
  and SC8, not the packed or meta formats. The loopback is not paced by the
  sample clock: RX sees 1.3 to 1.6 times the nominal rate, so at high rates it
  overloads USB (SC8 at 61.44 Msps lost 4 of every 4096 samples with the
  official image). With two channels, SC16 lost samples even at 4 Msps with
  the official image; SC8 did not.
* **pulse-check.py tx-fmt** transmits a tone at full rate in each format (TX2,
  and TX1 into its load for two channels); the B200mini checks the frequency
  and that the envelope never drops (an underrun sends zeros for at least a
  buffer).
* Nuand's `test_digital_loopback -D counting` is not usable with `-b 8`: it
  resynchronises by reading the 8-bit samples as 16-bit words and reported 94 %
  matches at 61.44 Msps. The harness uses its own pattern.
* **Overclocking** (`bladerf_enable_feature(BLADERF_FEATURE_OVERSAMPLE)`,
  up to 122.88 Msps, 8-bit only): Nuand's `test_oversample` (both RX
  channels) only checks the time a 1 s capture takes, the power and that
  samples are not stuck at zero. The counter check at 122.88 Msps tests the
  FPGA and USB at that rate; `pulse-check.py oc-rx` and `oc-tx` send pulse
  trains both ways (pulse widths 1, 10 and 100 us, repetition intervals 10,
  100 and 1000 us) and check every pulse width and interval against what was
  sent, which also proves the sample rate. On this board the overclocked
  receive samples are corrupted with every image, the official one included:
  with nothing transmitted to it the 8-bit samples spread over about ±75 LSB
  (I) with 3 to 11 % at full scale, against ±0.5 LSB at 61.44 Msps (the same in
  captures made with Nuand's bladeRF-cli), so the pulses from the B200mini
  cannot be found. The bist phase below shows where. In the transmit
  direction the B200mini finds every pulse, with the right width and
  interval.
* **bist** has the AD9361 replace its received samples with its built-in
  test pattern (BIST) at its RX data port: register 0x3F4 = 0x09 injects the
  PRBS, 0x0B a tone at fs/32 (the values the AD9361 no-OS driver's
  `ad9361_bist_prbs()` and `ad9361_bist_tone()` write; libbladeRF has no
  BIST call, so it uses `bladerf_set_rfic_register`). Every sample then
  crosses the AD9361-to-FPGA LVDS interface, and nothing else, with a known
  value: the pattern repeats (the PRBS every 65,535 samples), so
  hwtest-stream learns its period and one golden period (voted over 15
  repeats) from the first 2^24 samples per channel, restarts the stream,
  finds the phase and checks every later sample, counting errors per bit of
  I and Q. A run of 256 bad samples (lost samples, or garbage) makes it look
  for the phase again, and fails. `-o` saves the golden period and `-i`
  checks against a saved one instead of learning it (e.g. overclocked
  against one learnt at 61.44 Msps). On the xA5 the capture of this
  interface is the design's one timing path with negative setup slack, so
  this is the direct check of it. The AD9361 sends both channels over the
  interface whichever are streamed (2R2T framing, four DATA_CLK cycles per
  sample: `axi_ad9361_dev_if_alt.v`), so its clock is four times the sample
  rate: 245.76 MHz at 61.44 Msps, within the 250 MHz that bladerf.sdc and
  the LVDS receiver (altlvds_rx, 500 Mbps per lane) are set up for.
  libbladeRF's overclocking is a set of undocumented AD9361 register writes;
  the FPGA does nothing different. With it the pattern arrived intact up to
  76.8 Msps (307.2 MHz) and as garbage from 92.16 Msps on (no repeat in 2^24
  samples, and no match with the golden period anywhere), the same with
  Nuand's image and the rebuilds; the phase runs 76.8 and 122.88 Msps. The
  BIST is switched off at the end (also on SIGTERM); `lib/bist-selftest.c`
  (run by build-host.sh) checks the checker on synthetic data.
* **trigger** arms the RX trigger with this bladeRF as master (J51-1), checks
  that no samples arrive in 1 s, fires it and checks that samples then arrive
  and the trigger reports fired. The counter modes bypass the trigger gate,
  so it receives baseband samples.

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
| `bin/pulse-check.py` | Overclocked pulse trains both ways, and full-rate TX in every sample format, against the B200mini |
| `lib/hwtest-stream.c` | Streaming checks (every sample) in every format and layout, captures, triggers, the AD9361 BIST pattern (built into `host-build/output/hwtest-stream`) |
| `bin/compare.py` | Compares result directories and flags regressions |
| `bin/bw-diag.sh` | Runs the FPGA-mode bandwidth test with every Nios packet traced (`lib/nios-trace.c`) |
| `bin/nios-trace-analyze.py` | Flags Nios time-outs and replies that belong to another request in a trace |

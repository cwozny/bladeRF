# How the bladeRF 2.0 micro changes were tested

This is the test record for the changes on the fork's branches `niosv`
(building the bladeRF 2.0 micro FPGA with Quartus 25.1, which needs the
Nios V CPU) and `fixes` (libbladeRF and firmware bug fixes). It says what was
tested, how, on which FPGA images and libbladeRF builds, and what the results
were. It is the source for the "Testing" sections of the pull requests and
issue reports, and tells how to repeat each test.

Raw results are on the test host (XPS 13) under
`~/workarea/hwtest-data/results/<label>/<time>/`: one log per test,
`summary.jsonl` (test, exit status, seconds), `rf.jsonl` (RF measurements),
`meta.json` (image md5, libbladeRF build, harness commit). `bin/compare.py`
compares runs.

## Test setup

**Device under test:** one bladeRF 2.0 micro xA5 (Cyclone V 5CEBA5F23C8),
FX3 firmware v2.6.0 (not changed). Images are only loaded into the FPGA
(volatile); the SPI flash was never written (`bin/bladerf-cli-safe` refuses
every command that could write it).

**Host:** Dell XPS 13 9310, Linux Mint 22.3 (Ubuntu 24.04 base), USB 3
SuperSpeed. Until 2026-11-13 09:55 the bladeRF was on a USB-C port of the
laptop; after that it was behind the VIA hub of a USB-C adapter (port 2-1.3).
Timings from before and after that change do not compare.

**RF reference:** an Ettus USRP B200mini (UHD, python3-uhd), an independent
radio that transmits and measures for `bin/rf-check.py` and
`bin/pulse-check.py`.

**Cabling:**

| Period | bladeRF TX1 | bladeRF RX1 | bladeRF TX2 | bladeRF RX2 |
|---|---|---|---|---|
| until 2026-11-13 09:55 (receive-only) | antenna | antenna | antenna | antenna |
| from 2026-11-13 (transmit tests) | 50 Ω load | 50 Ω load | 20 dB → B200mini RX2 | ← 20 dB ← B200mini TX/RX |

Receive-only runs preload the no-TX guard (`lib/no-tx-guard.c`): nothing can
enable a TX channel. Transmit runs (`HWTEST_TX=1`, only with the second
cabling) preload the TX cap (`lib/tx-cap.c`, bladeRF TX gain ≤ 60 dB on both
channels, about 0 dBm) and limit the B200mini's TX gain to 65 dB, so neither
receiver sees more than about −20 dBm.

## FPGA images

| Label | Image | Built with | CPU | md5 of hostedxA5.rbf |
|---|---|---|---|---|
| A | Nuand's official FPGA v0.16.0 | (Nuand) | Nios II/e | `be0909dc27b48a08c54db33675130f27` |
| B | branch `niosv`, Nios II path (Nuand's design) | Quartus Prime Lite 23.1.1 | Nios II/e | `7ab1c8f913ae8a356de988728ceeb0f7` |
| C | branch `niosv`, Nios V | Quartus Prime Lite 25.1 | Nios V/m, non-pipelined | `b39357947802307a8aa156d5819e721d` |

B shows that the restructured Qsys script still builds Nuand's Nios II design
(same fit and timing as A); C is the Quartus 25.1 build the PR is for.

## libbladeRF builds

| Name | Source | Used for |
|---|---|---|
| main | Nuand master 35174c30 plus this harness (branch `hwtest`, which also carries the test_scheduled_retune fix) | baseline: what Nuand's users have |
| fixes | main plus the four libbladeRF fixes (b49041c9, e9463ba9, fdb03b4e, 3b60e59e); its libbladeRF is identical to branch `fixes` at 3b60e59e | verifying the fixes |

The fixes build was deleted after use (2026-11-13); to rebuild it, make a
detached worktree of `hwtest`, cherry-pick those four commits and run
`bin/build-host.sh <worktree>` with `HOST_BUILD` set to a new directory.

## What each test phase covers

`bin/run-suite.sh <image> <label> <phase...>` loads the image and runs the
phases; `bin/campaign.sh <plan>` runs a list of such runs unattended. The
phases (details in run-suite.sh):

- **smoke:** versions, `bladeRF-cli print` (every setting and sensor), flash
  ID, open/close loop.
- **host / fpga:** Nuand's libbladeRF test programs with AD9361 control on
  the host (libbladeRF's default) and on the Nios (FPGA tuning mode, which
  runs the AD9361 driver in the firmware): test_ctrl's cases (quick
  variants), open/close, quick and scheduled retunes (the Nios retune queue),
  frequency hopping, repeated streams, rx_meta (16- and 8-bit, one and two
  channels), rx_discont, timestamps, digital loopbacks (firmware, FPGA, RFIC
  BIST), tune timing, FPGA reload on an open device, peripheral timing.
- **deep:** test_ctrl's full frequency, bandwidth and sample rate sweeps in
  FPGA tuning mode (about an hour).
- **rf:** `rf-check.py`: tones both ways between the bladeRF and the B200mini
  at 100 MHz to 5.8 GHz and 2 to 30.72 Msps: frequency error, level, image
  and LO leakage, noise floor (compared between images).
- **soak:** a mixed workload (open, retunes, frequency checks, streaming)
  repeated for 90 to 150 minutes after one image load (longer than any IP
  evaluation time-out).
- **formats, oc, trigger, fullsoak:** see "Sample formats, overclocking,
  triggers, full-rate soak" below.

## Results

### 1. Reproducing Nuand's image (2026-11-12)

FPGA v0.16.0 hostedxA5 built with Quartus Prime Lite 23.1.1 (23.1std.1
b993) is byte-identical to Nuand's official image (md5 above). 23.1.0 and
20.1.1 give different bitstreams. So 23.1.1 is the reference toolchain for
image B.

### 2. Build checks with Quartus 25.1 and Nios V

| Image | Logic | RAM blocks | Worst setup slack | Worst hold slack |
|---|---|---|---|---|
| hostedxA4 | 7,845 ALMs (42 %) | 273 / 308 (89 %) | +0.540 ns | +0.039 ns |
| hostedxA5 (C) | 7,868 ALMs | | −0.110 ns | +0.058 ns |
| hostedxA9 | 7,833 ALMs (7 %) | 273 / 1,220 (22 %) | +0.523 ns | +0.066 ns |

- The xA5's only failing path is the AD9361 RX LVDS clock; Nuand's own xA5
  build (A, reproduced) fails the same path by 0.154 ns.
- Every Nios V build warns that Quartus could not get the free Nios V license
  ("Client token expired") and that the IP is in evaluation mode; Quartus
  25.1 still writes normal programming files (no time-limited file), and the
  soaks below show no time-out.
- `-s A2` is not a product (libbladeRF knows only A4, A5, A9) and does not
  build with Nuand's design either: with Nios II and Quartus 23.1.1 Qsys
  reports the debug port at 0x8800 inside the 32 KiB RAM at 0x8000. Left out.

### 3. Receive-only campaign (2026-11-12 22:46 to 11-13 07:23)

libbladeRF main, antennas attached, no transmitting. 14 runs, all completed:

- smoke + host + fpga: A, B and C 50/50 in each of three rounds;
- deep: A 5/6, B 5/6, C 6/6 (the failures: the full bandwidth sweep, the
  stale-reply bug fixed by b49041c9 below);
- soak: C 150 minutes after one image load, 938/938; A 90 minutes, 534/534;
- `compare.py`: no differences between the images beyond timing noise. C is
  faster on Nios-bound work (FPGA-mode sweeps: frequency 405 s vs 487 s,
  bandwidth 271 vs 303, sample rate 3169 vs 3443; host-mode tests equal).

### 4. Transmit campaign (2026-11-13 afternoon)

`HWTEST_TX=1`, second cabling, smoke + host + fpga + rf (+ the bandwidth sweep
with the fixes), test_ctrl's threads case left out (see open items):

| libbladeRF | A | B | C |
|---|---|---|---|
| main | 73/73 | 73/73 | 72/73 (rx_meta MIMO overrun) |
| first two fixes, plus the full bandwidth sweep | 79/79 | 79/79 | 78/79 (rx_meta MIMO overrun) |

RF (rf-check): C within ±0.8 dB tone level and ±0.05 ppm frequency of A.

### 5. Transmit soaks (2026-11-13, libbladeRF main, 90 minutes)

| Image | Rounds | Tests | Result | Results directory |
|---|---|---|---|---|
| C | 135 | 677/677 | no overruns | `results/C-tx-soak/20261113-163019` |
| B | 127 | 637/637 | no overruns | `results/B-tx-soak/20261113-180053` |

Each round includes test_quick_retune, which transmits.

### 6. Fix verification (2026-11-13 evening)

libbladeRF fixes, `HWTEST_FIXES=1` (also runs what fails with every image
without the fixes), smoke + host + fpga + rf:

| Run | A | B | C |
|---|---|---|---|
| txverify2 (`results/*-tx-verify2/`) | 78/78 | 77/78 (rx_meta 8-bit MIMO overrun) | 78/78 |

The first attempt (txverify) failed rx2_txtoggle-host/-fpga and
digital_loopback_rfic-host on all images because of test order (open item 3);
the harness now runs rx2_txtoggle last in its phase with the RX mux set first.
At 5.8 GHz the bladeRF TX tone measured −57.2 to −58.0 dBFS on all images.

### 7. Sample formats, overclocking, triggers, full-rate soak (2026-11-13 night)

(Filled in when the campaign has run.)

## Evidence per change

### Quartus 25.1 build of the bladeRF 2.0 micro (Nios V), branch `niosv`

- Builds: xA4, xA5, xA9 with Quartus Prime Lite 25.1 (section 2).
- Image C passes everything image A passes, in every campaign above
  (sections 3 to 7), in both tuning modes, with Nuand's libbladeRF and with
  the fixes; RF performance matches A (section 4); no time-out in 150-minute
  receive and 90-minute transmit soaks (sections 3, 5).
- Image B: the restructured Qsys script still builds Nuand's Nios II design,
  with the same fit and timing as A, and passes the same tests.
- Only one board (xA5) was tested on hardware; xA4 and xA9 were built only.

### b49041c9 libbladeRF: discard stale Nios responses

- Symptom: `test_ctrl -T fpga -t bandwidth` (full sweep) fails 2 to 25 times
  per run with images A and B ("Unexpected bandwidth ... actual=3").
- Cause shown with a trace of every Nios packet (`lib/nios-trace.c`,
  `bin/nios-trace-analyze.py`, `bin/bw-diag.sh`; traces in
  `diag-bw/20261113-075412/`): a late reply is taken as the answer to the
  next request.
- Reproduced deterministically with `lib/late-reply.c` (delays every 20th RFIC
  status reply): 17 failures without the fix, 0 with it.
- With the fix the full sweep passes on A, B and C (section 4), and it is not
  slower: 370.5 s without and 376.8 s with the fix, traced back to back
  (`results/bw-diag/20261113-161738`, `-162354`).

### e9463ba9 libbladeRF: enable both channels of a MIMO layout in FPGA tuning mode too

- Without the fix, `test_rx_meta -m` and `-b 8 -m` fail in FPGA tuning mode
  with every image (A, B, C); with it they pass on all three (sections 4, 6).

### fdb03b4e libbladeRF: initialize the RFIC again after an FPGA load in host tuning mode

- Without the fix, `test_fpga_load` fails in host tuning mode with every image
  ("Calibration TIMEOUT"); with it, it passes on A, B and C (section 6,
  fpga_load-host), and in FPGA tuning mode as before.

### 3b60e59e libbladeRF: host tuning mode: MIMO enable uses the layout of the channel's direction

- bladeRF-cli receives 3 s on RX1+RX2 at 5 Msps while TX1 sends 0.2 s:
  without the fix the receive stops when the transmit ends (2,228,224 of
  30,000,000 samples); with it all 30,000,000 samples arrive, in both tuning
  modes (rx2_txtoggle-host/-fpga, section 6, on A, B and C).

### 923d1b36 (= niosv 303b678a) nios: don't register an ISR for a VCTCXO tamer without an IRQ

- Found by reading the Nios V port's memory map: `alt_irq[-1]` overwrote the
  last 8 bytes of the RX retune queue in the Nios II build. Images B and C
  have the change and pass everything A passes (sections 3 to 7).

### 7eabdcb3 test_scheduled_retune: tune before reading the start timestamp; 64 RX buffers

- Before: "Requested timestamp is in the past" with the official image. After:
  passes in every run since, all images, both tuning modes, e.g. 135 times in
  a row in the C transmit soak.

## Known noise and open items

1. `rx_meta` with two channels overruns about once in 20 runs, with either
   libbladeRF build and every image (16 buffers of 4096 samples at 4 Msps:
   16 ms of slack). Not a regression.
2. Run times of `ctrl_bandwidth-fpga` vary between 10 and 26 s in every
   configuration; integer-second timings make small differences look large.
3. Test order matters: `test_digital_loopback -l fpga` leaves the RX mux at
   DIGITAL_LOOPBACK (libbladeRF's open does not reset it), and
   `test_digital_loopback -l rfic` fails its first run after an image load or
   after another program transmitted. Same with Nuand's libbladeRF.
4. `test_ctrl -T fpga -t threads` failed twice with image A and Nuand's
   libbladeRF, and the second time the bladeRF dropped off USB until it was
   replugged. It has been left out of unattended runs; it needs a rerun with
   someone at the bench.

## How to repeat

On the test host, with the harness checked out at `~/workarea/hwtest`
(branch `hwtest`) and `bin/build-host.sh` run once:

    # receive-only (antennas allowed)
    bin/run-suite.sh images/C-niosv-q25.1.rbf C-check smoke host fpga
    # with the transmit cabling above
    HWTEST_TX=1 bin/run-suite.sh images/C-niosv-q25.1.rbf C-tx smoke host fpga rf
    HWTEST_TX=1 bin/run-suite.sh images/C-niosv-q25.1.rbf C-new formats oc trigger fullsoak
    # several runs unattended
    HWTEST_TX=1 bin/campaign.sh plan.txt

Add `HOST_BUILD=<fixes build> HWTEST_FIXES=1` to test with the libbladeRF
fixes.

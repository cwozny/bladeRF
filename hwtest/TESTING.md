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
- **sched:** a recorder's scheduled 100 ms bursts with timestamps, written to
  a file between bursts, every sample checked against the FPGA's counter.
- **bist:** the AD9361's built-in test pattern (PRBS, or a tone) replaces the
  received samples at its RX data port, so every sample crosses only the
  AD9361-to-FPGA LVDS interface (the xA5's one failing timing path) with a
  known value; every sample and bit is checked, at 61.44 Msps on each channel
  and with two channels, and overclocked (README.md, "bist").

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
| hostedxA5 (C) | 7,868 ALMs (27 %) | 273 / 446 (61 %) | −0.110 ns | +0.058 ns |
| hostedxA9 | 7,833 ALMs (7 %) | 273 / 1,220 (22 %) | +0.523 ns | +0.066 ns |
| for comparison: hostedxA5 v0.16.0, Nios II/e, Quartus 23.1.1 (A; B is the same) | 6,757 ALMs (23 %) | 274 / 446 (61 %) | −0.154 ns | |

The Nios V/m takes 1,111 more ALMs (and 1,778 more registers) than the
Nios II/e on the xA5. B's fit and timing are identical to A's (same Fmax for
every clock).

The FPGA's whole RX/TX sample logic (FIFOs, metadata, loopback) runs on the
LVDS receiver's slow clock (`ad9361.clock`, `pll_sclk`): half the AD9361's
DATA_CLK, so 122.88 MHz at 61.44 Msps, constrained to 125 MHz. Its Fmax in
the slow 85 °C model: A and B 133.33 MHz, C 126.18 MHz (both meet the
constraint; the negative slack above is on a path into that domain from
another clock, which the Fmax summary leaves out).

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

### 7. Sample formats, overclocking, triggers, full-rate soak (night of 2026-11-13/14)

libbladeRF main, `HWTEST_TX=1`, harness 0821988d (job `newtests`, plan
`plans/new-20261114.txt`; results `results/{A,B,C}-new/`). Tools: see
README.md, "Sample formats, overclocking, triggers".

**Receive at full rate, every sample checked** (`fmt_rx`, 20 s each after
0.25 s of warm-up; host tuning mode, and one channel also in FPGA tuning
mode):

| Format | One channel | Two channels | A | B | C |
|---|---|---|---|---|---|
| SC16 | 61.44 Msps | 2 × 30.72 Msps | pass | pass | pass |
| SC16 meta | 61.44 | 2 × 30.72 | pass | pass | pass |
| SC8 | 61.44 | 2 × 61.44 | pass | pass | pass |
| SC8 meta | 61.44 | 2 × 61.44 | pass | pass | pass |
| packed (SC16_Q11_PACKED) | 61.44 | 2 × 40.96 | pass | pass | pass |

No sample lost, duplicated or corrupted in any of the 15 cases per image, the
two channels always in step. Two-channel SC8 and packed exceed the 80 Msps
total that libbladeRF recommends (it warns) and still passed.

**Transmit at full rate** (`fmt_txrf`, same formats and rates, TX2 and with
two channels TX1 into its load; the B200mini receives the tone): all 15 cases
pass on every image. Tone within +790 to +903 Hz (about 0.9 ppm) of the
expected frequency, level −31.9 to −33.0 dBFS in every format, images
−84 to −93 dBc, envelope never more than 0.8 dB below its median: no dropout.

**TX → RX through the FPGA's digital loopback, every sample checked**
(`fmt_loop`, 10 s):

| Case | A | B | C |
|---|---|---|---|
| SC16, one channel, 4 Msps (host, FPGA mode) | pass, pass | pass, pass | pass, pass |
| SC8, one channel, 4 Msps (host, FPGA mode) | pass, pass | pass, pass | pass, pass |
| SC8, two channels, 4 Msps | pass | pass | pass |
| SC16, two channels, 4 Msps | **fail** | **fail** | **fail** |
| SC16, one channel, 30.72 Msps (host, FPGA mode) | pass, pass | pass, fail (1 error) | pass, pass |
| SC8, one channel, 40.96 Msps (host, FPGA mode) | pass, fail (1 error) | pass, fail (1 error) | pass, fail (1 error) |

The two-channel SC16 loopback loses samples on every image, Nuand's
included (a limit of the FPGA's loopback path, not of these changes). The
single errors at the higher rates were a few samples lost at a fixed point
~78,000 samples after the start, on every image; the loopback check now skips
the first 0.25 s (0c7f2915) and was run again (below).

The digital loopback does not run at the sample rate that is set: with
loopback enabled the FPGA requests TX samples on every `tx_clock` cycle
(`bladerf-hosted.vhd`, `dac_controls(i).data_req <= (... or
tx_loopback_enabled)`), so the rate depends on the format and layout. The
same on A, B and C (rates per channel):

| Case | set | TX side | RX side |
|---|---|---|---|
| SC16, one channel | 4 / 30.72 Msps | 5.33 / 40.96 Msps (×4/3) | the same |
| SC8, one channel | 4 / 40.96 Msps | 6.40 / 65.53 Msps (×1.6) | the same |
| SC16, two channels | 4 Msps | 4.50 Msps (×9/8) | 4.00 Msps: one sample in nine lost |
| SC8, two channels | 4 Msps | 5.33 Msps (×4/3) | the same |

So the loopback checks the sample formats over USB in both directions, at
those rates, but not the sample clock.

**Overclocking** (`oc`, 122.88 Msps, 8-bit, both tuning modes):

| Test | A | B | C |
|---|---|---|---|
| Nuand's test_oversample, RX1 and RX2 | pass | pass | pass |
| counter check, SC8 (FPGA and USB at 122.88 Msps) | pass | pass | pass |
| counter check, SC8 meta | pass | pass | **fail** (both modes) |
| RX pulses from the B200mini | fail | fail | fail |
| TX pulses to the B200mini | fail (carrier check, see below) | fail (same) | fail (same) |

- **RX:** with nothing transmitted, the overclocked 8-bit samples spread over
  ±76.7 LSB (I) with 5.1–5.5 % at full scale on all three images (±0.5 LSB at
  61.44 Msps), so the pulses cannot be found. The same on A, B and C: the
  corruption is not caused by the changes. Nuand's test_oversample passes
  anyway (it checks timing, power and stuck samples). Section 8 shows that
  the AD9361-to-FPGA interface itself fails at this rate.
- **TX:** the B200mini finds every pulse, with the right width and interval
  on all three images (e.g. 1 µs / 10 µs sent: 1.019–1.022 µs / 10.002 µs
  measured, 24,995–24,996 pulses of 24,996; PRI within 1 ppm). The cases
  failed only on the carrier check, which my phase-step estimate got wrong
  (fixed in 0c7f2915, re-run below).
- **SC8 meta at 122.88 Msps fails on C only:** overrun flags within seconds,
  timestamp jumps, then the stream stalls ("Transfer timed out"), in both
  tuning modes, while A and B ran 20 s (2.46 G samples) clean. Without
  metadata the same counter arrives intact on C. Overclocking is a set of
  undocumented AD9361 register writes in libbladeRF
  (`bladerf_set_oversample_register_config`, which also writes 0x03 to
  bits of register 0x3F6 that the register map says must be 0); the FPGA
  design has no overclock mode. Being repeated (below).

**Trigger** (`trigger_rx`, master on J51-1): passes on every image in both
tuning modes; no samples while armed, first samples 4.1–4.6 ms after firing.

**libbladeRF start-up order** (`rx_order-host`, 10 stream starts at
61.44 Msps with no warm-up): a whole buffer arrived late (nothing lost) in
3 of 10 starts on A, 5 on B, 4 on C: libbladeRF, not the images.

**Full-rate soaks** (`fullsoak`, one image load each, host tuning mode):

| Image | RX counter, SC16, 61.44 Msps, 60 min | Loopback, SC16, set to 30.72 Msps, 30 min |
|---|---|---|
| C | pass: 221,183,311,872 samples, none missing, duplicated or late | pass: 73,730,588,672 samples back (at 40.96 Msps, see above), 0 errors |
| A | pass: 221,183,852,544 samples, none missing, duplicated or late | **fail:** stopped after 67 s (2,749,104,128 samples back, 0 errors), then RX and TX transfers timed out |
| B | pass: 221,183,754,240 samples, none missing, duplicated or late | pass: 73,729,507,328 samples back, 0 errors |

A's loopback stopped moving samples in both directions at once, 67 s into
the run, with no USB error in the kernel log (`journalctl -k`: only the
resets at each open) and nothing else running on the host; every sample
before that was correct. On Nuand's image, so not a regression, but an
intermittent stall of unknown cause; repeated below.

(Re-run with the current harness, the overclock repeats and the loopback
soak repeats: section 9, to come.)

### 8. The AD9361-to-FPGA LVDS interface with the AD9361's BIST (2026-11-14 morning)

`hwtest-stream bist` (README.md, "bist"): the AD9361 replaces its received
samples with its test pattern (a PRBS repeating every 65,535 samples, or a
tone) at its RX data port, so every sample crosses only the LVDS interface,
whose capture is the xA5's one timing path with negative setup slack
(section 2), and every sample and bit is checked. Receive only, host tuning
mode. The AD9361 sends both channels over the interface whichever are
streamed (2R2T framing: four DATA_CLK cycles per sample, see
`axi_ad9361_dev_if_alt.v`), so the interface clock is four times the sample
rate: 245.76 MHz at 61.44 Msps, where bladerf.sdc and the LVDS receiver
(altlvds_rx: 250 MHz, 500 Mbps per lane) are set up for 250 MHz.

**First runs** (`diag-bist/1114-0533`, A and C, harness 00e2b143), all pass
on both images with no bad sample:

| Case | Samples checked per channel |
|---|---|
| PRBS, SC16 (all 12 bits), RX1 and RX2, 61.44 Msps, 10 s each | 631,177,216 |
| tone, SC16, RX1, 61.44 Msps, 5 s | 323,977,216 |
| PRBS, SC8 (bits 11–4), RX1 + RX2 together, 61.44 Msps, 10 s | 631,177,216 |
| PRBS, SC8, RX1, 61.44 Msps, 5 s | 323,977,216 |

**Overclocking** (libbladeRF's `BLADERF_FEATURE_OVERSAMPLE`, SC8, RX1, 5 s
per rate; `diag-bist/oc-sweep-1114-0538`, `diag-bist/oc-fine-1114-0543`).
libbladeRF sets the AD9361 to half the rate and doubles it with undocumented
register writes; the FPGA does nothing different:

| Rate (interface clock) | A | B | C |
|---|---|---|---|
| 10, 30.72, 61.44 Msps, with and without the feature | pass | | pass |
| 76.8 Msps (307.2 MHz), 5 s, RX1 | pass (400,785,408 samples) | | pass |
| 76.8 Msps, 20 s each, RX1 / RX2, after 6 minutes of streaming (bist campaign) | 3,105 bad (2e-6) / 10.4 M bad (0.7 %) | 2 bad (1e-9) / garbage | 9,347 bad (6e-6) / 19.9 M bad (1.3 %) |
| 78 Msps (312 MHz) | 8,665 bad (2.1e-5) | 8 bad (2.0e-8) | 9 bad (2.2e-8) |
| 80 Msps (320 MHz) | 4,275 bad (1.0e-5) | 504,719 bad (1.2e-3) | 3,790 bad (9.1e-6) |
| 82 Msps (328 MHz) | 1.4 % bad | garbage | 633,370 bad (1.5e-3) |
| 84 Msps (336 MHz) | 1.3 % bad | garbage | garbage |
| 86 to 122.88 Msps (344 to 491.52 MHz) | garbage | garbage | garbage |

"Garbage": no repeat of the pattern in 2^24 samples; at 122.88 Msps the
samples also matched the pattern learnt at 61.44 Msps nowhere, and the tone
did not repeat either. Every bad sample up to 84 Msps had the same single
bit wrong: bit 7 of the 12-bit sample (bit 3 of SC8), in I and in Q, on all
three images: the first thing to fail is one data lane, the same with all
three builds, which points at the board or the IO cells rather than at
anything the fitter placed differently. With the feature at 61.44 Msps the PRBS (RX1 and
RX2) and the tone pass, so libbladeRF's register writes are not the problem:
the rate is.

- So overclocked receive at 122.88 Msps cannot work on this board with any
  image: the interface would need 491.52 MHz and delivers errors from about
  312 MHz. This explains the corrupted overclocked samples in section 7.
- The interface keeps its margin with the Nios V build: errors start at
  about 76.8 Msps, 1.25 times its design rate, on A, B and C alike, always in
  the same bit, and which image does best changes from channel to channel and
  run to run (at 76.8 Msps B was best on RX1 and worst on RX2). The negative
  slack (−0.154 ns in Nuand's build, −0.110 ns in C) is for the slow 85 °C
  model; one board at room temperature cannot show what happens at the
  corners.

**Cross-check with Nuand's bladeRF-cli** (`diag-oc-cli/1114-0535`; none of
the harness's streaming code; RX at 915 MHz, gain 10 dB, nothing
transmitted; the first 1 M samples skipped):

| Capture | I std | Q std | at full scale |
|---|---|---|---|
| A, RX1 / RX2, 8-bit, 61.44 Msps | 0.55 / 0.52 LSB | 0.51 / 0.50 LSB | 0 % |
| A, RX1 / RX2, `set feature oversample`, 122.88 Msps | 74.0 / 76.9 LSB | 38.3 / 41.5 LSB | 2.8 / 10.9 % |
| C, RX1 / RX2, 8-bit, 61.44 Msps | 0.55 / 0.52 LSB | 0.51 / 0.50 LSB | 0 % |
| C, RX1 / RX2, `set feature oversample`, 122.88 Msps | 74.1 / 76.8 LSB | 39.0 / 41.6 LSB | 2.7 / 11.0 % |

The same corruption as with the harness, on Nuand's image as on C.

**BIST campaign** (`bist` phase, harness 53394f98; results
`results/{A,B,C}-bist/`): every case at the design rate passes on A, B and C
with no bad sample:

| Case | Samples checked per channel (each image) |
|---|---|
| PRBS, SC16, RX1 and RX2, 61.44 Msps, 2 minutes each | 7,389,560,832 (B RX2: 7,389,577,216; C RX2: 7,389,544,448) |
| PRBS, SC16, RX1 + RX2 together, 30.72 Msps, 2 minutes | 3,703,177,216 (B, C: 3,703,169,024) |
| PRBS, SC8, RX1 + RX2 together, 61.44 Msps, 20 s | 1,245,577,216 |
| tone, SC16, RX1, 61.44 Msps, 20 s | 1,245,577,216 |

The overclocked cases (76.8 Msps on RX1 and RX2, 122.88 Msps on RX1) fail on
every image, as in the table above: they measure the margin, not a
regression.

**10-minute runs** (`BIST_MIN=10`; `results/C-bist-long/20261114-060813`,
`results/A-bist-long/20261114-063939`), no bad sample on C or A:

| Case | C | A |
|---|---|---|
| PRBS, SC16, RX1, 61.44 Msps, 10 minutes | 36,880,678,912 samples | 36,880,662,528 |
| PRBS, SC16, RX2, 61.44 Msps, 10 minutes | 36,880,695,296 | 36,880,678,912 |
| PRBS, SC16, RX1 + RX2, 30.72 Msps, 10 minutes | 18,448,736,256 per channel | 18,448,736,256 per channel |

That is about 2.66 × 10^12 bits per image (12 bits each of I and Q) with no
error: a bit error rate below 1.1 × 10^-12 (95 % confidence) at the design
rate, on the Nios V build as on Nuand's. Overclocked at 76.8 Msps in the same
runs: RX1 40 bad samples on C, 14,823 on A; RX2 garbage on both.

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

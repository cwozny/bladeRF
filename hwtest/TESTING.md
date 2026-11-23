# How the bladeRF 2.0 micro changes were tested

This is the test record for the changes on these branches of the fork, each
based on Nuand's master 35174c30:

- `niosv`: builds the bladeRF 2.0 micro FPGA with Quartus 25.1, which needs
  the Nios V CPU, for all of the micro's revisions (hosted, wlan, adsb,
  foxhunt). It includes the two firmware fixes the port needed: the VCTCXO
  tamer interrupt (an out-of-bounds write that is harmless in the Nios II
  build but hits live data in the Nios V build) and the assert macro
  (newlib-nano).
- `host-fixes`: six libbladeRF fixes and three fixes to Nuand's test
  programs (test_scheduled_retune, test_ctrl's threads and gain tests), for
  problems these tests found that occur with Nuand's own image and
  libbladeRF too, independent of the Nios V port.
- `adsb-fix`: two HDL fixes that let the adsb revision build again; Nuand's
  master fails to build it with Quartus 23.1.1 too.
- `hwtest`: this harness. It is not meant to be merged; it also carries the
  test_scheduled_retune fix and a `BUILD_BLADERF_UPDATE` option (configure
  the host software without libcurl) for its own builds.

It says what was tested, how, on which FPGA images and libbladeRF builds, and
what the results were. It is the source for the "Testing" sections of the
pull requests and issue reports, and tells how to repeat each test.

Raw results are on the test host (XPS 13) under
`~/workarea/hwtest-data/results/<label>/<time>/`: one log per test,
`summary.jsonl` (test, exit status, seconds), `rf.jsonl` (RF measurements),
`meta.json` (image md5, libbladeRF build, harness commit). `bin/compare.py`
compares runs.

On 2026-11-21 every commit on these branches was reworded (its AI-attribution
trailer only: the same trees, authors and dates), so their IDs changed. The IDs
in this file are the new ones; logs and `meta.json` files written before that
name the old ones (both IDs: `~/workarea/hwtest-data/commit-map-20261121.txt`
on the test host).

## Test setup

**Device under test:** one bladeRF 2.0 micro xA5 (Cyclone V 5CEBA5F23C8),
FX3 firmware v2.6.0 (not changed). Images are only loaded into the FPGA
(volatile); the SPI flash was never written (`bin/bladerf-cli-safe` refuses
every command that could write it).

**Host:** Dell XPS 13 9310, Linux Mint 22.3 (Ubuntu 24.04 base), USB 3
SuperSpeed. Until 2026-11-13 10:13 (its first enumeration behind the hub in
the kernel log) the bladeRF was on a USB-C port of the laptop (2-3); after
that it was behind the VIA hub of a USB-C adapter (port 2-1.3).
Timings from before and after that change do not compare. On 2026-11-16 at
22:07 it moved to the laptop's other USB-C port (2-3), and the B200mini,
which had that port, went off USB (section 14). On 2026-11-19 at 23:25 the
xA9 went behind the hub again (2-1.3) and the B200mini back on 2-3, for the
RF tests of section 18. On 2026-11-21 at about 08:40 the xA5 took the xA9's
place there, for the two-channel runs at the end of section 19.

**RF reference:** an Ettus USRP B200mini (UHD, python3-uhd), an independent
radio that transmits and measures for `bin/rf-check.py` and
`bin/pulse-check.py`.

**Cabling:**

| Period | bladeRF TX1 | bladeRF RX1 | bladeRF TX2 | bladeRF RX2 |
|---|---|---|---|---|
| until 2026-11-13 09:55 (receive-only) | antenna | antenna | antenna | antenna |
| from 2026-11-13 (transmit tests) | 50 Ω load | 50 Ω load | 20 dB → B200mini RX2 | ← 20 dB ← B200mini TX/RX |
| 2026-11-15 07:52 (channel 1, section 13) | 20 dB → B200mini RX2 | ← 20 dB ← B200mini TX/RX | 50 Ω load | 50 Ω load |
| from 2026-11-15 10:43 (Nuand's threads test, section 14) | 20 dB → 20 dB → B200mini RX2 | 50 Ω load | 50 Ω load | 50 Ω load |
| from 2026-11-19 23:25 (the xA9 at RF, section 18) | 20 dB → B200mini RX2 | ← 20 dB ← B200mini TX/RX | 50 Ω load | 50 Ω load |
| from 2026-11-20 ~07:05 (the xA9's channel 2, section 18) | 50 Ω load | 50 Ω load | 20 dB → B200mini RX2 | ← 20 dB ← B200mini TX/RX |
| from 2026-11-20 ~21:45 (the xA9's two channels at once, receive; open item 8) | 50 Ω load | ← 20 dB ← splitter ← B200mini TX/RX | 50 Ω load | ← 20 dB ← the same splitter |
| from 2026-11-20 ~22:20 (the same, the two cables at RX1 and RX2 exchanged) | 50 Ω load | the path that was on RX2 | 50 Ω load | the path that was on RX1 |
| from 2026-11-20 ~22:30 (the xA9's two channels at once, transmit; open item 8) | RX1's cable: 20 dB → splitter → B200mini RX2 | 50 Ω load | RX2's cable: 20 dB → the same splitter | 50 Ω load |
| from 2026-11-21 ~08:40 (the xA5's two channels at once, transmit; section 19) | 20 dB → splitter → B200mini RX2 | 50 Ω load | 20 dB → the same splitter | 50 Ω load |
| from 2026-11-21 ~12:45 (the xA5's two channels at once, receive) | 50 Ω load | ← 20 dB ← splitter ← B200mini TX/RX | 50 Ω load | ← 20 dB ← the same splitter |
| from 2026-11-21 ~13:20 (the same, the two cables at RX1 and RX2 exchanged) | 50 Ω load | the path that was on RX2 | 50 Ω load | the path that was on RX1 |

The splitter is a TOJOIN PS-2SM-2080 (2 to 8 GHz) with a 20 dB attenuator
on each output (README.md, "Two-channel setups"). The B200mini's unused
port has a 50 Ω load.

Receive-only runs preload the no-TX guard (`lib/no-tx-guard.c`): nothing can
enable a TX channel. Transmit runs (`HWTEST_TX=1`, only with one of the
transmit cablings) preload the TX cap (`lib/tx-cap.c`, bladeRF TX gain ≤ 60 dB on both
channels, about 0 dBm) and limit the B200mini's TX gain to 65 dB, so neither
receiver sees more than about −20 dBm.

## FPGA images

| Label | Image | Built with | CPU | md5 of hostedxA5.rbf |
|---|---|---|---|---|
| A | Nuand's official FPGA v0.16.0 | (Nuand) | Nios II/e | `be0909dc27b48a08c54db33675130f27` |
| B | branch `niosv` (work-in-progress commit d984883b, not on the fork), Nios II path (Nuand's design) | Quartus Prime Lite 23.1.1 | Nios II/e | `7ab1c8f913ae8a356de988728ceeb0f7` |
| C | `niosv` as tested (tag `niosv-tested`) at b2a2643d, Nios V | Quartus Prime Lite 25.1 | Nios V/m, non-pipelined | `b39357947802307a8aa156d5819e721d` |

B shows that the restructured Qsys script still builds Nuand's Nios II design
(same fit and timing as A); C is the Quartus 25.1 build the PR is for. The
commits after C do not change these images: 5233ad4e adds a message to
build_bladerf.sh, e60da19b moves the Nios V firmware build into
`niosv_app.cmake` (the hosted RAM image stays byte-identical), fe4bb32d
adds the foxhunt firmware and c44a10c0 updates documentation and two script
messages. For a Nios II build d984883b has the final
branch's code: what changed after it is the Nios V build, build_bladerf.sh,
documentation and an `#undef assert` that has no effect on a Nios II build.

The commit IDs in this document are those of the history the tests ran on,
kept on the fork as tag `niosv-tested` (c44a10c0). Branch `niosv` has the
same final tree (`git diff niosv-tested niosv` is empty), arranged as nine
commits for review:

| `niosv` | Same changes as (`niosv-tested`) |
|---|---|
| 3a6c0a83 nios: don't register an ISR for a VCTCXO tamer without an IRQ | d1aabacb |
| 569e19c3 hdl: bladeRF-micro: add a Nios V/m option to the Qsys system | bb8639fe |
| eb6b7670 nios: let the firmware's assert replace the C library's | 9db0f794 |
| d746efac nios: build the bladeRF 2.0 micro firmware for Nios V with CMake | 0a1a284b, the CMake part of b2a2643d |
| d2c0ad6a hdl: build_bladerf.sh: build the bladeRF 2.0 micro with Quartus 25.1 | 87935623, 0f20fbe9, 5233ad4e, the script part of b2a2643d |
| 25147134 doc: Quartus 25.1 and Nios V for the bladeRF 2.0 micro | the documentation in the above and in c44a10c0 |
| 03f3ef81 nios: share the Nios V firmware build between applications | e60da19b |
| 72f37da8 nios: build the foxhunt firmware for Nios V | fe4bb32d |
| 73333f55 doc: point to niosv_app.cmake for the Nios V compiler flags | part of c44a10c0 |

Built again from `niosv` (73333f55) with Quartus 25.1 on 2026-11-16,
hostedxA5 is byte-identical to C (`b39357947802307a8aa156d5819e721d`).

For a second board, an xA9 (Cyclone V 5CEBA9; `HWTEST_FPGA=A9`, images in
`images-xA9/`), the same three were built as hostedxA9 on 2026-11-17: A
`1ab3bc8bdcf16a5f0ddf3fcdbaa6556b` (Nuand's), B
`3c7e86602829af36f29c7e2165e14883` (d984883b, 23.1.1) and C
`fd73509cc7444ec1db0af6efb3482728` (73333f55, 25.1).

## libbladeRF builds

| Name | Source | Used for |
|---|---|---|
| main | Nuand master 35174c30 plus this harness (branch `hwtest`, which also carries the test_scheduled_retune fix) | baseline: what Nuand's users have |
| fixes | main plus the four libbladeRF fixes of branch `host-fixes` (3a768f3d, 2ac75f37, dbad1e70, e64e4026); built from the earlier branch `fixes` at 3b60e59e, whose libbladeRF is identical | verifying the fixes |

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

### 1. Reproducing Nuand's image (2026-11-12; xA9 2026-11-17)

FPGA v0.16.0 hostedxA5 built with Quartus Prime Lite 23.1.1 (23.1std.1
b993) is byte-identical to Nuand's official image (md5 above). 23.1.0 and
20.1.1 give different bitstreams. So 23.1.1 is the reference toolchain for
image B. The same holds for the xA9: hostedxA9 built from fpga_v0.16.0
(f746c5bd) with 23.1.1 is byte-identical to Nuand's official hostedxA9.rbf
(`1ab3bc8bdcf16a5f0ddf3fcdbaa6556b`).

### 2. Build checks with Quartus 25.1 and Nios V

| Image | Logic | RAM blocks | Worst setup slack | Worst hold slack |
|---|---|---|---|---|
| hostedxA4 | 7,845 ALMs (42 %) | 273 / 308 (89 %) | +0.540 ns | +0.039 ns |
| hostedxA5 (C) | 7,868 ALMs (27 %) | 273 / 446 (61 %) | −0.110 ns | +0.058 ns |
| hostedxA9 | 7,833 ALMs (7 %) | 273 / 1,220 (22 %) | +0.523 ns | +0.066 ns |
| for comparison: hostedxA5 v0.16.0, Nios II/e, Quartus 23.1.1 (A; B is the same) | 6,757 ALMs (23 %) | 274 / 446 (61 %) | −0.154 ns | |
| for comparison: hostedxA9 v0.16.0, Nios II/e, Quartus 23.1.1 (A; B has the same numbers) | 6,754 ALMs (6 %) | 274 / 1,220 (22 %) | +0.522 ns | +0.010 ns |

The Nios V/m takes 1,111 more ALMs (and 1,778 more registers) than the
Nios II/e on the xA5. B's fit and timing are identical to A's (same Fmax for
every clock). The hostedxA9 C row is the build from 5233ad4e (2026-11-13);
built again from 73333f55 on 2026-11-17 it is byte-identical
(`fd73509cc7444ec1db0af6efb3482728`).

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
  stale-reply bug fixed by 3a768f3d below);
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

libbladeRF main, `HWTEST_TX=1`, harness 05aa45f9 (job `newtests`, plan
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
the first 0.25 s (db9b74f1) and was run again (below).

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
  (fixed in db9b74f1, re-run below).
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
soak repeats: section 9.)

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

**First runs** (`diag-bist/1114-0533`, A and C, harness f4caf4e2), all pass
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

**BIST campaign** (`bist` phase, harness 180f7301; results
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

### 9. Repeats with the current harness; how far overclocking goes (2026-11-14 morning)

**Rerun of the format, overclock and trigger phases** (job `morning2`, harness
180f7301, results `results/{A,B,C}-new2/`): the same results as in section 7
on all three images, with the fixed checks. All 15 full-rate receive and all
15 transmit cases pass (10 each in host tuning mode, 5 in FPGA tuning mode);
the loopback fails only for two channels in SC16 (every
image, the loopback's own limit, section 7) and now passes at 30.72 and
40.96 Msps in both modes; `rx_order` and the trigger pass. Overclocked at
122.88 Msps: test_oversample and the SC8 counter pass, the received pulses
fail (corrupted samples, every image, section 8), the TX pulse cases fail
only on the carrier (below), and SC8 meta fails on C only, in both modes.

**Loopback soak repeat** (`results/{A,C}-loopsoak/`, 30 minutes, SC16 set to
30.72 Msps): A and C pass, 0 errors (A: 73.7 G samples back). A's stall in
section 7 did not come back: one stall in two 30-minute runs on Nuand's
image, none in two on ours (B one run).

**Where C's overclocked SC8 meta stream stops** (`diag-after/1114-0805`,
job `after`, FPGA counter, 20 s per rate, one channel): clean on A and C at
76.8, 92.16, 100, 107.52 and 115.2 Msps (1.5 to 2.3 G samples each, no
sequence jump); at 122.88 Msps A passes (2.46 G samples) and C stalls
("Transfer timed out"). With 100 ms reads scheduled 5 ms ahead and every
sample checked (`rxsched`), A 418 of 418 at 122.88 Msps, C fails on the
first. So it is the last step only, and only on C: overclocking runs the
FPGA's sample logic at 245.76 MHz against its 125 MHz constraint, where C's
timing model is weakest (section 2: Fmax 126 vs 133 MHz).

**The highest clean overclocked rate: 68 Msps** (BIST PRBS, SC8, every
sample checked, same runs):

| Rate | C RX1 | C RX2 | A RX1 | A RX2 |
|---|---|---|---|---|
| 64, 66 Msps, 60 s | 0 bad | 0 bad | | |
| 68 Msps, 60 s | 0 bad | 0 bad | | |
| 70 Msps, 60 s | 0 bad (4.2 G) | 297 bad (7e-8) | | |
| 68 Msps, 5 minutes | 0 bad (20.4 G) | 0 bad (20.4 G) | 0 bad (20.4 G) | 0 bad (20.4 G) |

The 70 Msps errors were in the same bit as in section 8 (bit 3 of SC8, bit 7
of the 12-bit sample). At 68 Msps on A and C: RX pulses from the B200mini
(10 µs/100 µs and 100 µs/1 ms) pass (the 1 µs case fails only its carrier
estimate, as for TX below), timestamped TX pulses (SC8 meta, all
three widths) pass, and 100 ms recordings (SC8 meta, scheduled 5 ms ahead)
are 479 of 479 clean. At 100 Msps the recordings still arrive complete and
on time on both images (876 and 878 bursts), but the samples are garbage
(BIST: no repeating pattern; pulses not found).

**Overclocked TX with metadata, re-measured** (job `after2`,
`diag-octx/1114-0925`): apart from a pulse cut off by the start of a
capture, every pulse has the right width and interval and the count is
right, on A, B and C at 122.88 Msps in both tuning modes and on A and C at
68 Msps (host mode). The
carrier on 10 µs and 100 µs pulses is within 0.8 to 1.3 kHz of the expected
frequency; on 1 µs pulses the estimate reads 9.5 kHz high on every image
(the 1 µs averaging window is as long as the pulse), so the check now
measures the carrier only on pulses of 3 µs or more, and leaves out the
first and last pulse of a capture (a pulse cut off by the capture's start
gave the single PW/PRI misfits). Re-analysed with those fixes (e7fa951a),
every saved capture passes.

**Noise floor**, from the pulse captures (nothing transmitted, SC8, RX gain
10 dB; the same on A and C):

| Setting | Noise | Phase spread inside a pulse |
|---|---|---|
| 61.44 Msps | −42.4 dBFS rms (below one 8-bit step) | 3–5° |
| 61.44 Msps with the overclock feature on | −36.5 dBFS | 5–6° |
| 70 Msps overclocked | −36.5 dBFS | 5–6° |
| 100 Msps overclocked | −18.8 dBFS (A), −20.6 (C) | ~100° (random) |

The overclock feature costs about 6 dB of noise at any rate (pulses also come
out about 4 dB stronger, so about 2 dB of SNR); at 100 Msps the extra 18 dB
are interface errors (BIST fails there), not the filters.

### 10. Recording 100 ms bursts to disk (2026-11-14)

The user's recorders read 100 ms bursts and write each to its own file;
`hwtest-stream rxsched` does the same with every sample checked against the
FPGA's counter (or, with `-N`, the first and last of each burst), either
scheduled a few ms ahead (`BLADERF_META_FLAG_RX_NOW` off) or back to back
(`-N`, RX_NOW, as the bladeRF paths of radar_recorder and sdr_channelizer
do), with libbladeRF's buffers set like theirs (`-S`).

**First runs** (job `after`, then `after2`, alternating images, 16-bit,
reads scheduled 5 ms ahead, libbladeRF buffers for 59.7 ms at 61.44 Msps):

| Run | A: failed / bursts | longest write | C: failed / bursts | longest write |
|---|---|---|---|---|
| one channel, 61.44 Msps, 2 min | 0 / 928 | | 1 / 929 (first burst) | |
| one channel, 61.44 Msps, 10 min | | | 7 / 4,651 | |
| two channels, 30.72 Msps, 2 min | 0 / 931 | | 27 / 904 | |
| two channels, 30.72 Msps, 3 min, round 1 | 0 / 1,394 | 20.9 ms | 36 / 1,382 | 133.7 ms |
| one channel, 61.44 Msps, 3 min, round 1 | 1 / 1,391 (first) | 22.8 ms | 15 / 1,389 | 121.6 ms |
| two channels, 30.72 Msps, 3 min, round 2 | 0 / 1,391 | 59.7 ms | 1 / 1,398 | 53.7 ms |
| one channel, 61.44 Msps, 3 min, round 2 | 1 / 1,395 (first) | 31.3 ms | 62 / 1,385 | 131.3 ms |
| the same without files | 0 | | 0 | |

Apart from first-burst overruns, every failure was a burst whose timestamp
had passed by the time libbladeRF got to it (BLADERF_ERR_TIME_PAST), in runs
with long disk writes.

**The mechanism** (`sched-diag2`, `diag-sched/1114-1148`, job `after3`):
with no files and a fixed pause instead of the write, the share of bursts
scheduled too late depends on the pause in a sawtooth with an ~8 ms period,
identical on both images (20 s per pause):

| Pause (ms) | 50 | 64 | 66 | 68 | 70 | 72 | 74 | 76 | 78 | 80 | 84 | 86 | 88 | 92 | 94 | 96 | 98 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A too late (%) | 42 | 1 | 5 | 47 | 38 | 0 | 6 | 35 | 48 | 0 | 39 | 46 | 17 | 38 | 39 | 9 | 0 |
| C too late (%) | 32 | 0 | 11 | 49 | 20 | 1 | 6 | 48 | 45 | 0 | 41 | 49 | 8 | 25 | 49 | 35 | 0 |

Each pause adds the ~17 ms counter check and a 0.2 ms timestamp read. After
an overrun libbladeRF's sync worker drops the next `num_transfers - 1`
transfers (`sync_worker.c`; here 31 of 16,384 samples, 8.3 ms at
61.44 Msps, with 256 buffers, 32 in flight): whether a scheduled timestamp falls inside such a run depends on
the pause modulo that. With the read scheduled 20 ms ahead instead of 5,
nothing failed at any pause (66 to 86 ms, 0 of 89 to 98 bursts, both
images). In the disk runs, every too-late burst followed a pause longer than
the 59.7 ms the buffers hold.

**Disk, alternating, C first** (one file rewritten, 2 min each):

| Order | Image | failed / bursts (too late) | longest write | pauses > 59.7 ms |
|---|---|---|---|---|
| 1 | C | 1 / 919 (0; the first burst) | 54.5 ms | 1 |
| 2 | A | 14 / 912 (13) | 134.2 ms | 32 |
| 3 | C | 3 / 915 (2) | 93.5 ms | 6 |
| 4 | A | 3 / 910 (3) | 95.6 ms | 10 |

So it is the disk, not the image: the earlier C-only failures came with C's
runs having the long writes, and here A had them. Writes to tmpfs: 1 of 909
(A) and 1 of 912 (C), both the first burst.

**The user's patterns, back to back** (`-N`, nothing scheduled, a new file
per burst):

| Pattern | A | C |
|---|---|---|
| 16-bit, 61.44 Msps, 4 × 1 Mi-sample buffers, 2 transfers (34.1 ms), synchronous writes, 2 min × 2 | 2 / 1,202 and 2 / 1,202 lost | 2 / 1,202 and 4 / 1,204 lost |
| 8-bit, 100 Msps overclocked, same buffers (21 ms), 2 min | 3 / 1,202 lost | 5 / 1,204 lost |
| the same three without files (16-bit also with radar_recorder's 64 × 8,192 / 16 buffers), 1 min each | 0 lost | 0 lost |

At 61.44 Msps every loss was one of the first two to four bursts after the
stream started (probably the start-of-stream disorder of section 7's
`rx_order`, which with metadata shows up as an overrun); after that, 1,200 bursts per run
arrived complete on both images, with writes of up to 21 ms. At 100 Msps
each loss followed a write of about 33.5 ms, longer than the 21 ms the
buffers hold at that rate. More buffers would absorb both.

### 11. The micro's other revisions: wlan, adsb, foxhunt (builds, 2026-11-14)

Built with Quartus Prime Lite 25.1 (Nios V) from `niosv`, at 5233ad4e (wlan,
adsb) and fe4bb32d (foxhunt), with Nuand's code (master 35174c30) built with
23.1.1 (Nios II) as the reference, same fitter seed:

| Revision | Build | Logic | Worst setup | Worst recovery |
|---|---|---|---|---|
| wlan xA9 | Nuand, 23.1.1, Nios II | 35,762 ALMs (31 %) | +0.219 ns | −3.630 ns |
| wlan xA9 | ours, 25.1, Nios V | 36,536 ALMs (32 %) | +0.600 ns | −3.803 ns |
| adsb xA4 | Nuand + adsb fix, 23.1.1 | 15,884 ALMs (86 %) | −2.814 ns | +0.914 ns |
| adsb xA4 / xA5 / xA9 | ours + adsb fix, 25.1 | 16,433 (89 %) / 16,735 (58 %) / 16,718 (15 %) | −1.737 / −2.526 / −1.938 ns | +0.526 / +0.990 / +0.073 ns |
| foxhunt xA4 | Nuand, 23.1.1, Nios II | 5,614 ALMs (30 %) | +1.438 ns | +3.208 ns |
| foxhunt xA4 | niosv fe4bb32d, 23.1.1, Nios II | identical to Nuand's | | |
| foxhunt xA4 / xA9 / xA5 | niosv fe4bb32d, 25.1, Nios V | 6,667 (36 %) / 6,641 (6 %) / 6,645 (23 %) | +1.046 / +0.427 / −0.012 ns | +3.866 / +4.004 / +3.348 ns |

- **wlan** needs Nuand/bladeRF-wiphy (8623814) next to the bladeRF tree and
  its IP generated (`fpga/ip/generate.sh`): the FFT and Viterbi cores
  regenerate with 25.1 (19.1 → 25.1, also 23.1) and the image needs no
  OpenCore Plus tethering. The recovery violation is Nuand's: in both builds
  every failing endpoint (564 in Nuand's, 567 in ours, TNS −1,887 / −1,904
  ns) is reset by `wlan_rx`'s `rst_rr(0)`, a reset synchronized to the
  80 MHz clock and used as the asynchronous reset of 40 MHz (sample clock)
  logic (upstream issue [bladeRF-wiphy #16]; `recovery-groups.tcl`).
- **adsb** has not built since 2025.05 (upstream issue [#1117]): it needs
  Nuand/bladeRF-adsb in `hdl/fpga/ip/nuand/adsb` and two one-line fixes
  (branch `adsb-fix`), applied to the working trees for these builds.
  The setup misses are in the ADS-B decoder, filters and preamble detector
  at the sample clock's 125 MHz constraint, worst in Nuand's own build;
  bladeRF_adsb runs at 16 Msps (32 MHz).
- **foxhunt** with Nios II from our branch generates the same Qsys system as
  Nuand's (no line of `nios_system.qsys` differs) with the same fit and
  timing; only the firmware differs (the VCTCXO tamer fix). Its Nios V
  firmware is 82.6 KB (43.0 KB of the RAM left). The xA5's −0.012 ns is the
  AD9361 RX LVDS capture path again (section 2); Nuand's foxhunt xA5 (the
  bench build below) misses it by 0.112 ns.
- wlan and adsb were built only (no ADS-B or Wi-Fi signals on the bench).
  On 2026-11-19 both ran on the xA9 through the AD9361's loopback (section 17).

### 12. foxhunt on the bench: Nios II against Nios V (2026-11-14)

`bin/foxhunt-check.py` (README.md): the B200mini records 75 s from just
before the load and decodes the Morse code. Nuand's foxhunt firmware
transmits on TX1, which the bench terminates, and switches on the TX bias
tee, which would put DC on the cabled port, so both images under test were
built with `setup/foxhunt-tx2.patch` (TX2, through 20 dB to the B200mini;
bias tee off): N2 = Nuand master + patch, 23.1.1, Nios II; V = niosv fe4bb32d
+ patch, 25.1, Nios V. Job `after7` and a trial run
(`foxhunt/1114-1714`, `foxhunt/trial-1114-1647`; numbers from
`--analyze` of the saved captures, harness 1657f494):

| Run | Messages decoded | Dot | Tone from carrier | Tone level | TX on per message | TX off | Message period |
|---|---|---|---|---|---|---|---|
| N2 trial | 3 × "PARIS PARIS" | 120.3 ms | 937.05 Hz | −23.8 dBFS | 13.02 s | 14.0 s | 27.016 s |
| N2-1 | 3 × "PARIS PARIS" | 120.3 ms | 936.98 Hz | −23.8 dBFS | (leakage too weak) | | 27.016 s |
| V-1 | 3 × "PARIS PARIS" | 120.3 ms | 936.77 Hz | −23.8 dBFS | 13.02 s | 10.0 s | 23.016 s |
| N2-2 | 3 × "PARIS PARIS" | 120.3 ms | 937.01 Hz | −23.8 dBFS | 13.02 s | 14.0 s | 27.016 s |
| V-2 | 3 × "PARIS PARIS" | 120.3 ms | 936.78 Hz | −23.8 dBFS | 13.02 s | 10.0 s | 23.016 s |

- Everything the FPGA times is identical: the text, the dot length (dash 2.98
  dots, gaps 119.4 ms: the tone starts and stops at zero crossings), the
  tone and its level, 80 dB above the gaps, and TX on for 13.02 s per
  message.
- Both images key twice as fast as the firmware asks: 120 ms dots (10 WPM)
  and 0.5 s pauses for its 240 ms (5 WPM) and 1 s. The tone generator counts
  durations in TX sample clock cycles, which run at twice the 768 ksps
  sample rate. The tone is right (937.5 Hz: the firmware's 1 kHz rounded to
  the generator's phase step).
- The one difference is CPU timing: TX stays off for 14.0 s on Nuand's
  Nios II/e where the firmware asks `usleep()` for 10 s, and for 10.0 s on
  the Nios V, whose HAL times usleep with the machine timer. The first tone
  also comes earlier after the load on V (3.27 s against 3.74 s from the
  start of the load; the firmware's first `usleep(1 s)`).
- The carrier drifts by 8 to 30 Hz over a capture against the B200mini's
  clock (the firmware writes a fixed VCTCXO trim, 0x1ec1); the tone offset
  is measured against the carrier at the same moment.
- Each image transmits until the next load; `recover.sh` put the official
  image back afterwards.

### 13. Channel 1 at RF (2026-11-15)

Every earlier RF test used channel 2. For these runs the two B200mini cables
moved to channel 1 (TX1 → 20 dB → B200mini RX2, B200mini TX/RX → 20 dB →
RX1, loads on TX2 and RX2) and `HWTEST_RF_CH=1` made the RF scripts use RX1
and TX1: libbladeRF main, both tuning modes, images A and C. Campaign `ch1`
(harness 3e48329e, `results/{A,C}-ch1/`: phases rf, formats, oc), repeats of
its two single failures (`results/ch1-repeat-1115-0834/`), a repro of one
of them (`results/tx433-1115-0849/`), and the RF tests again with the
corrected harness (14642e9e, `results/{A,C}-ch1b/`; the digital tests,
which do not depend on the cabling, skipped).

**C against A on channel 1: the same as on channel 2.**

- rf-check (100 MHz to 5.8 GHz, both directions, both tuning modes): the
  tone is found at every point; C is within 1.0 dB of A in level at every
  point and the same in frequency error, image and LO leakage, noise floor.
- Transmit in every sample format at full rate (`fmt_txrf`): 15 of 15 cases
  pass on both images.
- Overclocked (122.88 Msps): the pulse cases fail on both images, as on
  channel 2 (section 7); the transmitted ones now on a few PRIs out of
  thousands (6 to 11 per case on either image), with the carrier right.
- The first run (`ch1`) had two single failures on C, both explained:
  - TX at 433 MHz, host tuning mode: the tone 26 dB low and 1.4 kHz off.
    This was the start-up of the transmission (below); with the corrected
    measurement the point read normally 8 times out of 8 on C and on A.
  - `fmt_txrf_sc16meta_x2-host` (two channels at 30.72 Msps): 1,458
    "dropouts". Repeated 6 times per image with the old check: C passed 6
    of 6, A 5 of 6 (351 dropouts once), and with the corrected check both
    pass: the dropout check had counted the gaps of the start-up bursts.

**Channel 1 against channel 2, the same with A and C (so the board, not the
FPGA)**, host tuning mode, `ch1b` against `tx`:

| | Channel 1 | Channel 2 |
|---|---|---|
| TX tone at 100 MHz | −45.3 / −45.4 dBFS (A / C) | −31.9 / −32.0 dBFS |
| B200mini noise floor while TX transmits at 100 MHz | −99.1 / −98.7 dBFS | −115.4 / −115.3 dBFS |
| RX tone at 5.8 GHz | −55.0 / −55.1 dBFS | −51.7 / −51.7 dBFS |
| RX image rejection at 100 MHz | −56 dBc | −42 dBc |

Elsewhere the two channels are within 1.6 dB of each other. A finer sweep of
channel 1 (host mode, 2 Msps, `bin/rf-lowband.py` with rf-check's checks; A and C
within 0.2 dB of each other) shows a dip in TX1's output around 100-125 MHz,
not a roll-off; RX1 is flat:

| MHz | 70 | 85 | 100 | 125 | 150 | 200 | 250 | 300 | 433 |
|---|---|---|---|---|---|---|---|---|---|
| TX1 tone (dBFS) | −34.8 | −43.0 | −45.6 | −46.8 | −39.2 | −36.2 | −33.4 | −32.2 | −32.6 |
| RX1 tone (dBFS) | −11.3 | −11.9 | −11.5 | −12.3 | −11.6 | −12.7 | −12.1 | −12.6 | −12.2 |

Channel 2 was measured at 100 and 433 MHz only. A two-channel transmit
check through a combiner (twoch-check.py) can compare TX1 and TX2 directly.

**Start-up of a transmission (A and C alike).** In 24 receive-then-transmit
sequences on channel 1 (`bin/tx-startup.py`: 433 MHz, host tuning mode, 12
per image), the first
one to three 50 ms B200mini captures after the tone appeared read it 10 to
25 dB low in 7 runs on A and 2 on C. Inside such a capture there are bursts
of 5 to 10 ms, about 13 dB below the tone and about 1.5 kHz below its
frequency, with gaps between them, then the steady tone. The bursts come at
the start of each bladeRF transmission, up to about 0.7 s long; the B200mini
reported its receiver locked throughout, and all 666 captures after them
read the tone normally, at the same frequency. The cause was not
investigated. rf-check.py, pulse-check.py and twoch-check.py now measure a
transmission 1 s after its signal first appears (`TX_SETTLE_S`, 14642e9e);
before, a measurement could land in the bursts.

### 14. Nuand's threads test as Nuand runs it, and what made it fail (2026-11-15 to 17)

`test_ctrl -T fpga -t threads` streams RX and TX through the FX3's firmware
loopback while four threads change gains (75 sweeps), bandwidths (25
sweeps), frequencies (one sweep) and the IQ correction. It ran as Nuand's
release checklist runs it: `bin/ctrl-threads-nuand.sh`, with no harness
preload, no TX cap and no time limit (`HWTEST_TX_UNCAPPED_OK=1`; TX1 ->
20 dB -> 20 dB -> B200mini RX2, the other ports on 50 ohm loads). `FAST=1`
runs the checklist's fast variant (`-f`); `TRACE=1` records every Nios
request and response (`lib/nios-trace.c`, analysed by
`bin/nios-trace-threads.py`; since 153e9563 also the frequency calls and
the sample transfers; 1d302c3a moved its logging to a ring buffer and a
writer thread, to disturb the threads' timing less: checked with its
self-test, 1 million records, and receive runs in both tuning modes,
nothing dropped, but not yet used for a threads run). The FPGA images and
the FX3 firmware (2.6.0) are the same throughout; only the host software
differs. Results under `results/ctrlthreads-nuand-*`.

**With Nuand's libbladeRF and test.**

- Full variant on A (`fullA-1115-1350`): fails after 8,473 s with 48,524
  failures, 48,425 of them wrong gain readbacks, stuck at 11 and 2 whatever
  gain was set, and 7 Nios commands that timed out. At the start a
  configuration-register read during stream set-up got no reply; the RX
  stream reported no failure, so it most likely never started, and with
  nothing receiving the TX stream stalled in the loopback and failed. RX
  gain changes do not register while RX is disabled. For the first 15
  minutes the bandwidth thread held the device lock almost continuously.
- Fast variant on A, traced (`trace-main-1115-1616`): the bladeRF dropped
  off USB after 57 s and came back only after a power cycle. Until then, 127
  replies took longer than 250 ms (126 status polls during bandwidth
  changes, 1 during RFIC initialization), and the RX stream never started,
  so the TX stream stalled in the loopback and timed out. A status reply
  that came after its poll had timed out was then taken as the answer to
  the next request, the replies ran one behind, a configuration-register
  read got a bandwidth reply (0x52c5ff01), libbladeRF wrote that back
  modified (0x52c4ff01; the register held 0x00000001), and the next USB
  transfer failed with an I/O error.

**A long Nios command alone does no harm.** The Nios answers one request at
a time and none while it executes an RFIC command, and some commands take
longer than libbladeRF's 250 ms (`PERIPHERAL_TIMEOUT_MS`):

- a bandwidth change always runs the AD9361's TX quadrature calibration
  (`ad9361_update_rf_bandwidth()`). When that converges neither with the
  default phase nor with the last one, `ad9361_tx_quad_phase_search()` runs
  it for all 32 phases, about 230 ms more. Bandwidth changes took 23-32 ms
  median, up to 273 ms on A and 243 ms on C; 311-529 per run reached 250 ms
  on A, none on C;
- RFIC initialization took 397-672 ms.

libbladeRF waits for a command by polling the Nios's work queue
(`_rfic_fpga_spinwait()`) and counts a poll that timed out as not done, so it
polls again and the command completes. That is harmless as long as the late
reply to the timed-out poll is not taken as the answer to a later request
(3a768f3d) and no other request goes out meanwhile (06962e80). A 1 s
time-out was tried and dropped: with 250 ms and the fixes, 308
status polls timed out in a run on A, the 13 late replies were discarded,
and the test passed (table).

**What made it fail.** All in the host software:

- 3a768f3d: late replies were taken as the answer to the next request.
- 06962e80: `bladerf_get_gain_range()` and `bladerf_get_gain_stage_range()`
  sent Nios requests without the device lock. The gain thread's lookups
  went out while another thread's request was unanswered: once in 10,935
  requests and 3 times in 852,185 without the lock, never in 5.8 million
  with it. Twice the other request, a configuration-register read, was
  never answered.
- dc41000f: negative RX gains read back as -2147483648 in FPGA tuning mode.
- 378b9909 (test): the control threads started before the streams had
  enabled RX and TX.
- 15299a8b (test): gain readbacks were checked against the 420 MHz gain
  range while the frequency thread had moved the channel to another band.

**Fast variant, traced, with the fixes** (`trace-*-1116-*`; time-outs are
status polls that got no reply in time, all retried):

| Build | Image | Result | Failures | Requests | Time-outs | Late replies discarded | Notes |
|---|---|---|---|---|---|---|---|
| host-fixes + 06962e80, 1 s | A | fail | 163 | 852,040 | 0 | 0 | 141 readbacks of 60 while RX was not yet enabled, 22 of -2147483648 |
| host-fixes + 06962e80, 1 s | C | fail | 47,995 | 833,260 | 0 | 0 | RX stream started 45 s after TX; both streams timed out, RX disabled for the rest |
| Nuand's + 1 s | A | fail | 48,660 | 852,185 | 2 | - | 3 requests sent while another was unanswered, 2 never answered |
| above + dc41000f + 378b9909, 1 s | A | FX3 hung at 498 s | 3 before the hang | 731,259 | 0 before the hang | 0 before the hang | the 3: readback at a band's minimum gain (15299a8b) |
| all fixes, 1 s | C | pass | 0 | 1,025,375 | 0 | 0 | |
| all fixes, 1 s | A | pass | 0 | 1,025,353 | 0 | 0 | |
| all fixes, 250 ms | C | pass | 0 | 1,025,145 | 1 | 0 | during RFIC initialization |
| all fixes, 250 ms | A | pass | 0 | 1,025,670 | 308 | 13 | 307 during bandwidth changes, 1 during initialization |

"All fixes" is `host-fixes`' libbladeRF (3a768f3d, 2ac75f37, dbad1e70,
e64e4026, 06962e80, dc41000f) with its test_ctrl fixes 378b9909 and
15299a8b; the runs used the same changes under earlier commit IDs.

**Full variant with all fixes and 250 ms** (`full-keep250-1116-0840`,
untraced; the variant Nuand's checklist runs):

- C: pass, 7,176 s, no failures.
- A: one failure in 7,653 s. Near the end of the frequency thread's random
  RX tuning, after the other three threads had finished, one frequency read
  (`bladerf_get_frequency()`, an RFIC read) got no reply within 250 ms.
  About 600 tunings later both sample streams failed: RX and TX transfers
  that had waited 10 s timed out. Only the frequency thread was using the
  device then, and frequency commands took at most 27 ms in the fast
  traces, so the Nios should not have been busy for 250 ms. At the fast
  traces' 6-7 ms a tuning, that puts the streams' stop a few seconds before
  the read's time-out. The test streams through the FX3's firmware loopback,
  which does not involve the FPGA (about 5,500 transfers of 64 KiB a second
  each way, some 360 MB/s, in the rerun's trace), and the Nios's requests
  and replies pass through the FX3 too. Every later Nios request was
  answered, and the kernel logged no USB error. Not explained; one of the
  three stops counted below.

**Full variant again, traced** (`full-trace-keep250-1116-1259`, with the
sample transfers and frequency calls in the trace):

- A: 54 failures in 7,766 s, every one a frequency readback of 420 MHz (the
  test's race below). No stream stalled: the longest gap between RX
  transfers was 34.5 ms. 15,904 replies timed out (15,903 status polls
  during bandwidth changes, polled again, and 1 during RFIC
  initialization); 745 late replies were discarded.
- C: after 3,687 s both streams stopped, and then the FX3 stopped
  answering (below).

**The test's own race: test_gain's 420 MHz** (not changed on `host-fixes`;
upstream [#1137]). Before it tests a channel, test_gain sets it to 420 MHz,
a 2018 workaround for FPGA v0.10.2 (d1a9c8e1). In the threads test the gain
thread does that 75 times per channel while the frequency thread sets and
reads back frequencies on the same channels, and RX1 and RX2 share the
AD9361's RX LO (TX1 and TX2 its TX LO). A 420 MHz set that lands between the
frequency thread's set and its readback fails the readback: "Frequency
(4951000000) != Readback value (420000000)". The gain thread always finished
before the frequency thread's TX sweep, so only RX readbacks failed. Seen in
0 of 2 untraced full runs behind the hub, in 4 of 7 with the bladeRF on its
own port (6 failures, both images; next paragraph) and 54 times in the
traced run on A. Whether the workaround is still needed is for Nuand to say:
with FX3 firmware 2.5.0 or later, libbladeRF refuses FPGAs older than
v0.16.0.

**Behind the hub and on its own port.** From 2026-11-13 the bladeRF was
behind the USB 3 hub in the laptop's USB-C adapter, which also carries the
laptop's wired network adapter and is powered by its charger. On 2026-11-16
at 22:07 it moved to the laptop's other USB-C port (2-3, SuperSpeed). Seven
untraced full runs there, A and C in turn, with all fixes and 250 ms
(`results/ctrlthreads-nuand-direct-*`):

| Run | Image | Result | Seconds | Failures |
|---|---|---|---|---|
| 1 | C | fail | 6,857 | 1, the race |
| 2 | A | fail | 7,480 | 1, the race |
| 3 | C | pass | 6,756 | 0 |
| 4 | A | pass | 7,710 | 0 |
| 5 | C | fail | 6,449 | 2, the race |
| 6 | A | pass | 7,700 | 0 |
| 7 | C | fail | 6,821 | 2, the race |

In these 13.8 h no stream stopped, no RFIC command timed out and the FX3
always answered. Behind the hub the runs with the fixes had stopped 3 times
in 8.1 h on 2026-11-16 (from the 07:22 run in the table on, which lacked
15299a8b; a run that stopped counted up to its stop): the FX3 at 07:30 (A)
and at 16:11 (C), and the streams of the full run on A. If the hub made no
difference, all three stops would fall in the hub's hours with a
probability of (8.1/21.9)^3, about 0.05. The move also changed the
bladeRF's power source (the charger through the adapter, then the laptop's
port), and the hub's hours include fast and traced runs, so this points at
the hub or its power but does not separate them.

**RX gain readbacks** (`lib/gain-readback.c`, RX only: 14 gains set and read
back on RX1 at 420 MHz, 2 GHz and 5 GHz): with Nuand's libbladeRF in FPGA
tuning mode the 5 gains with a negative full-stage value read back as
-2147483648, on A and on C; host tuning mode reads all 14 correctly; with
dc41000f all 14 are correct in both modes on both images.

**Not explained: the FX3 stopped answering**, four times, always behind the
hub. Three times on A: on 2026-11-13 (25 s into a capped run), on 2026-11-15
at 16:17 (after the bad configuration-register write above), and on
2026-11-16 at 07:30 (498 s into the run in the table, with no such write:
one reply came 1 s late, 250 more requests got none, both streams stopped,
and then the FX3 answered neither USB control requests nor device-descriptor
reads). Once on C, on 2026-11-16 at 16:11, 3,687 s into the traced full run:
both loopback streams stopped first, the Nios answered for 9.8 s more, then
nothing did; a raw libusb probe got no answer to control requests and I/O
errors on the bulk endpoints. Each time only a power cycle or a replug
brought it back.

**At the end of every run** "TX failed ... Transfer timed out for TX buffer"
is printed after the four threads have joined: test_threads stops the RX
stream first, and with the firmware loopback the TX stream then stalls.
test_threads does not count it.

### 15. A second board: the xA5's baseline (2026-11-17)

To see whether the results depend on the board, the same plan runs on this
xA5 and on an xA9 (`plans/unit-xA5-20261117.txt`, `unit-xA9-20261118.txt`):
smoke, host, fpga, formats, oc, trigger, sched and bist on A, B and C, then
deep, fullsoak and soak on A and C. The bladeRF on its own USB-C port (2-3),
the B200mini off USB, the last cabling row, `HWTEST_TX=1`, libbladeRF and
tests of `host-fixes` (15299a8b) with `HWTEST_FIXES=1`; the tests that need
the B200mini (fmt_txrf, oc_pulse) and ctrl_threads-fpga left out (results
`results/xA5-*-u1*/`, 12:59 to 23:16):

| Run | A | B | C |
|---|---|---|---|
| smoke to bist | 134/148 | 135/148 | 133/148 |
| deep | 6/6 | | 6/6 |
| fullsoak | 4/4 | | 4/4 |
| soak, 90 minutes | 632/632 | | 667/667 |

Every failure is one seen before, or gave no result:

| Failure | A | B | C |
|---|---|---|---|
| fmt_txrf, two channels, FPGA tuning mode, five formats: no result (the plan did not leave them out, and they need the B200mini: "No devices found"; the xA9's plan leaves them out) | 5 | 5 | 5 |
| fmt_loop, SC16, two channels, 4 Msps, both tuning modes (the loopback's limit, section 7) | 2 | 2 | 2 |
| bist overclocked: 76.8 Msps on RX1 and RX2, 122.88 Msps on RX1 (section 8) | 3 | 3 | 3 |
| sched: SC16 meta with one and two channels, SC8 meta with two (below) | 3 | 3 | 3 |
| oc_rx, SC8 meta at 122.88 Msps, both tuning modes (sections 7 and 9) | | | 2 |
| rx_meta, two channels, host tuning mode: "Requested timestamp is in the past" (the case of open item 1, 16 ms of slack) | 1 | | |

New in this run: the formats phase with two channels in FPGA tuning mode
(with `HWTEST_FIXES=1` only, as that needs 2ac75f37). Full-rate receive in
all five formats and the SC8 loopback pass on A, B and C; the SC16 loopback
fails as in host tuning mode.

**sched**, the first time in a campaign (60 s per case, every burst written
to a file in the results directory):

| Case (the buffers last) | A: bad / bursts | B | C |
|---|---|---|---|
| SC16 meta, one channel, 61.44 Msps (59.7 ms) | 1 / 462 | 4 / 459 | 8 / 462 |
| SC8 meta, one channel, 61.44 Msps (119.5 ms) | 0 / 482 | 0 / 483 | 0 / 482 |
| SC16 meta, two channels, 30.72 Msps (59.7 ms) | 5 / 461 | 2 / 454 | 4 / 458 |
| SC8 meta, two channels, 61.44 Msps (59.7 ms) | 4 / 408 | 3 / 409 | 3 / 409 |

Each bad burst was either the first of the one-channel SC16 run, on every
image (an overrun at the stream's start, open item 6), or scheduled too late
after a pause between reads longer than the buffers last (section 10: the
disk). In the SC8 one-channel case the longest pause was 78 ms, within its
119.5 ms.

**deep, fullsoak, soak:**

- deep (FPGA tuning mode): A frequency 487 s, bandwidth 314 s, sample rate
  3,975 s; C 405, 275 and 3,785 s. Frequency and bandwidth as in section 3;
  the sample-rate sweep took 15 % (A) and 19 % (C) longer than there, on
  both images alike. The libbladeRF build and what the RF ports saw
  (antennas then) differ from section 3 (the USB port is the same, 2-3);
  not investigated.
- fullsoak: receive, SC16 at 61.44 Msps for 60 minutes: A 221,183,459,328
  and C 221,184,507,904 samples, none missing, duplicated or late. Loopback,
  SC16 set to 30.72 Msps, 30 minutes: A 73,721,479,168 and C 73,722,593,280
  samples back, 0 errors. A's loopback stall of section 7 did not come back
  (on A one stall in three 30-minute runs, on C none in three).
- soak (both tuning modes): A 63 rounds, C 66 to 67, no failure; C again
  faster on Nios-bound tests (ctrl_frequency in FPGA tuning mode 9 s per run
  against 11 s).

### 16. The xA9: the same plan on a second board (2026-11-18/19)

An xA9 (rev 1.3, 301 KLE, a 128 Mbit flash with the official v0.16.0 as its
autoload image, FX3 firmware 2.6.0) ran section 15's plan with the xA9
builds of the same images (`plans/unit-xA9-20261118.txt`, `images-xA9/` and
its MANIFEST.txt, `HWTEST_FPGA=A9`), on the same USB-C port (2-3), with the
same cabling, host build and `HWTEST_TX=1`. The plan also leaves out the
five fmt_txrf runs the xA5 could not run without the B200mini. First a
read-only inventory (`results/inventory-20261118-183727/`): the whole flash
was byte-identical to the backup made on the MacBook Air at 11:10 the same
day (open item 10). Results `results/xA9-*-u1*/`, 18:45 to 04:39:

| Run | A | B | C |
|---|---|---|---|
| smoke to bist | 138/143 | 136/143 | 140/143 |
| deep | 6/6 | | 6/6 |
| fullsoak | 4/4 | | 4/4 |
| soak, 90 minutes | 637/637 | | 672/672 |

The xA5's smoke-to-bist runs without those five results: A 134/143, B
135/143, C 133/143. Every failure on the xA9 is one seen before, and C
failed only tests A failed too:

| Failure | A | B | C | xA5: A, B, C |
|---|---|---|---|---|
| fmt_loop, SC16, two channels, 4 Msps, both tuning modes (the loopback's limit, section 7) | 2 | 2 | 2 | 2, 2, 2 |
| bist overclocked, 122.88 Msps on RX1 (section 8) | 1 | 1 | 1 | 1, 1, 1 |
| sched: bursts scheduled too late after a long pause between reads (section 10), or the first burst overrunning (open item 6) | 1 | 3 | | 3, 3, 3 |
| rx_meta, 8-bit, an overrun: one channel (A, open item 6), two channels (B, open item 1) | 1 | 1 | | |
| rx_meta, 16-bit, two channels: "Requested timestamp is in the past" (open item 1) | | | | 1, 0, 0 |
| bist overclocked, 76.8 Msps on RX1 and RX2 (below) | | | | 2, 2, 2 |
| oc_rx, SC8 meta at 122.88 Msps, both tuning modes (sections 7 and 9) | | | | 0, 0, 2 |

**The one difference: overclocked receive at 76.8 Msps.** The AD9361's PRBS
over the LVDS interface at 307.2 MHz (section 8), 20 s per channel; bad
samples of 1,552,777,216:

| | A | B | C |
|---|---|---|---|
| xA9, RX1 / RX2 | 0 / 0 | 0 / 0 | 0 / 0 |
| xA5, RX1 / RX2 | 78 / no pattern | 95 / 41.4 M (2.7 %) | 101 / no pattern |

On the xA5 every bad sample had the same bit wrong (bit 3 of SC8, bit 7 of
the 12-bit sample), as in section 8: one data lane fails first there, with
every image. The xA9 has no bad sample with any image. Its builds close
timing with at least 0.52 ns of setup slack on every path (MANIFEST.txt),
the xA5's with −0.154 ns (A) and −0.110 ns (C) on the LVDS capture (section
2), but its FPGA is another device (301 KLE against 77), so this does not
tell the board from the FPGA. 76.8 Msps is beyond the AD9361's 61.44 Msps
either way, and at 122.88 Msps neither board receives anything usable.

**deep, fullsoak, soak:**

- deep (FPGA tuning mode): A frequency 487 s, bandwidth 317 s, sample rate
  3,336 s; C 403, 281 and 3,017 s. Frequency and bandwidth as on the xA5;
  the sample-rate sweep took 16 % (A) and 20 % (C) less time than on the
  xA5 in section 15, and 3 to 5 % less than on the xA5 in section 3. So the
  xA5's slower sweep in section 15 is not the build's or the cabling's
  alone; not investigated.
- fullsoak: receive, SC16 at 61.44 Msps for 60 minutes: A 221,183,688,704
  and C 221,183,934,464 samples, none missing, duplicated or late. Loopback,
  SC16, 30 minutes: A 73,723,707,392 and C 73,720,922,112 samples back, 0
  errors.
- soak (both tuning modes): A 63 to 64 rounds, C 67, no failure;
  ctrl_frequency in FPGA tuning mode 9 s per run on C against 11 s on A, as
  on the xA5.

**After the campaign: the FX3 lockup on Linux.** Open item 10's reproducer
(a two-channel receive started in FPGA tuning mode in bladeRF-cli's order),
receive only, with libbladeRF from Nuand's master (`host-build`, 35174c30)
and the official image: the FX3 locked up on the 6th run (04:39), the same
way as on the MacBook Air. Enabling RX2 made libbladeRF send RF_RX(0) under
the running RX1 stream; that request timed out, and from then on GET_STATUS
and every descriptor read timed out, with the board still enumerated
(`results/repro-linux-20261119-043858/`, `hang-evidence.txt`). So the
lockup does not depend on the host. On Linux every open resets the USB port
("USB port reset succeeded"), and the board came back from each of those
resets until the lockup. After the replug (06:27) the whole flash was
byte-identical to the 18:37 inventory (`inventory-20261119-062733`).

### 17. The xA9 the next day: the FX3 fix on Linux, how fast it samples, the other revisions (2026-11-19)

The xA9 on the same port and cabling (the last cabling row: TX1 through
40 dB to the B200mini, which stayed off USB; the other ports on loads).

**The FX3 endpoint-NAK fix on Linux** (`results/nak-linux-20261119-0628*`;
the user approved writing the test firmware and restoring the official one).
The 2.6.0 + NAK image tested on the MacBook Air (fork `fx3-epnak`, 46fa04d1,
SDK 1.3.3 libraries, md5 1a179190…, version 2.6.0-epnak-46fa04d1) was written
to the flash's firmware area with `bladeRF-cli -f` (3 erase blocks, 484 pages,
read back and compared), the FX3 rebooted from the flash
(`bladerf_device_reset`) and the FPGA autoloaded as usual. Then:

- the lockup trigger (open item 10), receive only, Nuand's libbladeRF, FPGA
  tuning mode: 60 bladeRF-cli runs (20 each with A, C and B) and 20 bad-order
  runs of `rx2order` after 3 good-order ones: no lockup in 80 starts (the
  official 2.6.0 locked up on the 6th the night before). Every stream still
  stopped after at most one buffer: that is libbladeRF's part (open item 10,
  fixed on `host-fixes` by 2ac75f37).
- normal use: section 16's run 1 again (A, smoke to bist): 143 tests, 5
  failed, the three that fail everywhere (fmt_loop SC16 x2 4M in both modes,
  bist_oc 122.88 Msps) and two known noise cases (rx_meta_8bit_mimo-host, an
  overrun; rx_meta_mimo-fpga, "Requested timestamp is in the past", also seen
  with the official firmware on 11-13 and 11-17).
- the official 2.6.0 written back and the FX3 rebooted: the whole flash
  byte-identical to the 18:37 inventory (07:10).

**How fast the xA9 samples, receiving and transmitting**
(`results/ratesweep-20261119-071039`). Overclocked
(`BLADERF_FEATURE_OVERSAMPLE`, SC8), host tuning mode. libbladeRF sets the
AD9361 up for half the rate and then doubles its output (register 0x003 =
0xD4: both RX channels on, RHB3 and RHB1 decimating by 2, RHB2 and the FIR
off; 0x002 = 0xC0). Both RX channels stay on, so the interface runs 2R2T and
its DATA_CLK is four times the sample rate (0x010 = 0xC8: the 2R2T-timing bit
is not set; the channel enables decide): the FPGA's counter, which runs at the
FPGA's RX clock / 2 = DATA_CLK / 4, measured 122.877 Msps at 122.88
(`results/ocexplore-*`, register dumps at 61.44, 76.8 and 122.88 Msps).

- Receive, BIST check, 10 s per rate on RX1 and RX2: A and C clean to
  108 Msps, B to 104. The first bad samples were always in bit 3 of SC8
  (bit 7 of the 12-bit sample), the lane that fails first on the xA5 too; at
  122.88 Msps 8 to 9 % of the samples were bad. 120 s per channel: A and C at
  108 Msps, 1 bad sample in 1.3 × 10^10 on RX1 and none on RX2; B at
  104 Msps none. (The xA5 is clean to 68 Msps, section 9.)
- Transmit alone (`hwtest-stream tx`, SC8, a 1 MHz tone on TX1, TX gain
  0 dB): libbladeRF took 100.7 to 101.4 % of the sample rate up to
  122.88 Msps with A and C (the rest is the first buffers filling): host, USB
  and FX3 keep the DAC fed. Nothing receives these samples, so this checks
  throughput only.
- Transmit with every sample checked (`hwtest-stream loopback -A`: numbered
  samples out, through the AD9361's data-port loopback, back in and checked):
  clean to 80 Msps on both channels with A and C; from 84 Msps single bits
  flipped in fixed positions while the full rate still arrived, so the
  transmit side of the interface fails there, not the throughput. In this
  mode `ad9361_tx_quad_calib` reports "Unhandled case ... clkrf 40000000
  clktf 80000000".
- Why the boards differ: the same design and constraints, but the A9 die
  gives the LVDS receiver's slow clock domain (`pll_sclk`) about 1 ns more
  setup margin. In Nuand's own v0.16.0 builds (Quartus 23.1.1) that slack is
  +0.076 ns (slow 85 °C) and −0.154 ns (slow 0 °C) on the xA5, +1.010 and
  +1.092 ns on the xA9.

**The RX interface delays** (`results/ocexplore-*`, `results/delayconfirm-*`).
AD9361 register 0x006 holds the DATA_CLK delay (bits 7:4) and the data
delay (bits 3:0); libbladeRF sets 0x50 (DATA_CLK 1.5 ns) and skips the
AD9361's interface tuning. A BIST check of 1 s per setting, DATA_CLK delay 0
to 15 and data delay 1 to 15, on RX1 and RX2:

- 76.8 Msps: the default sits in the middle of a 4-setting window (the
  windows repeat every 7 settings, one bit time).
- 112 Msps: the default is at an edge (errors); 0x40, 0x05 and 0x06 are clean
  on both channels.
- 122.88 Msps: single-setting windows at 0x00, 0x40, 0x90, 0xd0, 0x09 and
  0x0e on both channels, with A and with C.

Then 60 s per channel at 122.88 Msps with A: 0x40, 0x00, 0x90, 0xd0 and 0x09
gave 0 bad samples in 7.37 × 10^9 each, 0x0e 99,243 on RX2; 0x40 for 600 s
per channel: 0 bad in 7.37 × 10^10 on each; with C, 60 s per channel, 0 bad.
So on this board receiving at 122.88 Msps is bit-exact in the stock 2R2T mode
with one register value (0x006 = 0x40 instead of 0x50). The xA5 has not been
mapped.

At 122.88 Msps with RX2 and TX2 off (0x003 = 0x54, 0x002 = 0x40) the FPGA's
counter dropped to 61.44 Msps: in 1R1T the AD9361 halves DATA_CLK to
245.76 MHz, where both boards are clean. The FPGA would need a 1R1T deframer
(`axi_ad9361_dev_if_alt.v` decodes 2R2T only); that is separate work.

**The other revisions with Nios V** (`results/revs-smoke-20261119-084339`,
receive only). wlan built from master with Quartus 23.1.1 (Nios II) and from
`niosv` (5233ad4e) with 25.1 (Nios V), each: loaded, versions read,
frequency, sample rate and bandwidth set and read back in FPGA and in host
tuning mode, and an RX1 capture of 80,000,000 bytes of 80,000,000 in both
modes, the same on both images. adsb built from `niosv` (Nios V): loaded,
set and read back the same way; Nuand's `bladeRF_adsb` ran 30 s without an
error (no messages: the RX ports are terminated). Nuand's published
`fpga/v0.16.0/wlanxA9.rbf` (md5 fb1dc763…, the same as their .md5sum) is FPGA
v0.15.3, which libbladeRF refuses with FX3 firmware 2.6.0 ("Firmware v2.6.0
requires FPGA v0.16.0 or later"). Checked on 2026-11-21: it is byte for byte
Nuand's `fpga/v0.15.3/wlanxA9.rbf` and `fpga/wlanxA9-latest.rbf` (md5
fb1dc76308b64ffe5f2a4146e44c7b79, sha256 9f1fd747…), which the server dates
2024-04-18 (v0.15.3) and 2025-05-11 (v0.16.0). Nuand publishes no wlan image
for the xA4 or the xA5 (none for v0.14.0 to v0.16.0 or latest), and
bladeRF-wiphy names only the xA9, so there is none to check on those boards.

**Nuand's threads test on the xA9** (`results/ctrlthreads-nuand-xa9th-1119-0846`
and `-1140`): `libbladeRF_test_ctrl -T fpga -t threads`, the full variant,
with the all-fixes build (`host-build-threads-keep250`: `host-fixes`'
libbladeRF and test fixes, Nuand's 250 ms Nios time-out), the bladeRF on its
own port. A passed in 7,915 s and C in 7,387 s, each with a failure count of
0. Both ended with the end-of-run TX time-out of section 14.

**The endpoint-NAK fix rebased onto master** (`results/nak-master-20261119-105833`;
the user approved writing the test firmware and restoring the official one).
`fx3-epnak-master` (6aca76fd, FX3 2.6.1 + the NAK change), built like the
official image (version 2.6.1-epnak-6aca76fd, md5 0e43774a…), written and
tested as above: no lockup in 80 starts of the trigger (20 each with A, C and
B, 20 bad-order `rx2order` runs); section 16's run 1 again: 143 tests and the
same 5 failures as with the official firmware; the official 2.6.0 written
back, and the whole flash byte-identical to the 18:37 inventory (11:40).

**Each fix alone** (`results/split-20261119-142639`). Every `pr/*` branch
(upstream master 35174c30 + one fix) exported with its submodules and
hwtest's build-only BUILD_BLADERF_UPDATE option, built alone (0 compiler
warnings each), and checked against master built the same way, with A unless
the row says otherwise:

| Fix | Check | master | the fix alone |
|---|---|---|---|
| [#1122] stale Nios replies | late-reply injector, `test_ctrl -T fpga -t bandwidth` | 19 failures | 0 |
| [#1122] | full bandwidth sweep, FPGA tuning mode | 0 failures | 0 (A and C) |
| [#1123] two-channel enable, FPGA tuning mode | `test_rx_meta -m`, `-b 8 -m`, both modes, A and C | not run (open item 10's trigger) | pass; one start-of-stream overrun (below) |
| [#1124] MIMO layout, host tuning mode | RX1+RX2 receive while TX1 sends 0.2 s | 2,228,224 of 30,000,000 samples | 30,000,000; `test_rx_meta -m`, `-b 8 -m` pass |
| [#1125] RFIC re-init after a load | `test_fpga_load`, host tuning mode | "Calibration TIMEOUT (0x16, 0x80)" | pass (A and C); FPGA mode passes before and after |
| [#1128] gain-range lock | `test_ctrl -t gain -f`, both modes | pass | pass |
| [#1129] gain sign | gain readback check, FPGA tuning mode | 5 of 14 wrong (-2147483648) | 0 of 14 (A and C; host mode 0 before and after) |
| [#1131] gain check range | `test_ctrl -t gain -f`, both modes | pass | pass |
| [#1134] flash page count | `flash_backup` of all 16 MiB in one call | stops: "usb.c:612: usb_read_flash_pages: Assertion `count == count_u32' failed" | identical to the two 8 MiB halves read with master (md5 9cc8724e…) |
| [#1135] flash read error | `flash_backup` of the low 8 MiB | - | identical to master's |

`test_scheduled_retune` (rx, Nuand's hop list, quick tune; every failure
"Requested timestamp is in the past"), runs failed:

| Build | host tuning mode | FPGA tuning mode |
|---|---|---|
| master | 10 of 10 (iteration 1) | 10 of 10 (iteration 1) |
| [#1126] (tune first) | 26 of 30 (15 at iteration 3) | 24 of 30 (iterations 114-150) |
| [#1127] (64 buffers) | 10 of 10 (iterations 1-4) | 0 of 10 |
| [#1126] + [#1127] | 0 of 30 | 0 of 30 |

[#1130] (test only) had no run of its own: alone it cannot pass the threads
test, and the threads runs above include it.

The [#1123] overrun: both FPGA-mode `-m` runs of the split check (A, then C)
reported one "Overrun detected" after 9 of 15 reads, at the same sample
(4,016 valid samples, then a jump to t = 0x10f7b); the stream went on and
the test completed. Then, each right after a load of A: [#1123] alone 12
runs, the all-fixes build 12 runs (alternating order): no overrun
(`results/recheck-*`, `recheck2-*`). The all-fixes build also had none in the
5 campaign runs on the xA9. So 2 of 14 with [#1123] alone, 0 of 17 with all
fixes: open item 1's overrun, not a stop of the stream ([#1109]'s failure).

**A libbladeRF bug found on the way: a timed-out wait reads as an
"unexpected error"** (`results/recheck2-*`). `posix_cond_timedwait()` in
`host/common/include/thread.h` (5cdcd28e, 2023-12; in 2025.05 and later)
returns -1 for a time-out, while its callers compare with THREAD_TIMEOUT
(ETIMEDOUT). A test program (`lib/sync-timeout-check.c`, receive only:
`bladerf_sync_rx()` for one 16384-sample buffer at 1 Msps with a 5 ms
time-out) got -1 ("An unexpected error occurred") in 41 of 41 calls with
master, -6 (BLADERF_ERR_TIMEOUT) in 41 of 41 with the one-line fix (local
branch `pr/cond-timedwait-timeout`). bladeRF-cli's `rx wait 2s` during an
endless receive was still waiting after 20 s with master; with the fix the
whole run took 3.6 s. The end-of-run "TX failed with: An unexpected error
occurred" of the threads test may be the same bug (not checked).

**The adsb revision's decoder, through the AD9361 loopback** (`lib/adsb-loop.c`;
`results/adsb-loop-*`, `recheck-*`, `recheck2-*`, `ab950-*`). Nuand's ADS-B
decoder in the adsb revision (Nios V build), fed a made-up DF17 frame
1,000 times a second through the AD9361's data-port loopback, the messages
read as Nuand's `bladeRF_adsb` reads them. Version 1 of the program sent the
frame at DC and got no message at any level: bladeRF_adsb tunes 4 MHz below
1090 MHz, and the FPGA's front end (`adsb_fe.vhd`) mixes by -Fs/4 and
low-pass filters (-78 dB at 4 MHz), so a frame at DC is filtered out. From
version 2 the frame is sent at +Fs/4 (+4 MHz) and the TX LO is at 915 MHz
(ISM band). Results (20 s, about 20,050 frames per run):

| Run, 20 s at 1,000 frames a second | v2 (one frame) | v3 (reads on 0.3 s after TX stops) | v4 (numbered frames) |
|---|---|---|---|
| TX off | 0 messages | 0 | 0 |
| DC, amplitude 1000 (of 2047) | 0 | 0 | 0 |
| +Fs/4, amplitude 1000 | 20,019 of 20,055 | 20,041 of 20,042 | 20,033 of 20,035 |
| 100 | 20,003 of 20,037 | 20,061 of 20,062 | 20,068 of 20,068 |
| 50 | 20,022 of 20,058 | 20,045 of 20,047 | 20,070 of 20,072 |
| 40 | | | 20,055 of 20,058 |
| 30 | | 0 | 0 |
| 20 | 0 | 0 | |

Every message that came back was exact (v4: no wrong or repeated frame). The
about 35 frames v2 missed were still queued for TX when it stopped reading.
The 0 to 3 that v3 and v4 missed were scattered (v4: frames 9786 and 12517;
2453 and 10586; 15818, 18549 and 20057), not at the start or the end.
Then 60 s runs at amplitude 1000 (v5, `results/after950-*`), with 32 TX
buffers (33 ms) and with 128 (131 ms), alternating: 6, 7, 5 and 7 of about
60,060 frames missing, and the longest gap between `bladerf_sync_tx()`
returns was 1.5 ms in every run, so the host never let the TX buffers run
dry. At amplitude 100 (32 buffers) 5 of 60,043 were missing. The missing
frames follow a fixed pattern in time: 2,731 or 8,133 frames apart (the two
128-buffer runs had the same seven gaps, shifted by 160 frames). A probable cause, from the code (not proven): the adsb
revision's RX FIFO (`adsbfifo`, 512 words of 128 bits) holds exactly one
8 KiB GPIF buffer, and `fx3_gpif.vhd` starts a read only when a whole buffer
is in the FIFO, so the FIFO is full each time a read is about to start, and
a slot written then is dropped. The decoder's message aggregator writes
about 118 padding slots every 10 ms, so those moments drift against the
1 ms frame grid by about 118 clocks (32 MHz) per 10 ms, which brings a frame
into one about every 2,710 frames. Nuand's code does the same
(`results/adsbcmp-*`): adsbxA9 built from `adsb-fix` alone with Quartus
23.1.1 (Nios II) lost 6 of 60,067 frames at amplitude 1000 and 6 of 60,057
at 100, with the same gaps; the Nios V image, right after it, 7 and 7 (and
the last frame of one run, in the partly filled TX buffer). So it is the
adsb design, not the port.

The decoder finds no frame at amplitude 30, and about 9,999 in 10,000 from
40 up. Its preamble detector has a fixed power threshold
(`preamble_detector.vhd`, POWER_THRESHOLD 5000), probably the limit here
(not worked out).

**The wlan revision's modem** (`lib/wiphy-loop.c`, `lib/wiphy-loop.md`;
`results/wiphy-*`, `recheck*-*`, `ab950-*`). Unicast 802.11 frames to an
address that never answers, through the AD9361's data-port loopback; for each
frame the modem must send one "no ACK" TX report with the frame's cookie.
Version 1 of the program stopped on every run with "An unexpected error
occurred" and hung until its time-out: it polled `bladerf_sync_rx()` with a
time-out (the bug above), and Nuand's driver's stream time-out
(10,000,000 ms) left the idle receive transfers pending at close. Version 2
uses a 1 s stream time-out and a longer `bladerf_sync_rx()` time-out.
Results, each run right after an FPGA load (TX gain 0 dB, 915 MHz):

| Image | status, 20 frames | control (`-n`) | loop mode | status, 64 frames, every rate, 1000 bytes |
|---|---|---|---|---|
| wlan, Nuand's code, Quartus 23.1.1 (Nios II) | 1 report (frame 0), 19 missing | pass | 0 back (expected) | 1 report (frame 0), 63 missing |
| wlan, `niosv` 5233ad4e, Quartus 25.1 (Nios V) | 1 report (frame 0), 19 missing | pass | 0 back (expected) | 1 report (frame 0), 63 missing |

Both images behave the same, so this is not the Nios V port. It is the
pattern `wiphy-loop.md` (section 5) predicted from the code for 950189d2
(2024-03-06, "fix fifo_reader packet mode dma_downcount off-by-one error"):
`fifo_reader.vhd` ends each TX packet one dword early for wlan_top's
one-dword reads, so the next packet starts with the stale dword, and every
frame after the first is read with a wrong header.
The A/B (`results/ab950-*`): Nuand's master built again with Quartus 23.1.1
exactly as the image above, but with that one line as it was before
950189d2 (`meta_future.dma_downcount <= packet_len;` instead of
`packet_len - 1`; md5 cfea4e6a…):

| Run | Nuand's code | with line 261 before 950189d2 |
|---|---|---|
| status, 20 frames, after a load | 1 report (frame 0), 19 missing | 20 of 20 |
| status, 20 frames again, no reload | 0 of 20 | 20 of 20 |
| status, 64 frames, every rate, 1000 bytes, after a load | 1 of 64 | 64 of 64 |
| control (`-n`) | pass | pass |
| loop mode | 0 back (expected) | 0 back (expected) |

So since 950189d2 the wlan revision sends only the first frame after an
FPGA load as the host asked (upstream issue [#1121]). The second stock run got
no report at all: the stale dwords left in the TX FIFO by the first run
misalign even its first frame, as `wiphy-loop.md` predicted. The demodulator
is not tested: the stock design blanks its receiver while it transmits.

### 18. The xA9 at RF against the B200mini (2026-11-19/20)

The xA9 with channel 1 cabled to the B200mini as the xA5 was in section 13
(the cabling table's row from 2026-11-19 23:25: TX1 → 20 dB → B200mini RX2,
B200mini TX/RX → 20 dB → RX1, loads on TX2 and RX2; channel 2 follows at
the end of this section), the bladeRF behind the USB hub (2-1.3)
and the B200mini on the laptop's port (2-3), `HWTEST_TX=1`,
`HWTEST_RF_CH=1`. Campaign `rfch1` (`plans/rfch1-xA9-20261119.txt`, job
script `xa9-rfch1.sh`; this section's job scripts are in
`~/workarea/builds/scripts/` on the XPS; results
`results/xA9-{A,B,C}-rfch1{,-fixes}/` and `results/rfch1-*`): phases rf,
formats and oc with only the tests that use the B200mini (the others ran in
section 16), images C, A and B with Nuand's libbladeRF (`host-build`), then
with the all-fixes build (`host-build-threads-keep250`, `HWTEST_FIXES=1`,
which adds the five two-channel fmt_txrf cases in FPGA tuning mode). 23:32
to 00:35, no stop.

| Run | A | B | C |
|---|---|---|---|
| Nuand's libbladeRF (32 tests) | 30 | 31 | 31 |
| all fixes (37 tests) | 35 | 36 | 37 |

Every failure was `oc_pulse_rx` (below). The rest passed on every image:

- rf-check (100 MHz to 5.8 GHz, both directions, both tuning modes): the
  tone found at every point; the six runs within 1.2 dB of each other at
  every point but one: B, Nuand's libbladeRF, host mode, TX at 5.8 GHz read
  −61.2 dBFS against −56.7 to −57.4, with the same total power as the other
  runs (−55.4 dBFS rms), so the tone was spread, not weaker (once in 84 TX
  readings; not repeated). The frequency error is the same on every image:
  about +2.0 ppm (TX) and −2.0 ppm (RX) against the B200mini at every
  frequency, the two boards' reference oscillators (the xA5: 0.9 ppm).
- fmt_txrf: 15 of 15 cases (20 of 20 with the fixes) on every image.
- oc_pulse_tx and oc_pulse_tx_meta (the bladeRF transmits pulses at
  122.88 Msps, SC8 and SC8 meta, and the B200mini measures them): 12 of 12
  cases in every run. On the xA5's channel 1 (section 13, `ch1b`) 6 of 12
  (A) and 7 of 12 (C) failed on short false pulses (1 to 11 per failed
  case, once 768 on A); its channel 2 had none (sections 7 and 9).

**The xA9 against the xA5 on channel 1** (the mean of the xA9's six runs
against the mean of the xA5's `ch1b` runs on A and C): within 1.5 dB at
every point in both directions, except TX at 100 MHz: 12.9 dB stronger on
the xA9. The low-band sweep of section 13 (`bin/rf-lowband.py`, host mode,
2 Msps, image A; `results/ocprobe-20261120-004638/`, job script
`xa9-ocprobe.sh`):

| MHz | 70 | 85 | 100 | 125 | 150 | 200 | 250 | 300 | 433 |
|---|---|---|---|---|---|---|---|---|---|
| xA9 TX1 tone (dBFS) | −31.9 | −31.7 | −31.7 | −32.6 | −31.8 | −32.8 | −32.0 | −31.8 | −31.9 |
| xA5 TX1 tone (dBFS) | −34.8 | −43.0 | −45.6 | −46.8 | −39.2 | −36.2 | −33.4 | −32.2 | −32.6 |
| xA9 RX1 tone (dBFS) | −11.5 | −11.5 | −11.5 | −12.5 | −11.7 | −12.8 | −12.3 | −12.5 | −12.3 |
| xA5 RX1 tone (dBFS) | −11.3 | −11.9 | −11.5 | −12.3 | −11.6 | −12.7 | −12.1 | −12.6 | −12.2 |

The xA9's TX1 is flat where the xA5's dips, and the B200mini's noise floor
while it transmits stays at −114.5 to −116.1 dBFS (the xA5's TX1: −99 at
100 MHz). At 100 MHz the xA9's image is −74 to −91 dBc in both directions
(the xA5: −52 to −57 on channel 1, −42 to −48 on channel 2). So open item
9's TX1 dip belongs to that xA5 board. RX1 at 5.8 GHz: −53.5 to −54.5 dBFS
on the xA9 (the xA5: RX1 −55.0 to −55.9, RX2 −51.7 to −52.1); the xA9's RX2
is in the channel 2 part below.

**Overclocked receive** (`oc_pulse_rx`: the B200mini sends pulses, the
bladeRF captures 0.25 s at 122.88 Msps, SC8). On the xA5 the samples were
unusable at this rate (section 7). On the xA9 they are usable: with nothing
sent they spread 1.5 LSB (I) and 3.7 to 4.2 LSB (Q) rms, none at full scale
(the larger Q spread is probably section 17's bit errors at the default
delay), the pulses are 37 to 39 dB above the gaps and their widths within
about 10 ns rms. Still 10 of the 36 captures failed (A 4, B 4, C 2), each on
one to three PRIs, on every image, in both tuning modes and with either
libbladeRF build. The worst PRI of every failed case was off by a whole
number of rxcap buffers (32,768 samples, 266.667 µs) modulo the PRI, within
10 ns: samples out of place, not corrupted.

Where they fall (`results/ocprobe-20261120-004638/`, `bin/ocrx-locate.py`:
the oc-rx cases with 0.35 s captures, 9 captures): every bad PRI lay in the
stream's buffers 16 to 33, the first 4.5 ms that rxcap kept (it dropped only
the first 16 buffers); judged from 0.1 s on, all 9 captures pass. The FPGA's
counter with no warm-up (`rxcheck -W 0`, 10 starts each) shows that window:
one of buffers 16 to 31 comes right after buffer 32 (the tool keeps 32
transfers in flight), late by 1 to 16 buffers, in 7 of 10 starts at SC16
61.44 Msps (nothing lost or repeated) and 3 of 10 at SC8 122.88 Msps
overclocked; the campaign's `rx_order` (SC16, 61.44 Msps, 10 starts per run)
found a late buffer in 3 to 8 of 10 starts in every run, with every image
and either build. That is libbladeRF's start-up reordering (section 7's
`rx_order`; upstream [#1139]), which rxcheck skips with its 0.25 s warm-up.
rxcap now warms up the same way (2d8f306a). The six runs again with it
(`plans/ocrx2-xA9-20261120.txt`, job script `xa9-ocrx2.sh`, results
`results/xA9-{A,B,C}-ocrx2{,-fixes}/`; the patched hwtest-stream built on
its own and given to pulse-check.py with `HWTEST_STREAM`): all 36
captures pass, on every image, in both tuning modes, with either build (each
test 1 to 2 s longer: the warm-ups). The samples as before: 1.5 (I) and 3.6
to 4.0 (Q) LSB rms with nothing sent, the pulses 38 to 39 dB above the gaps,
their widths within 10 to 11 ns rms, and the mean pulse interval 1.9 to
2.1 ppm from the B200mini's: the two boards' clock difference that rf-check
measures too. So with the default interface delay the xA9 receives at
122.88 Msps well enough for pulse timing on all three images; the bit
errors of section 17 remain.

**Channel 2** (2026-11-20 morning). The two B200mini cables moved to channel
2 (the cabling table's last row: TX2 → 20 dB → B200mini RX2, B200mini TX/RX
→ 20 dB → RX2, loads on TX1 and RX1), USB as before, `HWTEST_RF_CH=2`. The
same six runs (`plans/rfch2-xA9-20261120.txt`, job script `xa9-rfch2.sh`;
results `results/xA9-{A,B,C}-rfch2{,-fixes}/` and `results/rfch2-*`), with
rxcap's warm-up (the hwtest-stream of the rerun above, through
`HWTEST_STREAM`). 07:11 to 08:14, no stop.

| Run | A | B | C |
|---|---|---|---|
| Nuand's libbladeRF (32 tests) | 32 | 32 | 31 |
| all fixes (37 tests) | 37 | 37 | 37 |

- rf-check: the tone found at every point; the six runs within 1.3 dB of
  each other at every point, none more than 1 dB from the six runs' median;
  the frequency error +1.8 to +2.1 ppm (TX) and −1.8 to −2.1 ppm (RX), as on
  channel 1.
- Channel 2 against channel 1 (the means of the six runs): within 1.2 dB at
  every point below 5.8 GHz. At 5.8 GHz RX2 reads 2.0 to 2.3 dB stronger
  than RX1 (−51.8 to −52.4 dBFS) and TX2 1.2 to 1.7 dB weaker than TX1.
  Against the xA5's channel 2 (section 4's `tx` runs, A and C, as in
  section 13): within 1.5 dB at every point, and the xA5's RX2 at 5.8 GHz
  read −51.7 to −52.1. So RX1 is weaker than RX2 at 5.8 GHz on both boards
  (open item 9).
- The low-band sweep on channel 2 (`results/rfch2-20261120-070831/`):

| MHz | 70 | 85 | 100 | 125 | 150 | 200 | 250 | 300 | 433 |
|---|---|---|---|---|---|---|---|---|---|
| xA9 TX2 tone (dBFS) | −34.6 | −34.7 | −34.3 | −33.3 | −33.9 | −33.2 | −32.8 | −32.6 | −33.1 |
| xA9 RX2 tone (dBFS) | −12.2 | −12.4 | −12.2 | −11.5 | −12.3 | −12.1 | −12.2 | −12.3 | −13.0 |

  TX2 has no dip: it is 0.4 to 3.0 dB below TX1, most at 70 to 100 MHz.
  RX2 is within 1.0 dB of RX1. While TX2 transmits, the B200mini's noise
  floor is −111.0 to −116.0 dBFS (TX1: −114.5 to −116.1; the xA5's TX1: −99
  at 100 MHz), and −108.4 to −114.3 at rf-check's 100 MHz point (TX1:
  −115.3 to −115.5). At 100 MHz the image is −70 to −80 dBc (TX) and −75 to
  −86 dBc (RX) in rf-check, against −48 and −42 dBc on the xA5's channel 2.
- fmt_txrf: 15 of 15 cases (20 of 20 with the fixes) on every image.
- oc_pulse_rx, with the warm-up: 36 of 36 captures pass. The samples are
  as on RX1: 1.7 to 1.9 LSB (I) and 3.4 to 3.7 LSB (Q) rms with nothing
  sent, and the pulses 37 to 39 dB above the gaps.
- oc_pulse_tx and oc_pulse_tx_meta: 71 of 72 cases pass. The one failure
  was on C (Nuand's libbladeRF, host tuning mode, the 1 µs pulses in SC8):
  15 extra pulses among 25,000, the shortest 41 ns long, with two rises
  89 ns apart. That is the kind of false pulse that the xA5's channel 1 had
  in about half of its cases, on A and C alike (37 to 52 ns long, rises 75
  to 105 ns apart; section 13).

The overclocked transmit cases again (job script `xa9-octx2.sh`, results
`results/octx2-20261120-081653/`: oc_pulse_tx and oc_pulse_tx_meta, host
tuning mode, Nuand's libbladeRF, A and C alternately, 8 rounds, the
B200mini's captures kept for a failed test): A 48 of 48 cases, C 47 of 48.
In the one failure the envelope dropped to 1 % of the pulse level for
about 0.1 µs within the first microsecond of the B200mini's capture. That
split the capture's first pulse, which pulse-check drops, so its second
part counted as the first whole pulse (0.60 µs wide, 9.60 µs to the next).
That gap is the B200mini's own start. Its receive filters are still
filling when a stream starts: within the first 20 samples the level dips to
10 to 30 %, at every rate the harness uses (0.1 to 30.72 Msps, 20 starts
each, receive only, job scripts `b200-start.py` and `b200-start2.py`), and
at 30.72 Msps (no decimation) the first 10 samples are zero. At 20 Msps the
dip is at samples 12 to 16, where this capture's gap was. Every B200mini
capture of the harness goes through rf-check's `b200_capture`, which now
drops the first 32 samples (78bd1f5a). With that, no dip is left at any of
those rates, the failed capture passes when judged again, and oc-tx (3 of 3
cases) and rf-check (every point) pass on A. Without that failure, C failed
1 of 96 overclocked transmit cases over both channels (the false pulses
above), A 0 of 96 and B 0 of 48: not a significant difference. Open item
15.

### 19. Both channels at once through a splitter (2026-11-20)

Open item 8: the twoch phase (`bin/twoch-check.py`), its first runs on
hardware. The xA9 with USB as in section 18; a TOJOIN PS-2SM-2080 splitter
with a 20 dB attenuator on each output (README.md, "Two-channel setups";
the cabling table's rows from 2026-11-20 ~21:45), `HWTEST_TX=1`. Job script
`xa9-twoch.sh`: first a pre-check (twoch-check.py at all five points on the
image already loaded, host tuning mode, Nuand's libbladeRF, captures kept),
then the campaign, phase twoch only. Summaries by `twoch-summary.py`.

**Receive** (`HWTEST_2CH=rx`, `plans/twochrx-xA9-20261120-2157.txt`;
results `results/twochrx-*` and `results/xA9-{A,B,C}-twochrx{,-fixes}/`).
The first pre-check (21:50) passed at 433 MHz to 3.5 GHz. At 5.8 GHz it
failed on one check: the fractional-lag fit found 23 coherent bins, and it
needs 64. Over the chirp's band the two channels' coherence had a median of
0.68: the chirp reaches the bladeRF about 30 dB weaker there than at
433 MHz. The cross-correlation's own estimate of the lag was +0.006
samples, and the other checks passed. 6a3c3f43 raises the B200mini's TX
gain at 5.8 GHz from 50 to 60 dB (its cap is 65) and changes no analysis
and no limit. Then (21:57 to 22:05, no stop) the pre-check and all six runs
passed: images C, A and B, with Nuand's libbladeRF in host tuning mode and
with the all-fixes build in both modes (50 records of one point each, 50
pass, no retries). The ranges over all of them:

| Point | RX2 − RX1 level | Phase spread (limit 5°) | RX2 / RX1 move for the 10 dB step | Alone against both | RX2 − RX1 phase (both, 4 retunes) | Lag: whole, fit, cross-correlation |
|---|---|---|---|---|---|---|
| 433 MHz, 4 Msps | +0.2 dB | ≤ 0.10° | −10.1 / ≤ +0.1 dB | within 0.1 dB | −3.2 to −3.9° | 0, ≤ 0.001 samples |
| 915 MHz, 10 Msps | 0.0 dB | ≤ 0.17° | −10.1 / ≤ +0.1 dB | within 0.1 dB | −8.2 to −9.0° | 0, ≤ 0.0012 |
| 1575.42 MHz, 5 Msps | −0.1 to −0.2 dB | ≤ 0.12° | −9.1 to −9.2 / ≤ +0.1 dB | within 0.1 dB | −22.7 to −23.5° | 0, ≤ 0.0013 |
| 3.5 GHz, 20 Msps | +0.7 dB | ≤ 0.61° | −9.1 / 0.0 dB | within 0.1 dB | −20.1 to −20.6° | 0, ≤ 0.0041 |
| 5.8 GHz, 10 Msps | +1.4 dB | ≤ 0.63° | −9.9 / ≤ −0.1 dB | within 0.1 dB | −36.7 to −37.8° | 0, ≤ 0.0044 |

- The three images and the two tuning modes gave the same numbers: levels
  within 0.3 dB, phases within 1.1°.
- After every retune the phase difference came back within 0.8°. The two
  channels share the AD9361's RX LO, so the difference is that of the two
  analog paths (splitter output, attenuator, cable and the board's own),
  and a retune does not change it.
- The tone's frequency on RX1 and on RX2 agreed within 0.014 Hz (one bin:
  15 to 19 Hz).

**The two paths exchanged** (`xa9-twoch.sh rx swap`: the two cables at RX1
and RX2 exchanged, so each path, from its splitter output to its port, now
goes to the other channel; results `results/twochrx-swap-20261120-222517/`
and `results/xA9-A-twochrx-swap-fixes/`). The pre-check and one run, image
A with the all-fixes build in both modes, 22:25 to 22:28: 15 records, 15
pass, and the differences hardly moved. The mean of the records before plus
the mean after is twice the board's own difference; the mean before minus
the mean after is twice the two paths' difference:

| Point | RX2 − RX1 level, before / after | Board | Paths | RX2 − RX1 phase, before / after | Board | Paths |
|---|---|---|---|---|---|---|
| 433 MHz | +0.23 / +0.21 dB | +0.22 dB | +0.01 dB | −3.6 / −3.5° | −3.5° | −0.1° |
| 915 MHz | +0.01 / −0.10 dB | −0.04 dB | +0.05 dB | −8.7 / −8.0° | −8.4° | −0.4° |
| 1575.42 MHz | −0.15 / −0.01 dB | −0.08 dB | −0.07 dB | −23.2 / −23.3° | −23.2° | 0.0° |
| 3.5 GHz | +0.71 / +0.84 dB | +0.78 dB | −0.06 dB | −20.4 / −19.4° | −19.9° | −0.5° |
| 5.8 GHz | +1.39 / +1.53 dB | +1.46 dB | −0.07 dB | −37.3 / −40.0° | −38.7° | +1.4° |

The two paths agree within 0.07 dB and 1.4°: the differences are the
board's. Its RX2 is stronger than RX1 on the 3 to 6 GHz input (port A:
0.8 dB at 3.5 GHz, 1.5 dB at 5.8 GHz) and within 0.2 dB on the input
below 3 GHz (port B). That is the RX1-weaker-at-5.8-GHz of open item 9,
now measured on one signal at the same time; section 18's 2.0 to 2.3 dB
came from two campaigns 7 hours apart, with the cable moved in between.

**Transmit** (`HWTEST_2CH=tx`: the same splitter and attenuators turned
round, the splitter's common port on the B200mini's RX2, the cabling
table's row from ~22:30; `plans/twochtx-xA9-20261120-2238.txt`, results
`results/twochtx-20261120-223438/` and `results/xA9-{A,B,C}-twochtx{,-fixes}/`).
22:34 to 23:25, no stop. The pre-check and all six runs passed: images C, A
and B, in both tuning modes with either build (two-channel transmit in FPGA
tuning mode works with Nuand's libbladeRF too). 65 records of one point
each, 65 pass, no retries; each run 8 minutes. The ranges over all of them:

| Point | TX2 − TX1 level | TX2 / TX1 move for TX2's 10 dB step | Alone against both | Other channel's tone, alone | Image | LO leakage | Tone above floor |
|---|---|---|---|---|---|---|---|
| 433 MHz | −0.4 dB | −10.1 / 0.0 dB | within 0.1 dB | −67 to −77 dBc | −47 to −48 dBc | −38 to −40 dBc | ≥ 81 dB |
| 915 MHz | −0.1 dB | −10.1 / 0.0 dB | within 0.1 dB | −67 to −74 dBc | −47 to −49 dBc | −41 to −42 dBc | ≥ 80 dB |
| 1575.42 MHz | −0.4 dB | −10.1 / 0.0 dB | within 0.1 dB | −62 to −70 dBc | −37 to −40 dBc | −39 to −40 dBc | ≥ 75 dB |
| 3.5 GHz | −0.4 to −0.5 dB | −10.1 / 0.0 dB | within 0.1 dB | −55 to −62 dBc | −46 to −53 dBc | −47 to −50 dBc | ≥ 67 dB |
| 5.8 GHz | −1.2 to −1.4 dB | −9.9 to −10.1 / ≤ +0.1 dB | within 0.1 dB | −39 to −49 dBc | −39 to −47 dBc | −26 to −27 dBc | ≥ 52 dB |

- The three images, the two tuning modes and the two builds gave the same
  levels within 0.1 dB.
- The disabled channel sends nothing that the B200mini can see. In the
  pre-check's saved captures, no line at the other channel's tone
  frequency stands above the noise around it (±400 bins), at any point. So
  that column is a noise reading, and its margin to the −30 dBc limit
  (9 dB at 5.8 GHz) comes from the signal-to-noise ratio, not from leakage.
- The two tones' frequency errors agree within 1.4 Hz (one bin: 30 to
  38 Hz). Both are +2.0 ppm against the B200mini at every point, as in
  section 18.
- TX2 is 1.3 dB weaker than TX1 at 5.8 GHz (section 18, one channel at a
  time: 1.2 to 1.7 dB). The splitter, attenuators and cables are passive,
  so the two paths agree within 0.07 dB in this direction too (the swap
  above), and that difference is the board's. At 5.8 GHz this board's
  channel 2 is the weaker transmitter and the stronger receiver.
- These runs do not measure the phase or the timing between TX1 and TX2:
  the two tones have different frequencies. The sync runs below do.

**Transmit phase and timing** (2026-11-21: twoch-check.py's sync runs,
README.md "tx, sync runs", hwtest 2cd48cc3; `HWTEST_2CH_TX=sync`, the same
cabling; `plans/twochtx-sync-xA9-20261121-0648.txt`, results
`results/twochtx-sync-20261121-064502/` and
`results/xA9-{A,B,C}-twochtx-sync{,-fixes}/`). Both channels send the same
29-line multitone, TX2's turned by 0°, 90°, 180° and 270° in four blocks;
from each line's power in the four blocks follows the cross-spectrum of TX1
and TX2, with no lock between the two radios. 06:45 to 07:24, no stop. The
pre-check and all six runs passed: images C, A and B, in both tuning modes
with either build. 65 records, each with three sync runs (the second and
third after tuning 20 MHz or an octave away and back), 65 pass, no
retries; each run 6 minutes. The ranges over all 195 captures:

| Point | TX2 − TX1 phase at the carrier | TX2's lag: whole / fraction (samples) | Phase spread over a capture's cycles | Lag spread | Phase over the three runs | Line SNR (median) |
|---|---|---|---|---|---|---|
| 433 MHz | +0.7° | 0 / ≤ 0.001 | ≤ 0.1° | ≤ 0.001 | within 0.1° | ≥ 60 dB |
| 915 MHz | +1.6° | 0 / ≤ 0.001 | ≤ 0.2° | ≤ 0.002 | within 0.1° | ≥ 56 dB |
| 1575.42 MHz | +2.2° | 0 / ≤ 0.001 | ≤ 0.2° | ≤ 0.003 | within 0.1° | ≥ 53 dB |
| 3.5 GHz | −14.2° to −14.5° | 0 / ≤ 0.003 | ≤ 0.8° | ≤ 0.013 | within 0.3° | ≥ 40 dB |
| 5.8 GHz | −6.8° to −8.1° | 0 / ≤ 0.011 | ≤ 3.1° | ≤ 0.045 | within 0.7° | ≥ 28 dB |

- TX2 was at lag 0 in every capture. The markers' coarser lag agreed
  with it: within 2 samples at 3.5 GHz and below, 8.6 at most at 5.8 GHz
  (the check allows 15).
  The three images, both tuning modes (in FPGA tuning mode the Nios does
  the retunes) and both builds gave the same phases.
- TX2's phase relative to TX1 stays where it was after each retune (class
  "stable" at every point), as RX2's relative to RX1 did above.
- The phases include the two paths' own difference, which the swap above
  put at 1.4° or less.
- Both spreads are noise: they grow as the lines' signal-to-noise ratio
  falls. At 5.8 GHz the largest phase spread, 3.1°, is 1.9° below the 5°
  limit.
- The B200mini is +1.9 to +2.2 ppm from the bladeRF at every point, as in
  section 18.

**The xA5** (2026-11-21: open item 8 on a second board). The same
splitter, attenuators, cables and USB ports, the xA5 in the xA9's place
(the cabling table's rows from 2026-11-21). Job script `xa5-twoch.sh`: the
steps of `xa9-twoch.sh` with `HWTEST_FPGA=A5`, the longest run first. The
two cable-and-pad pairs may be on the other channels than on the xA9 (they
were moved between the boards); the swap below shows that the two paths
agree within 0.09 dB and 0.9°, so that changes nothing.

*Transmit* (hwtest 88963f4d, tone runs and sync runs;
`plans/xa5twochtx-xA5-20261121-0850.txt`, results
`results/xa5twochtx-20261121-084331/` and
`results/xA5-{A,B,C}-xa5twochtx{,-fixes}/`). 08:43 to 10:11, no stop; each
run 13 minutes. 65 records, 63 pass, no retries; the two failures are
below. The tone runs' ranges over the 63 that passed, and the sync runs'
over all 195 captures (the sync runs of the two failed records passed):

| Point | TX2 − TX1 level | TX2 / TX1 move for TX2's 10 dB step | Alone against both | Other channel's tone, alone | Image | LO leakage | Tone above floor |
|---|---|---|---|---|---|---|---|
| 433 MHz | −0.1 dB | −10.2 / 0.0 dB | within 0.1 dB | −69 to −74 dBc | −47 to −65 dBc | −33 to −34 dBc | ≥ 81 dB |
| 915 MHz | +0.2 dB | −10.2 / 0.0 dB | within 0.1 dB | −67 to −76 dBc | −47 to −53 dBc | −34 dBc | ≥ 80 dB |
| 1575.42 MHz | −0.1 dB | −10.2 / 0.0 dB | within 0.1 dB | −61 to −70 dBc | −38 to −39 dBc | −34 dBc | ≥ 75 dB |
| 3.5 GHz | −0.4 dB | −10.1 / 0.0 dB | within 0.1 dB | −55 to −61 dBc | −46 to −53 dBc | −36 to −38 dBc | ≥ 67 dB |
| 5.8 GHz | −0.4 to −0.5 dB | −9.7 to −9.9 / ≤ +0.1 dB | within 0.1 dB | −41 to −48 dBc | −39 to −46 dBc | −32 to −34 dBc | ≥ 53 dB |

| Point | TX2 − TX1 phase at the carrier | TX2's lag: whole / fraction (samples) | Phase spread over a capture's cycles | Lag spread | Phase over the three runs | Line SNR (median) |
|---|---|---|---|---|---|---|
| 433 MHz | +0.8° | 0 / ≤ 0.002 | ≤ 0.1° | ≤ 0.001 | within 0.1° | ≥ 59 dB |
| 915 MHz | +0.2° to +0.3° | 0 / ≤ 0.002 | ≤ 0.2° | ≤ 0.002 | within 0.1° | ≥ 55 dB |
| 1575.42 MHz | −0.4° to −0.6° | 0 / ≤ 0.003 | ≤ 0.2° | ≤ 0.003 | within 0.1° | ≥ 53 dB |
| 3.5 GHz | −13.6° to −13.9° | 0 / ≤ 0.004 | ≤ 0.8° | ≤ 0.013 | within 0.2° | ≥ 40 dB |
| 5.8 GHz | −10.0° to −11.1° | 0 / ≤ 0.010 | ≤ 2.8° | ≤ 0.042 | within 0.7° | ≥ 27 dB |

- As on the xA9, apart from the two failures, the three images, the two
  tuning modes and the two builds gave the same numbers; TX2 was at lag 0
  in every capture, and TX2's phase stays where it was after each retune
  ("stable" at every point).
- Against the xA9: TX2 − TX1 levels within 0.9 dB (the xA9's TX2 is
  1.3 dB weaker at 5.8 GHz, this board's 0.4 to 0.5 dB), phases within
  3.1° (center to center). Both include each board's own difference and
  the paths' (0.9° or less, the swap below).
- The B200mini is +0.9 to +1.3 ppm from the xA5 (from the xA9: +1.9 to
  +2.2).

The two failures: FPGA tuning mode at 5.8 GHz with Nuand's libbladeRF,
images C and A. In one of the point's seven bladeRF-cli runs both tones
read about 5 dB low (C: the run with both channels at the same gain, so
the later runs read 5 dB higher against it; A: the run with TX2 10 dB
lower, so TX1 moved −4.9 dB and TX2 −14.6 dB); the point's other runs were
normal. twoch-check.py did not keep bladeRF-cli's output, so 182b1dd4 adds
it to the records (`cli`), with an optional trace of every Nios packet of
each bladeRF-cli run, and `--freqs` and `--repeat` (README.md). The rerun
(`xa5-twochdiag.sh`, results `results/xa5twochdiag-20261121-105011/`,
10:50 to 11:40): images C and A, FPGA tuning mode, Nuand's libbladeRF,
3.5 and 5.8 GHz in turn, ten times each, every Nios packet traced. 40
records, 40 pass, but image A had four low runs at 5.8 GHz (2.6 to
5.2 dB, in its first and seventh rounds), C none:

- No low run followed a Nios time-out or a reply to another request. The
  host's requests, their values and the Nios's replies were the same as
  in the normal runs of the same point.
- In the low runs the tone stood 0.3 to 0.5 dB less above the B200mini's
  floor than the level drop alone explains: the floor read higher.
- In FPGA tuning mode the first run at 3.5 GHz after 5.8 GHz (and the
  first after the image load) sets a new sample rate and bandwidth
  (20 Msps and 20 MHz after 10), and in every such run one of the two
  commands (in two runs both) kept the Nios busy for 360 to 385 ms:
  libbladeRF's status read timed out after 250 ms (the condition of
  upstream issue [#1108]), 22 times in 20 runs. No reply was taken for another
  request, and those runs' levels were normal.

Open item 16 has older records that show the same.

*Receive* (hwtest 182b1dd4, whose change to twoch-check.py does not touch
rx; `plans/xa5twochrx-xA5-20261121-1247.txt`, results
`results/xa5twochrx-20261121-124647/` and
`results/xA5-{A,B,C}-xa5twochrx{,-fixes}/`). 12:46 to 12:56, no stop: the
pre-check and all six runs passed, 50 records, 50 pass, no retries:

| Point | RX2 − RX1 level | Phase spread (limit 5°) | RX2 / RX1 move for the 10 dB step | Alone against both | RX2 − RX1 phase (both, 4 retunes) | Lag: whole, fit, cross-correlation |
|---|---|---|---|---|---|---|
| 433 MHz, 4 Msps | 0.0 dB | ≤ 0.19° | −10.2 / 0.0 dB | within 0.1 dB | −3.5 to −3.8° | 0, ≤ 0.0093 samples |
| 915 MHz, 10 Msps | −0.2 to −0.3 dB | ≤ 0.15° | −10.2 / 0.0 dB | within 0.1 dB | −8.7 to −9.0° | 0, ≤ 0.0107 |
| 1575.42 MHz, 5 Msps | −0.3 to −0.4 dB | ≤ 0.15° | −9.2 to −9.3 / 0.0 dB | within 0.1 dB | −23.2 to −23.5° | 0, ≤ 0.0097 |
| 3.5 GHz, 20 Msps | +0.9 dB | ≤ 0.59° | −9.1 / 0.0 dB | within 0.1 dB | −19.1 to −19.5° | 0, ≤ 0.0067 |
| 5.8 GHz, 10 Msps | +1.9 to +2.0 dB | ≤ 0.58° | −9.9 to −10.0 / 0.0 dB | within 0.1 dB | −33.8 to −34.7° | 0, ≤ 0.0107 |

*The two paths exchanged* (`xa5-twoch.sh rx swap`; results
`results/xa5twochrx-swap-20261121-132253/` and
`results/xA5-A-xa5twochrx-swap-fixes/`): the pre-check and one run, image
A with the all-fixes build in both modes, 13:22 to 13:25: 15 records, 15
pass. Board and paths as for the xA9 above:

| Point | RX2 − RX1 level, before / after | Board | Paths | RX2 − RX1 phase, before / after | Board | Paths |
|---|---|---|---|---|---|---|
| 433 MHz | −0.03 / −0.11 dB | −0.07 dB | +0.04 dB | −3.7 / −3.6° | −3.7° | −0.1° |
| 915 MHz | −0.25 / −0.43 dB | −0.34 dB | +0.09 dB | −8.9 / −8.0° | −8.5° | −0.4° |
| 1575.42 MHz | −0.35 / −0.29 dB | −0.32 dB | −0.03 dB | −23.3 / −22.6° | −22.9° | −0.3° |
| 3.5 GHz | +0.88 / +0.99 dB | +0.93 dB | −0.06 dB | −19.2 / −17.4° | −18.3° | −0.9° |
| 5.8 GHz | +1.95 / +1.79 dB | +1.87 dB | +0.08 dB | −34.1 / −32.7° | −33.4° | −0.7° |

- The two paths agree within 0.09 dB and 0.9°: the differences are the
  board's. Like the xA9's, this board's RX2 is the stronger receiver on
  the 3 to 6 GHz input: 0.9 dB at 3.5 GHz, 1.9 dB at 5.8 GHz (the xA9: 0.8
  and 1.5 dB).
- Against the xA9: levels within 0.6 dB, phases within 3.0° (center to
  center).
- RX2's fractional lag is −0.009 to −0.011 sample (−0.005 at 3.5 GHz) in
  every record, before and after the exchange (the paths' part: 0.0002
  sample at most); on the xA9 0.004 at most. A property of this board,
  more than 20 times below the quarter-sample limit.

### 20. A failed flash page read; the bandwidth sweep in host tuning mode (2026-11-21)

Two checks that the issue texts needed (the draft of [#1120] came from
reading the code only; the draft of [#1108] named no workaround). The xA5
with the official image (A) loaded first, receive-only (the no-TX guard
preloaded), job `xa5v4v12`, results `results/xa5v4v12-20261121-153432/`.

**A failed flash page read ([#1120], [#1135]).** `lib/flash-read-fail.c` makes
the first read of page 100 fail in one of three ways: the firmware's status
reads back as -1 (status), the FLASH_READ request fails (cmd), or the page
buffer read fails (buffer). `bladeRF-cli -e "flash_backup <file> fw"` reads
pages 0 to 767 (the FX3 firmware region), with the master and [#1135] builds of
section 17 ("Each fix alone"). The shim refuses every request that could write
the flash; it refused none.

| Build | Failure | bladeRF-cli prints | Backup file |
|---|---|---|---|
| master | none | - | saved |
| master | status | "Firmware page read (op=100) failed at page 100: -1" | saved; pages 100-767 all zeros |
| master | cmd | nothing | saved; pages 100-767 all zeros |
| master | buffer | nothing | saved; pages 100-767 all zeros |
| [#1135] | none | - | saved; the same data as master's |
| [#1135] | status | the same error, then "flash_backup: Failed to read flash region", "An unexpected error occurred" | none |
| [#1135] | cmd | "flash_backup: Failed to read flash region", "File or device I/O failure" | none |
| [#1135] | buffer | the same as cmd | none |

Each failing read stopped after page 100 (no "Done reading 768 pages").
bladeRF-cli's exit status was 0 in every case, also when the command failed
(as for every failed `-e` command).

**The full bandwidth sweep in host tuning mode ([#1108]).** `test_ctrl -T
host -t bandwidth` (the full sweep: 3,448 bandwidth changes) three times with
Nuand's libbladeRF (host-build), then `-T fpga` once as the control, each
with every Nios packet traced (`lib/nios-trace.c`, test_ctrl only) and the
trace analyzed with `bin/nios-trace-analyze.py`:

| Run | Result | Time | Nios requests | IN time-outs | Replies to another request | Longest Nios reply |
|---|---|---|---|---|---|---|
| host 1 | pass | 472 s | 1,153,103 | 0 | 0 | 0.5 ms |
| host 2 | pass | 483 s | 1,167,046 | 0 | 0 | 0.5 ms |
| host 3 | pass | 496 s | 1,183,654 | 0 | 0 | 0.5 ms |
| FPGA (control) | pass | 392 s | 18,539 | 1,261 | 0 | 250.3 ms |

- In host tuning mode libbladeRF runs the AD9361 driver on the host: about
  340 Nios requests per bandwidth change, each a short register access (the
  only RFIC packets were 4 status reads, when test_ctrl opened the device).
  No request keeps the Nios busy, so no status poll times out, and [#1108]
  cannot happen. Host tuning mode is libbladeRF's default; FPGA tuning mode
  is selected with `BLADERF_DEFAULT_TUNING_MODE=fpga` or
  `bladerf_set_tuning_mode()`.
- The control has [#1108]'s time-outs as before: 1,261, and 1,279 bandwidth
  changes took 250 ms or more (the traces of 2026-11-13 under 3a768f3d
  below: 1,300 to 1,330 time-outs per run with A and B). This time the Nios
  never answered both polls, so no reply went to the wrong request and the
  run passed: a failure needs the Nios to finish in the ~0.2 ms between a
  time-out and the next poll (README.md).

### 21. 1R1T mode: one channel at 122.88 Msps on the xA5 and the xA9 (2026-11-21/22)

The OVERSAMPLE feature keeps both AD9361 receive channels on (register
0x003 = 0xD4), so the AD9361-FPGA interface runs in 2R2T mode, with DATA_CLK
at 4 x the sample rate: 491.52 MHz at 122.88 Msps. With only RX1 and TX1 on
(0x003 = 0x54, 0x002 = 0x40) the AD9361 uses 1R1T timing, and DATA_CLK is
2 x the sample rate: 245.76 MHz, the clock of 2R2T at 61.44 Msps (section
17 measured the halving on the xA9; the xA5 halves too). Nuand's interface
module decodes only 2R2T frames, so 1R1T needs an FPGA change.

**The starting point: 2R2T at 122.88 Msps on the xA5** (job `xa5delay`,
results `results/xa5delay-20261121-225647/`). The AD9361's PRBS (BIST, SC8,
OVERSAMPLE) for each RX delay setting (register 0x006, 1 s each), image A
('.' clean, 'x' bad samples, '-' no lock):

| Rate | Channel | DATA_CLK delay 0..15 | data delay 1..15 |
|---|---|---|---|
| 76.8 | RX1 | `-x....--x....--x` | `-....x--....x--` |
| 76.8 | RX2 | `-x...x--x...x--x` | `-....x--x...x--` |
| 122.88 | RX1 | `x--.x--.x--x.--x` | `.x--.x--x.---xx` |
| 122.88 | RX2 | `x--.x--.x--x.--x` | `.x--xx--x.---.x` |

At 122.88 Msps four settings were clean on both channels: 0x30 and 0x70
(DATA_CLK delay 3 and 7), 0x01 and 0x0a (data delay 1 and 10), each also
for 30 s per channel (3.69e9 samples, 0 bad). Nuand's default (0x50, DATA_CLK
delay 5) does not lock. Image C gave the same windows. Each clean window
is one step wide.

**The change** (branch `1r1t` on master 35174c30; on the fork since
2026-11-22):

- `axi_ad9361_dev_if_alt.v`: the 1R1T receive frames (the frame is high for
  the two msb words of each sample, so each l_clk cycle holds 4'b0011,
  4'b0110, 4'b1100 or 4'b1001 and starts one sample) and transmit frames
  (4'b0011), selected by the AXI AD9361 core's R1 mode bits. The 2R2T cases
  do not change. A cycle model (now `deframer_1r1t_model.py` on branch `1r1t`) decodes
  every sample at all 8 2R2T and 4 1R1T alignments.
- Qsys: the core exports its R1 bits; the time tamers count every clock in
  1R1T mode.
- `bladerf-hosted.vhd`: in 1R1T mode the core's channel 0 valid passes
  without the rising-edge filter, channel 1 is dropped, and the RX counter
  runs at one sample per clock.
- `fifo_reader.vhd` and `fifo_writer.vhd`: one sample per clock (below).
- libbladeRF: `bladerf_set_rfic_1r1t()` and `bladerf_get_rfic_1r1t()` in
  bladeRF2.h turn RX2 and TX2 off in the AD9361, and set the core's R1 bits
  and a DAC rate of one sample per clock. The mode is set again after each
  sample rate change, and it is off after every open. While it is on,
  enabling RX2 or TX2 fails. (Replaced by the automatic selection below.)
- hwtest-stream: `-1` (1R1T mode), `-w` (analog bandwidth), `-X` (print
  the first received loopback samples).
- `r1-state.c`, `r1-meta.c` and the two cycle models, used below, are on
  branch `1r1t` now: `host/libraries/libbladeRF_test/test_1r1t_state` and
  `test_1r1t_meta`, and `hdl/fpga/platforms/bladerf-micro/vhdl/tb/models/`.

Images (hostedxA5, Quartus Prime Lite 23.1.1, Nios II, worst setup slack in
brackets; Nuand's design in the same build: -0.154 ns): R = 10494cab +
7d7480fa (-0.165 ns), R2 = R + aeb7039e (-0.066 ns), R3 = R2 + d66391ae
(-0.188 ns). Receive only and host tuning mode unless noted; TX runs with
`HWTEST_TX=1` (TX cap) into 50 ohm loads. Jobs `xa5r1`, `xa5r1b`, `r1chain`,
`xa5r1start`, `xa5r1soak`.

| Check | Result |
|---|---|
| 2R2T on R, R2, R3: BIST SC16 at 61.44 on RX1 and RX2; SC8 at 61.44 (R); counter at 122.88 (R) | pass |
| 1R1T counter at 30.72, 61.44, 122.88 (SC8) and 61.44 (SC16), 10 s each | pass: every sample, 122.879 Msps |
| 1R1T BIST SC16 at 30.72 and 61.44 (all 12 bits) | 0 bad |
| 1R1T BIST SC8 at 30.72, 61.44, 76.8, 92.16, 100, 112 and 122.88, 10 s each, against a golden period learnt in 1R1T | 0 bad |
| 1R1T 0x006 map at 122.88, RX1 | DATA_CLK delay 1-6 and 10-14, data delay 3-8 and 12-15 clean (`x......--x.....x`, `--......---....`) |
| 1R1T BIST SC8 at 122.88, long | R: 300 s, 3.69e10 samples, 0 bad; R2: see below; R3: 1800 s, 2.21e11 samples, 0 bad |
| 1R1T counter at 122.88 for 600 s | R2: pass; R3: pass (7.37e10 samples, no jump) |
| 1R1T in FPGA tuning mode (R2): BIST, counter, scheduled bursts at 122.88 | pass |
| 1R1T loopback in the AD9361 (`-A`, R2, R3) | bit-exact at 30.72 (SC16), 61.44 and 76.8 (SC8); 2R2T fails at 76.8 on this board |
| `bladerf_set_rfic_1r1t()` state (`r1-state.c`), host and FPGA tuning mode | as expected; RX2 refused in 1R1T mode (TX2: the no-TX guard refuses it first) |
| Image A with this libbladeRF, 2R2T BIST SC16 and SC8 | pass (1R1T on image A gives no usable samples, as expected) |
| FPGA digital loopback x2 SC16 at 30.72 | fails on R2 and on image A alike (x1 passes on both) |

The R2 1800 s soak lost samples once: at 121 s, for 0.4 ms (145 phase
losses, 37,120 bad samples, bit errors spread evenly over all 16 bits), in
the second the XPS's package cache refresh started (`mint-refresh-cache`,
every 2 h at :11:52 and :12:52); the other 1,680 s (2.06e11 samples) were
clean. A host stall, not the interface (open item 17).

Two problems came up in the first images and were fixed:

- **TX samples in swapped pairs (R).** The loopback in the AD9361 never
  locked in 1R1T. Its first received samples (`-X`) were pairs swapped:
  ..., 0x79, 0x78, 0x7b, 0x7a, ... With one channel a TX FIFO word holds two
  samples, and fifo_reader's registered read strobe advanced the FIFO one
  clock after the request that took the word's last sample; at one request
  per clock the next request read the old word again. aeb7039e takes the
  strobe from the next state in 1R1T mode; 2R2T keeps the registered strobe.
- **RX meta timestamps (R, R2).** Back-to-back bursts read "now" (rxsched
  -N, sc8meta, 122.88) failed in 1R1T in 3 of 3 runs, always the first
  burst: an "overrun" after 4,088 samples, the first message. `r1-meta.c`
  (the first 12 messages, the FPGA counter as the source) showed why: in
  1R1T the second SC8 message's timestamp was 4,089 samples after the first
  (then 4,088 each), and each SC16 message's at 61.44 Msps 2,046 after the
  previous (2,044 expected); the samples were continuous. A cycle model of
  fifo_writer (now `fifo_writer_1r1t_model.py` on branch `1r1t`) gives the same: at one sample per clock its meta FSM's steps
  through IDLE and META_WRITE let samples into the next buffer before the
  next meta. d66391ae counts each word when it is decided and writes the
  next meta in that cycle; on R3 the messages are 4,088 (SC8) and 2,044
  (SC16) samples apart in 1R1T, as in 2R2T.

With exact timestamps, back-to-back bursts on R3 still overran at the
start of some streams: bursts 1 to 3, then none. That is open item 6
([#1139]), and it is the same in both modes (`xa5r1start`, 3 s runs at
122.88 sc8meta): 1R1T 4 of 8 runs clean, 2R2T 4 of 8, image A 3 of 6;
scheduled bursts 3 of 4 in each mode. Every burst after the start was
complete.

**RF** (B200mini tone into RX1 through the splitter and 20 dB, RX gain 15
dB, `bin/r1-rf.py`). 1R1T at 122.88 and 2R2T at 122.88 with 0x006 = 0x70 gave
the same tone level (within 0.5 dB), image and noise floor at every offset;
2R2T at 122.88 with Nuand's delay gave garbage (image within 2 dB of the
tone). The analog filter sets the bandwidth, not the interface:

| Tone offset (MHz) | +3 | +-10 | +-25 | +-40 | +-55 |
|---|---|---|---|---|---|
| 1R1T 122.88, default bandwidth (18 MHz), dBFS | -34.6 | -37.2 | -60.3 to -60.9 | not seen | not seen |
| 1R1T 122.88, bandwidth 56 MHz (`-w`), dBFS | -34.4 | -34.5 | -36.1 to -36.4 | -44.0 to -44.4 | -58.6 to -59.9 |
| the same, I/Q image (dBc) | -39 | -39 | -37 to -38 | -18 | -13 to -14 |
| 2R2T 61.44 (no OVERSAMPLE), bandwidth 56 MHz, I/Q image (dBc) | -64 | -63 | -30 to -31 | | |

libbladeRF keeps the RX filter at its 18 MHz default unless the bandwidth
is set after the sample rate (with OVERSAMPLE it warns that the result is
unknown). The image rejection with OVERSAMPLE, about -38 dBc near the
center, is the overclock's: the same in 1R1T and 2R2T.

**Automatic mode on either channel (R4, 2026-11-22).** The branch then
added:

- 797e214f hdl: FPGA version 0.17.0, a placeholder until Nuand assigns one;
  libbladeRF can tell images with 1R1T support apart only by the version.
- 6a00637f libbladeRF: `bladerf_set_rfic_1r1t_mode()` and
  `bladerf_get_rfic_1r1t_mode()` replace `bladerf_set_rfic_1r1t()`: AUTO
  (the default after every open), OFF (2R2T always) or ON (1R1T at every
  rate); `bladerf_get_rfic_1r1t()` reports the current mode. AUTO uses 1R1T
  mode while OVERSAMPLE is on, the FPGA has `BLADERF_CAP_FPGA_RFIC_1R1T`
  (v0.17.0 or later) and at most one RX and one TX channel are enabled. It
  decides after each sample rate change and before each enable, never on a
  disable, and writes nothing when 2R2T mode is wanted and already on.
- db5be887 hdl and 82551d8e libbladeRF: 1R1T mode on either channel, RX
  and TX independently. Bits 7:6 of the AD9361's 0x003 (RX) and 0x002 (TX)
  are 01 for channel 1 and 10 for channel 2; the core carries the channel
  that is on as its channel 0, and the hosted top level gives it to both RX
  slots and loads the TX data from the slot that has it. The MIMO enables
  pick the slot. A second channel of one direction gives 2R2T mode in AUTO
  and is refused in ON.
- c5079fb5, 1683a817, 7c722924 and 09953fe5 test: `test_1r1t_state`,
  `test_1r1t_meta` and `test_1r1t_stream` in `host/libraries/libbladeRF_test/`
  (from `r1-state.c` and `r1-meta.c`, plus a counter check at 122.88 Msps;
  `-c` selects the channel); c91131a3 the two cycle models in
  `hdl/fpga/platforms/bladerf-micro/vhdl/tb/models/`.
- hwtest-stream `-1` is mode ON (either channel) and `-2` mode OFF; without
  them libbladeRF's AUTO applies. `bin/r1-rf.py ... auto 1|2` measures the
  tone at +3 MHz on either channel.

Images (Quartus Prime Lite 23.1.1, Nios II, worst setup slack): hostedxA5
R4a = c91131a3 (AUTO on channel 1 only; interim, -0.188 ns) and R4 = 09953fe5
(-0.197 ns, hold +0.024 ns; the same AD9361 RX LVDS path as every build);
hostedxA9 R4-A9 = 09953fe5 (setup +0.046 ns, hold +0.052 ns; built, not run:
the boards must be swapped). Job `xa5r4`
(`results/xa5r4-20261122-065435`); R4 and AUTO unless noted, 122.88 Msps with
OVERSAMPLE:

| Check | RX1 / TX1 | RX2 / TX2 |
|---|---|---|
| `test_1r1t_state` (all mode changes, both channels, TX1 and TX2 on for a moment into the loads) | pass, 31 of 31 checks | |
| AD9361 test pattern (BIST SC8), 300 s | 3.69e10 samples, 0 bad | 3.69e10 samples, 0 bad |
| FPGA counter (`test_1r1t_stream`), 60 s | 7.37e9 samples, no jump, 122.880 Msps | 7.37e9 samples, no jump, 122.879 Msps |
| meta timestamps (`test_1r1t_meta`): SC8 122.88 AUTO; SC16 61.44 ON | pass; pass | pass; pass |
| AD9361 loopback (`-A`, SC8) at 76.8, 92.16 and 122.88 | bit-exact at 76.8 and 92.16; 122.88 fails (below) | bit-exact at 76.8 and 92.16; 122.88 fails (below) |
| B200mini tone at +3 MHz (RX gain 15 dB): level, I/Q image | -34.2 dBFS, -41 dBc | -35.3 dBFS, -46 dBc |
| the same in 2R2T at 61.44 Msps (no OVERSAMPLE) | -38.2 dBFS, -64 dBc | -39.4 dBFS, -62 dBc |
| 2R2T: counter at 122.88 (OFF), 10 s; BIST SC16 at 61.44, 10 s | pass (1.23e9 samples); 6.3e8 samples, 0 bad | not run; 6.3e8 samples, 0 bad |
| Image A with this libbladeRF: `test_1r1t_state` (AUTO stays 2R2T, ON refused), BIST SC16 at 61.44 | pass (7 of 7); 6.3e8 samples, 0 bad (RX1) | |

`test_1r1t_state` also showed that an RX bandwidth (56 MHz), frequency or gain
change after the rate keeps 1R1T mode. The B200mini reaches both RX ports
through the splitter: RX2 reads the tone about 1.2 dB lower than RX1 in both
modes, a path difference. The loopback at 122.88 Msps failed on both channels
in the same way: TX and RX together reached only 90.2 Msps (libbladeRF warns
that 245.76 Msps in total is above its 160 Msps recommendation), and the wrong
samples were other values of the sequence (gaps of lost or late blocks), not
corrupted values. That points to the USB link's limit, not the interface (not
yet checked with a control run). TX through the interface is bit-exact up to
92.16 Msps on both channels.

**FPGA tuning mode, TX at RF, and the xA9 (R4 and R4-A9, 2026-11-22).** AUTO
mode, 122.88 Msps with OVERSAMPLE unless noted. TX at RF: `bin/r1-txrf.py`
(job script `txrf-r4.sh`), cabled TX1 and TX2 through a 20 dB pad each into
the splitter and on to the B200mini's "RX2", every other port terminated; the
bladeRF sends a tone at FC + 3 MHz (FC 2450 MHz, TX gain 30 dB, TX cap), the
B200mini measures it at 16 Msps around FC and counts 1 ms dropouts over 4 s
at 2 Msps. A port swap is unlikely: in 1R1T mode the AD9361 powers only the
selected TX channel (0x002 reads 0x40 or 0x80). Jobs `xa5r4f`, `txrfa5` (xA5),
`xa9tx`, `txrfa9b`, `xa9rxrf` (xA9; the board behind the adapter's hub, as the
xA5).

| Check | xA5 RX1 / TX1 | xA5 RX2 / TX2 | xA9 RX1 / TX1 | xA9 RX2 / TX2 |
|---|---|---|---|---|
| FPGA tuning mode: `test_1r1t_state`; meta; counter 30 s; BIST SC8 60 s | pass (31 of 31); pass; no jump; 7.39e9, 0 bad | pass; no jump; 7.39e9, 0 bad | | |
| `test_1r1t_state`, host tuning mode | | | pass (31 of 31) | |
| meta timestamps (SC8 AUTO, SC16 ON); OFF SC8 | | | pass, pass; pass | pass, pass |
| FPGA counter (`test_1r1t_stream`), 60 s | (above) | (above) | 7.37e9, no jump, 122.880 Msps | 7.37e9, no jump, 122.880 Msps |
| AD9361 test pattern (BIST SC8), 300 s | (above) | (above) | 3.69e10, 0 bad | 3.69e10, 0 bad |
| AD9361 loopback (`-A`, SC8) | (above) | (above) | bit-exact at 76.8, 92.16; 122.88 fails (90.2 Msps) | the same |
| TX alone (`hwtest-stream tx`), 20 s: libbladeRF took | 100.3 % (FPGA and host tuning mode) | 100.3 % (both) | 100.3 % | 100.3 % |
| TX at RF, 1R1T: tone, image, carrier leak, worst spur | -45.4 dBFS, -36, -35, -53 dBc | -45.5 dBFS, -35, -34, -53 dBc | -46.5 dBFS, -37, -40, -56 dBc | -46.3 dBFS, -35, -36, -53 dBc |
| TX at RF, 2R2T at 61.44 (reference) | -45.5 dBFS, -36, -35, -53 dBc | -45.5 dBFS, -35, -34, -56 dBc | -46.4 dBFS, -36, -42, -57 dBc | -46.2 dBFS, -35, -36, -55 dBc |
| TX at RF, 2R2T at 122.88 (OFF) | broken: tone -51.3 dBFS, image -0.3 dBc | broken (the same) | broken: image 0.0 dBc | broken (the same) |
| TX at RF: dropouts in 4 s (1R1T, 2R2T 61.44) | 0, 0 | 0, 0 | 0, 0 | 0, 0 |
| 2R2T regression, BIST SC16 at 61.44 | | | 6.3e8, 0 bad | 6.3e8, 0 bad |
| image A with this libbladeRF: `test_1r1t_state`; BIST SC16 61.44 | | | pass (7 of 7); 6.3e8, 0 bad | |
| RX tone at +3 MHz (`r1-rf.py auto`) | (above) | (above) | -33.9 dBFS, image -36 dBc (2R2T 61.44: -38.2, -62) | -35.2 dBFS, image -34 dBc (2R2T 61.44: -39.4, -62) |

The loopback in the AD9361 cannot run in FPGA tuning mode: libbladeRF allows
the RFIC BIST loopback only in host tuning mode. The first xA9 TX runs showed
26 to 595 "dropouts" per run, in 2R2T at 61.44 Msps as well. They were the
analysis's: the xA9's clock is about 2 ppm off the B200mini's, 5 kHz at
2.45 GHz, a null of the 1 ms block mean, so the block levels followed the
noise (the xA5's 3.3 kHz offset was clear of it). `r1-txrf.py` now mixes the
tone down from its measured frequency; the saved xA9 captures then give 0
dropouts and a 0.08 to 0.13 dB spread. On both
boards the worst RX spur at 122.88 Msps is -32 to -38 dBc, 48 to 61 MHz from
the center (xA9 -32 to -35, xA5 -36 to -38), against -38 to -43 dBc at 61.44
Msps; the xA5's working 2R2T run at 122.88 (0x006 = 0x70, R) had a -34 dBc
worst spur too. RX2 reads the tone about 1.3 dB lower than RX1 on the xA9 as
on the xA5, in both modes.

**Scheduled TX, repeated starts, a soak, Nios V and every fork change (xA9,
2026-11-22).** New on branch `1r1t`: `test_1r1t_txsched` (48eb1329;
4d8467b9 finds a burst by the numbers of its first two samples, since the
TX holds its last sample between bursts; 7ac9f992 keeps up with the RX
stream and adds `-r`, `-o`, `-n`), `test_1r1t_stream -n` (b1f3f9f7), the
fifo_reader model cf388a33 and the fifo_reader fix c7ac1500 (below). Two local
test branches, not on the fork: `1r1t-niosv` (1r1t + niosv) and `fork-all`
(1r1t-niosv + adsb-fix + pr/cond-timedwait-timeout + the 12 pr/* branches +
fx3-epnak-master); every merge was clean. Images with Quartus Prime Lite 25.1
(Nios V), from 9495d531 and 35cbda3d (before the fix): N4 = hostedxA5 (setup
-0.068 ns, hold +0.072 ns), N4-A9 = hostedxA9 (+0.236 ns, +0.062 ns); fork-all's
N5 and N5-A9 are byte-identical to N4 and N4-A9. R5-A9 = R4-A9 + the fix
(Lite 23.1.1, +0.121 ns, +0.058 ns). The xA9 was in the RX cabling (B200mini ->
splitter -> a 20 dB pad each -> RX1, RX2; TX1, TX2 terminated), behind the
adapter's hub. Jobs `xa9940`, `xa9p1`, `xa9p1b`, `xa9p2`, `xa9p3`,
`xa9txdiag`, `r5a9`.

| Check (122.88 Msps, AUTO, unless noted) | R4-A9, 1r1t libbladeRF | N4-A9, 1r1t libbladeRF | N5-A9, fork-all libbladeRF |
|---|---|---|---|
| Test 2: 20 stream cycles in one session; 50 sessions (each channel) | pass; 50 of 50 | pass; 50 of 50 | pass; 50 of 50 |
| Soak: BIST SC8, 15 min per channel | 1.11e11 samples each, 0 bad | 1.11e11 samples each, 0 bad | not run |
| `test_1r1t_state`; meta (SC8 AUTO, SC16 ON); counter 60 s; BIST 300 s (each channel) | (earlier, R4-A9) | 31 of 31; pass; no jump; 3.69e10, 0 bad | 31 of 31; pass; no jump; 3.69e10, 0 bad |
| RX tone at +3 MHz: RX1, RX2 (tone, image) | (earlier) | -34.2 dBFS, -35 dBc; -35.4 dBFS, -34 dBc | (run-suite) |
| AD9361 loopback 76.8, 92.16; 122.88 | (earlier) | bit-exact; fails, USB limit (90.2 Msps) | |
| TX alone, 20 s (host and FPGA tuning mode) | (earlier) | 100.3 % | |
| FPGA tuning mode: state, meta, counter 30 s, BIST 60 s | (earlier) | 31 of 31, pass, no jump, 0 bad | |
| 2R2T regression, BIST SC16 61.44 | (earlier) | 0 bad | |
| run-suite.sh, receive only (smoke host fpga formats oc sched bist), HWTEST_FIXES=1 | | | 112 tests, 3 failed: rx_meta_8bit_mimo-host (overrun), sched_sc16meta_x1-host and sched_sc8meta_x2-host (one burst each after a 60-75 ms host pause); bist_oc_122M88 passes (it fails on image A) |
| run-suite.sh twoch, HWTEST_2CH=rx (only the B200mini transmits) | | | 4 tests, 0 failed |

**Test 1, scheduled TX bursts.** `test_1r1t_txsched` sends 100 bursts of
4,096 numbered samples, P apart, through the AD9361's internal loopback
(TX1->RX1, TX2->RX2) and checks their RX timestamps and samples. Three things
came out:

- **libbladeRF does not schedule SC8_Q7_META bursts** (Nuand's code, either
  mode): `handle_tx_parameters()` in `streaming/sync.c` reads the burst flags
  and the timestamp only for SC16_Q11_META. With SC8_Q7_META the first burst
  left at once (0.1 s before its timestamp) and the rest followed back to
  back (all images). OVERSAMPLE allows only 8-bit formats, so a scheduled TX
  at an OVERSAMPLE rate is not possible with Nuand's libbladeRF.
- **1R1T sent the start of every timed burst early** (SC16_Q11_META, mode ON,
  R4-A9 and N4-A9, both channels): an RX dump showed burst 1's samples 0-28
  leave about 1,700 samples after the previous buffer, the TX hold sample 28
  for 579,966 samples, and samples 29-4,095 leave exactly at the burst's
  timestamp (delay 37 samples, as burst 0); burst 2 sent 55 samples early.
  Cause: fifo_reader's META_DOWNCOUNT counts the registered read strobe and
  ends a message at `dma_downcount <= 2`, which assumes a FIFO read every
  fourth clock; in 1R1T with 16-bit samples the FIFO is read every other clock,
  the end is found one read late, and META_LOAD lets the next message's first
  word through without counting it. Each message boundary moves the data one
  word (2 samples) ahead of its metadata. The cycle model
  (`tb/models/fifo_reader_1r1t_model.py`) gives 33 and 65 early samples for
  bursts 1 and 2 (31 uncounted reads in 32 messages; 0 in 2R2T). c7ac1500 ends
  the message at `<= 4` with back_to_back_en and 16-bit samples (2R2T and
  8-bit unchanged). R5-A9 with the fix: 1R1T at 61.44 Msps on both channels
  passes (100 of 100 bursts bit-exact, exactly P apart, delay 37 throughout),
  and test_1r1t_state, meta, counter, BIST, the loopback and TX alone pass as
  before.
- Two smaller effects, in both modes and in Nuand's design: (1) at 30.72 Msps
  (no overruns) the first burst's delay is one sample longer than the rest
  (20 then 19 in 2R2T, 37 then 36 in 1R1T with the fix): the model shows the
  next burst's first sample leaving during META_LOAD and the TX holding it
  until the burst's time, so its second sample takes the first one's place;
  (2) at 61.44 Msps the RX stream overran about 130 times in 1.5 s (TX and RX
  together, behind the hub; 0 at 30.72), and after the overruns the 2R2T delay
  read 2 samples shorter (20 to 18, once 3) while 1R1T stayed at 37. Not yet
  checked: whether the 2R2T RX timestamps move after an RX overrun.

**Nuand/bladeRF#940** ("Sample Rate is Halved When Using
BLADERF_FEATURE_OVERSAMPLE"), job `xa9940`, 50 Msps, SC8, RX1: not
reproduced. On image A with the harness's Nuand host build, and on R4-A9 with
the 1r1t libbladeRF (AUTO: 1R1T, 0x003 = 0x54), with OVERSAMPLE on and off,
and with `bladerf_set_sample_rate()` and `bladerf_set_rational_sample_rate()`
(which `bladeRF-cli set samplerate` uses): the FPGA counter arrived at 49.995
to 50.001 Msps without a jump, and the B200mini tone at +3 MHz landed at
+2.995 MHz in all 8 captures (the 5 kHz is the clocks' difference); half the
rate would put it at +6 MHz. Both functions read back 50,000,000.

Not run: the FX3 endpoint-NAK firmware and ctrl_threads under it. The xA9 is
behind the adapter's hub (USB 2-1.3), and the 11-19 flash procedure
(`xa9-nak-master.sh`) writes the FX3 firmware only to a board on a root port.

**Scheduled TX with 8-bit samples, and images with both fixes (phase 4, xA9,
2026-11-22).** libbladeRF: `pr/sc8-meta-tx` f1e95d55 (off master; on the fork
only as a local branch) makes `handle_tx_parameters()` and the burst-end reset
in `streaming/sync.c` take SC8_Q7_META the way they take SC16_Q11_META. The
async path does not read the flags at all: there the program writes the
message headers itself. Test branch `fork-all-sc8` = fork-all + f1e95d55
(local), host build `host-build-sc8`.

With it, on R5-A9 (job `xa9p4a`, both channels), the SC8_Q7_META bursts were
scheduled: every burst arrived, at a fixed delay. 2R2T (OFF, 61.44 and 30.72
Msps, OVERSAMPLE) behaved like SC16 in 2R2T: delay 20, bursts after the first
one sample early (spacing 614,399 then 614,400). 1R1T (AUTO 122.88 Msps, ON
61.44 Msps) found each burst after the first 32,704 samples after the previous
one: its first samples left at the end of the previous buffer. SC16 ON 61.44
with the same build passed (delay 37, every spacing exactly P).

Cause, from the fifo_reader model with 8-bit samples (4 per FIFO word, a FIFO
read every fourth clock in 1R1T): the end of each message is found at the
right read, so nothing drifts, but the read that takes a message's last word
comes a clock before META_DOWNCOUNT counts it, and the sample FSM goes on with
the next word while the meta FSM passes META_LOAD: a timed burst's first 3
samples leave at the end of the previous buffer, and the rest 3 samples early.
Ending the message one read earlier, as c7ac1500 does for 16-bit samples,
makes every burst after the first late by one word per message (+61, +125,
+189 samples). The fix 7c43dc3b (1r1t): the meta FSM registers whether the
next message follows without a gap (its timestamp is the current one plus a
message of 8-bit samples, or "now"); if not, the sample FSM holds after that
last read until meta_time_go has dropped or the meta FSM reads the next
message's metadata. In the model every burst then leaves at its timestamp
without a gap, and messages that follow on still go out back to back; 16-bit
samples and 2R2T are unchanged (in 2R2T the model shows the first sample of
each burst after the first leaving during META_LOAD, for both sample sizes:
Nuand's design). Model update e641f302.

Images with both fixes (job `p4fpga`; worst slack on the AD9361 RX LVDS capture
path, as in every build): R6-A9 = hostedxA9 from 1r1t e641f302, Lite 23.1.1
(setup -0.347 ns, hold +0.020 ns); N6-A9 = hostedxA9 from fork-all f2f8d5e1,
Lite 25.1 (-0.284 ns, +0.062 ns); R6 = hostedxA5 from 1r1t (-0.563 ns, +0.008
ns); N6 = hostedxA5 from fork-all (-0.116 ns, +0.075 ns). R6 and N6 are not
run yet (the xA5 is on the MacBook Air). The xA9 runs (job `xa9p4b`, RX
cabling, behind the hub, libbladeRF `host-build-sc8`, TX into the
terminations with the TX cap):

| Check | N6-A9 RX1 / TX1 | N6-A9 RX2 / TX2 | R6-A9 RX1 / TX1 | R6-A9 RX2 / TX2 |
|---|---|---|---|---|
| `test_1r1t_state` | 31 of 31 | | 31 of 31 | |
| Counter 60 s, AUTO 122.88 | 122.879 Msps, no jump | 122.879 Msps, no jump | | |
| BIST SC8 122.88, 60 s | 7.39e9 samples, 0 bad | 7.39e9 samples, 0 bad | 7.39e9 samples, 0 bad | |
| AD9361 loopback 76.8, 92.16 | bit-exact | bit-exact | | |
| Test 1, SC16_Q11_META, ON 61.44 | pass, delay 37, every spacing P | pass, delay 37 | pass, delay 37 | pass, delay 37 |
| Test 1, SC8_Q7_META, AUTO 122.88 (1R1T) | pass, delay 38, every spacing P | pass, delay 37 | pass, delay 38 | pass, delay 38 |
| Test 1, SC8_Q7_META, ON 61.44 (1R1T) | pass, delay 37 | pass, delay 37 | | |
| Test 1, SC8_Q7_META, OFF 61.44 and 30.72 (2R2T references) | delay 20 (19 at 30.72), the bursts after the first one sample early | the same | | |

So with `pr/sc8-meta-tx` and 7c43dc3b a scheduled TX at 122.88 Msps works
in 1R1T mode on both channels: every burst bit-exact, exactly P apart, at one
delay. The 2R2T references still fail the strict check by the one-sample
effect described above (Nuand's design, both sample sizes).

### 22. Timed TX bursts: two fifo_reader faults, their fixes, and 1R1T v2 (2026-11-22)

**The check.** `bin/txburst-check.py` sends numbered timed bursts (16 bursts of 4096 samples, 1 ms
apart unless noted) through the Python binding into the AD9361's internal loopback and checks every
received sample, the gaps included; `test_1r1t_txsched` checks only the bursts, so it never saw
what follows. TX gain at its minimum on every TX channel, all TX ports on 50 ohm loads. SC16 runs
use Nuand's libbladeRF (`host-build`); SC8 runs use `host-build-sc8`, because Nuand's libbladeRF
sends timed SC8_Q7_META bursts at once (`pr/sc8-meta-tx`, upstream draft I26).

**Two faults in Nuand's `fifo_reader.vhd`** (image A, v0.16.0, with Nuand's libbladeRF; xA9 and
xA5 alike):
1. Bursts after the first leave one sample early, and the gap before each burst holds the burst's
   first sample instead of zeros (one channel with SC16 or SC8; two channels with SC8). At 30.72
   Msps the held sample lasted 22,544 samples of each 1 ms period. In the meta FSM, `meta_time_go`
   stays high while META_LOAD runs at the end of a message, so a sample request there reads the
   next message's first sample, and bladerf-hosted.vhd's DAC register, which latches only valid
   samples, holds it until the burst's timestamp (the cause is from code reading and a cycle
   model; the effect is measured). Upstream draft I27.
2. Two channels with SC16: bursts after the first leave 19,475 to 19,588 samples early (8192-sample
   TX buffers) and lose or cut samples; the offset changes by one sample per message (8 per burst
   with 8192-sample buffers, 16 with 32768). With two 16-bit channels the FIFO is read on every
   sample request; a read during META_LOAD or META_WAIT takes the next message's first word, and
   META_WAIT starts the next message's count without it. Upstream draft I28.

Continuous timed streams are not affected: the samples stay in order.

**The fixes**, each one commit on master 35174c30: `pr/fifo-reader-early-sample` (P27): at the end
of a message, samples go on through META_LOAD only when the next message follows without a gap, or
is to be sent now; `pr/fifo-reader-msg-count` (P28): the reads made during META_LOAD and META_WAIT
count toward the next message. They merge without a conflict (`test-fifo-reader-both`). Images
built with Quartus Prime Lite 23.1.1: G (both), GA (P27 only), GB (P28 only).

| Case | A (Nuand) | GA (P27) | GB (P28) | G (both) |
|---|---|---|---|---|
| 1 channel SC16, 30.72 and 61.44 Msps, TX1 and TX2 | 1 early, gaps held | pass | 1 early, gaps held | pass |
| 1 channel SC8 (OVERSAMPLE, 2R2T), 61.44 Msps | 1 early, gaps held | pass | 1 early, gaps held | pass |
| 2 channels SC8, 30.72 Msps | 1 early, gaps held | pass | 1 early, gaps held | pass |
| 2 channels SC16, 30.72 Msps | about 19,500 early, 60,480 wrong samples | still drifts | 1 early, gaps held | pass |
| continuous: 1 channel SC16 30.72 and SC8 61.44, 2 channels SC16 15.36 | pass | pass | pass | pass |

xA9: all four images; xA5: A and G, with the same results. "Pass" is one fixed delay for every
burst, zeros in every gap and every burst sample right. With image A, a zero first sample in every
burst (`--zero-first`) gives zeros in the gaps and the payload on time; only the first burst after
the TX start then leaves one sample late. Limits of the check: two 16-bit channels at 30.72 Msps,
continuous or with 32768-sample TX buffers, overrun the RX (USB 3), with any image; those cases run
at 15.36 Msps. A stream cut short leaves its last sample in the DAC register, and the next run sees
it before its first burst.

**1R1T v2.** `pr/1r1t` is rebuilt on P27 and P28 (force-pushed, 723b2e1f): its `fifo_reader`
commit now makes the meta FSM count the FIFO's own read strobe, which in 1R1T mode advances the
FIFO in the cycle of the request, so a message ends at the read that takes its last word. That
replaces c7ac1500 (end 16-bit messages one read earlier) and 7c43dc3b (hold 8-bit samples at a
burst's start). R7 (Nios II, this branch with `pr/sc8-meta-tx`) and N7 (`fork-all-v2`: Nios V,
1R1T v2 and every fork fix, Quartus Prime Lite 25.1) on the xA9 and the xA5: `test_1r1t_state`,
`test_1r1t_txsched` (SC8 AUTO 122.88 and SC16 ON 61.44 Msps, both channels), `test_1r1t_meta`,
`test_1r1t_stream` and `txburst-check.py` (1R1T bursts with SC8 at 122.88 and 61.44 and SC16 at
61.44 Msps on both channels; continuous 1R1T SC8 at 61.44 and SC16 at 30.72 Msps; 2R2T with one and
two channels): 17 of 17 pass per board and image. Continuous 1R1T TX and RX at 122.88 Msps (SC8)
or 61.44 Msps (SC16) together overrun the RX (USB 3).

**RX timing of the new builds.** Worst setup slack, all on the AD9361 RX LVDS capture: G -0.322 ns
(xA5), +0.357 ns (xA9); R7 -1.076 ns (xA5), -0.528 ns (xA9); N7 -0.791 ns (xA5), -0.225 ns (xA9);
master's build -0.154 ns (xA5). On the boards: AD9361 BIST 60 s per channel clean for every image
(122.88 Msps 1R1T for R7 and N7, 61.44 Msps 2R2T for G and A), and the RX delay sweep (register
0x006, 1 s per setting) found the same clean window for A, G, R7 and N7 on the xA5: clock delay 1
to 5, where the default (5) sits at the window's edge, with Nuand's image too; on the xA9, R7-A9
was clean from 2 to 7.

**FX3 endpoint-NAK firmware with every fork change (xA9).** Job `xa9nakf` (flash, then restore):
no FX3 lockup in 60 two-channel bladeRF-cli tries (N6, R6, A), 23 rx2order tries (20 in the bad
order) or the campaign (N6, `host-build-sc8`: 143 tests, the known 3 failures and 3 rxsched misses
after host pauses). ctrl_threads first stopped at a 300 s limit and under the TX cap, both wrong:
passing runs take about 550 to 650 s, and the cap fails the test's own gain checks. Job `xa9nakc`
(flash, then restore), no cap with the TX ports on loads, 1200 s limit: N6 3 of 3 (547 to 551 s)
and A 3 of 3 (641 to 653 s) pass, the FX3 answered after every run, and the whole flash was again
identical to the 2026-11-18 inventory. `run-suite.sh`'s 300 s limit for ctrl_threads is too short
for a pass (open item).

Results: `results/nak-forkall-20261122-155030/`, `results/nak-ctrl-20261122-174223/`,
`gapfix/res-*-1122-*`, `results/xa9r7t-20261122-202215/`, `results/xa5p7-20261122-211758/`.

## Evidence per change

### Branch `niosv`: Quartus 25.1 and Nios V for the bladeRF 2.0 micro

#### The Quartus 25.1 build (Nios V)

- Builds: xA4, xA5, xA9 with Quartus Prime Lite 25.1 (section 2); the
  micro's other revisions, wlan xA9, adsb xA4/xA5/xA9 (with Nuand's decoder
  and the adsb fix) and foxhunt xA4/xA5/xA9, build too, with the same timing
  as Nuand's own builds of them (section 11).
- Image C passes everything image A passes, in every campaign above
  (sections 3 to 7), in both tuning modes, with Nuand's libbladeRF and with
  the fixes; RF performance matches A on channel 2 (section 4) and on
  channel 1 (section 13), and on both channels of the xA9 (section 18); no
  time-out in 150-minute receive and 90-minute transmit soaks (sections 3,
  5).
- Image B: the restructured Qsys script still builds Nuand's Nios II design,
  with the same fit and timing as A, and passes the same tests.
- Tested on hardware on two boards: the xA5 (sections 3 to 15 and 19) and
  an xA9 rev 1.3 (section 16: smoke to bist on A, B and C, deep, fullsoak
  and soak on A and C; C failed only tests A failed too; section 17: Nuand's
  threads test passes on A and C, and the Nios V adsb and wlan images
  behave like Nuand's code; section 18: at RF against the B200mini on both
  channels, A, B and C the same, C's one overclocked transmit failure in 96
  cases within chance; section 19: both channels at once, A, B and C the
  same, on both boards). The xA4 was built only.

#### 3a6c0a83 (tested as d1aabacb) nios: don't register an ISR for a VCTCXO tamer without an IRQ

- On the micro, `VCTCXO_TAMER_0_IRQ` is −1, and `alt_ic_isr_register()`
  stores the handler in `alt_irq[-1]`, the 8 bytes before the table. Found by
  reading the Nios V port's memory map. In every Nios II build, Nuand's
  v0.16.0 included, those bytes are the end of the RX retune queue, which is
  written before it is read, so the write has done no harm. In every Nios V
  build of the branch (hosted, wlan, adsb and foxhunt; xA4, xA5 and xA9) they
  are `adf400x_reg[2]` and `[3]`, the firmware's copy of the ADF400x
  registers, which the host reads back. So the port needs the fix, and it is
  on `niosv` rather than `host-fixes`.
- Images B and C have the change and pass everything A passes (sections 3 to
  7).

#### eb6b7670 (tested as 9db0f794) nios: let the firmware's assert replace the C library's

- Needed for the Nios V build only: newlib-nano's headers include
  `<assert.h>`, so the firmware's own `assert` is a redefinition, which
  `-Werror` turns into an error. A Nios II build is unchanged.

#### 03f3ef81, 72f37da8 (tested as e60da19b, fe4bb32d): share the Nios V firmware build; build the foxhunt firmware for Nios V

- e60da19b moves the application-independent part of the Nios V firmware
  build into `niosv_app.cmake`: the hosted revision's RAM image is
  byte-identical before and after (xA4, Quartus 25.1,
  `nios_system_ram.hex` md5 57a32993323672674ee56998429aa8f4), also when
  built inside the foxhunt revision.
- fe4bb32d: foxhunt builds with Quartus 25.1 for xA4, xA5 and xA9 (section
  11); its Nios V firmware is 82.6 KB, 43.0 KB of the RAM left. With
  Quartus 23.1.1 the branch still builds foxhunt with Nios II exactly as
  Nuand's code does (same Qsys system, fit and timing).
- On the bench (section 12) the Nios V foxhunt sends the same keyed tone as
  Nuand's Nios II build: same text, timing, frequency and level; only its
  10 s pause between messages is exactly 10 s (14.0 s on the Nios II/e).

### Branches `host-fixes` and `adsb-fix`: problems that occur with Nuand's code too

These problems do not depend on the Nios V port. The tests ran on the earlier
branch `fixes`, which also had the VCTCXO tamer fix; `host-fixes` has the
same commits without it, with the same libbladeRF code: 7eabdcb3, e9463ba9,
fdb03b4e and 3b60e59e there are a91e9fd9, 2ac75f37, dbad1e70 and e64e4026
here.

For the pull requests each fix also has its own branch from master (`pr/*`;
a91e9fd9 is split into a tune-order and a buffer-count change), and each was
checked alone against master on the xA9 (section 17, "Each fix alone").

#### 3a768f3d libbladeRF: discard stale Nios responses

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
- Without the fix, host tuning mode (libbladeRF's default) avoids it: three
  full sweeps had no time-out and passed (section 20).

#### 2ac75f37 libbladeRF: enable both channels of a MIMO layout in FPGA tuning mode too

- Without the fix, `test_rx_meta -m` and `-b 8 -m` fail in FPGA tuning mode
  with every image (A, B, C); with it they pass on all three (sections 4, 6).

#### dbad1e70 libbladeRF: initialize the RFIC again after an FPGA load in host tuning mode

- Without the fix, `test_fpga_load` fails in host tuning mode with every image
  ("Calibration TIMEOUT"); with it, it passes on A, B and C (section 6,
  fpga_load-host), and in FPGA tuning mode as before.

#### e64e4026 libbladeRF: host tuning mode: MIMO enable uses the layout of the channel's direction

- bladeRF-cli receives 3 s on RX1+RX2 at 5 Msps while TX1 sends 0.2 s:
  without the fix the receive stops when the transmit ends (2,228,224 of
  30,000,000 samples); with it all 30,000,000 samples arrive, in both tuning
  modes (rx2_txtoggle-host/-fpga, section 6, on A, B and C).

#### a91e9fd9 test_scheduled_retune: tune before reading the start timestamp; 64 RX buffers

- Before: "Requested timestamp is in the past" with the official image. After:
  passes in every run since, all images, both tuning modes, e.g. 135 times in
  a row in the C transmit soak.
- The 64 RX buffers against Nuand's 16, both binaries with the tuning
  change and `host-fixes`' libbladeRF (`results/sched-ab-1116-2208`,
  2026-11-16, bladeRF on its own port, receive only, 30 runs per image and
  tuning mode): with 16 buffers 72 of 120 runs failed
  ("Requested timestamp is in the past", from the third iteration on; A 40
  of 60, C 32 of 60), with 64 buffers none of 120.

#### 06962e80 libbladeRF: lock the device while looking up gain ranges

- Symptom: in `test_ctrl -T fpga -t threads`, Nios requests that are never
  answered, and replies taken by the wrong request.
- Cause shown in the packet traces of section 14: on the bladeRF 2.0 the
  gain-range lookups read the frequency from the Nios without the device
  lock, so the gain thread's request went out while another thread's was
  unanswered (once in 10,935 requests and 3 times in 852,185); twice the
  other request was never answered.
- With the fix: no such overlap in 5.8 million requests (six runs, A and C).

#### dc41000f libbladeRF: bladeRF 2.0 FPGA tuning mode: sign-extend RX gain readbacks

- Symptom: `bladerf_get_gain()` returns -2147483648 for RX gains whose
  full-stage value is negative (above 1.3 GHz), in FPGA tuning mode only.
- Reproduced with `lib/gain-readback.c`: 5 of 14 readbacks wrong on A and on
  C with Nuand's libbladeRF, 0 in host tuning mode; with the fix 0 of 14 in
  both modes on both images (section 14).

#### 378b9909 test_ctrl: threads: start the control threads once both streams run

- Symptom: wrong RX gain readbacks at the start of the run (141 reading 60),
  or for the whole run when the streams start too late and time out
  (47,995), and stream failures.
- With the fix the streams start before the control threads in every run
  since (section 14).

#### 15299a8b test_ctrl: gain: check readbacks against the range the gain is clamped to

- Symptom: "Erroneous overall gain readback=-15, expected=-16" (or -12) when
  the frequency thread has moved the channel to a band with a higher minimum
  gain (3 failures in the first 8 minutes of a run, its only ones until the
  FX3 stopped answering).
- With the fix: 0 gain readback failures in the four runs since, A and C
  (section 14). The single-threaded gain test is unchanged in effect: with
  the frequency fixed, the gains are within the range and the check is the
  same.

#### adsb-fix 14616827, 58d1c2c8: tie off fifo_writer's highly_packed_mode_en; size the 12-bit packing buffer from the FIFO width

- Nuand's master does not build the adsb revision (since 2025.05, with
  Quartus 23.1.1 too): rx-adsb.vhd leaves fifo_writer's new
  `highly_packed_mode_en` port open ("Error (10346) ... must have actual or
  default value"; 14616827 ties it low), and fifo_writer's 96-bit packing
  buffer is narrower than the adsb revision's 128-bit sample FIFO ("Error
  (10393) ... fifo_writer.vhd(567): left bound (127) of slice must belong to
  range (95 downto 0)"; 58d1c2c8 sizes it FIFO_DATA_WIDTH + 32 bits, still
  96 for the other revisions).
- With both, adsb builds: Nuand's code with Quartus 23.1.1 (xA4) and `niosv`
  with 25.1 (xA4, xA5, xA9) (section 11).
- The branch itself, built alone (2026-11-19, `adsb-fix` 58d1c2c8 with
  Nuand/bladeRF-adsb 40b7599, Quartus 23.1.1): adsbxA4 15,884 ALMs (86 %),
  worst setup −2.814 ns, byte-identical to the 2026-11-14 build of master
  with the two changes as a patch (md5 f7181684…); adsbxA9 15,838 ALMs
  (14 %), worst setup −1.923 ns (the decoder at the 125 MHz constraint, as
  in section 11; md5 e6f31fbe…).
- On hardware (xA9, section 17): that adsbxA9 and the Nios V image decode
  the ADS-B loopback's frames alike, every message exact, both losing about
  1 frame in 10,000 (open item 14).

### Branch `1r1t`: 1R1T mode for one channel at 122.88 Msps

On the fork since 2026-11-22; not proposed upstream yet (section 21).
Built and tested on the xA5 and the xA9 (R4, R4-A9, R5-A9, R6-A9) with Quartus
Prime Lite 23.1.1 (Nios II), and with the Nios V port (N4-A9, N6-A9, Lite 25.1).

- 10494cab hdl: the 1R1T frames in the AD9361 interface module, the R1
  bits exported from the core, the time tamers and the hosted top level.
  Evidence: in 1R1T mode BIST bit-exact from 30.72 to 122.88 Msps, the
  FPGA counter complete at 122.879 Msps, a delay window of 6 steps at
  122.88 Msps (2R2T: 1 step), scheduled bursts on time; in 2R2T the same
  results as before.
- 7d7480fa libbladeRF: `bladerf_set_rfic_1r1t()`. Evidence: r1-state.c,
  both tuning modes.
- aeb7039e hdl: fifo_reader reads ahead in 1R1T mode. Evidence: the
  loopback in the AD9361 swapped sample pairs without it and is bit-exact
  with it.
- d66391ae hdl: exact RX meta boundaries in 1R1T mode. Evidence: r1-meta.c
  message timestamps 4,089/2,046 apart without it, 4,088/2,044 with it.
- d15ec490 libbladeRF: RX2 and TX2 refused in 1R1T mode. Evidence:
  r1-state.c (RX2; TX2 is refused by the harness's no-TX guard first).
- 797e214f hdl: FPGA version 0.17.0. Evidence: R4 reports v0.17.0, and
  libbladeRF selects 1R1T mode on it and not on image A.
- 6a00637f libbladeRF: automatic 1R1T selection (AUTO, OFF, ON). Evidence:
  `test_1r1t_state` on R4 and on image A.
- c5079fb5, 1683a817, 7c722924 test, c91131a3 hdl models: the 1R1T tests.
  Evidence: they pass on R4 (section 21).
- db5be887 hdl, 82551d8e libbladeRF, 09953fe5 test: 1R1T mode on either
  channel. Evidence: on R4, RX2 bit-exact at 122.88 Msps (BIST, counter),
  meta timestamps exact, the loopback TX2 to RX2 and the tone at +3 MHz as
  on channel 1.
- 48eb1329, 4d8467b9, 7ac9f992 test: `test_1r1t_txsched`; b1f3f9f7 test:
  `test_1r1t_stream -n`. Evidence: they pass on R6-A9 and N6-A9 (section 21).
- c7ac1500 hdl: fifo_reader ends a 16-bit TX message one read earlier in
  1R1T mode. Evidence: without it each timed burst after the first sent 29+
  samples early (33 and 65 in the model); with it SC16 bursts are exact at
  61.44 Msps on both channels (R5-A9, R6-A9, N6-A9).
- 7c43dc3b hdl: fifo_reader holds 8-bit samples at a timed burst's start in
  1R1T mode. Evidence: without it each SC8 burst after the first left its
  first samples at the end of the previous buffer (R5-A9 with
  `pr/sc8-meta-tx`; 3 samples in the model); with it SC8 bursts are exact
  at 122.88 Msps (AUTO) and 61.44 Msps (ON) on both channels (R6-A9, N6-A9).
- cf388a33, e641f302 hdl: the fifo_reader cycle model (16-bit, 8-bit, the
  two fixes).

### Branch `pr/sc8-meta-tx`: libbladeRF schedules SC8_Q7_META bursts

Local (2026-11-22), off master 35174c30; tested as `fork-all-sc8`
(fork-all + this).

- f1e95d55 libbladeRF: the sync TX path honors BURST_START, TX_NOW, the
  timestamp, UPDATE_TIMESTAMP and BURST_END with SC8_Q7_META too. Evidence:
  without it every SC8 burst left at once (all images); with it every burst
  arrived at a fixed delay in 2R2T and, with 7c43dc3b, exactly on time in
  1R1T at 122.88 Msps (section 21).

## Known noise and open items

1. `rx_meta` with two channels overruns about once in 20 runs, with either
   libbladeRF build and every image (16 buffers of 4096 samples at 4 Msps:
   16 ms of slack). Not a regression. On the xA9 in FPGA tuning mode
   (section 17): 2 of 14 runs with [#1123] alone (both in one session, at the
   same sample), 0 of 17 with all fixes.
2. Run times of `ctrl_bandwidth-fpga` vary between 10 and 26 s in every
   configuration; integer-second timings make small differences look large.
3. Test order matters: `test_digital_loopback -l fpga` leaves the RX mux at
   DIGITAL_LOOPBACK (libbladeRF's open does not reset it), and
   `test_digital_loopback -l rfic` fails its first run after an image load or
   after another program transmitted. Same with Nuand's libbladeRF.
4. `test_ctrl -T fpga -t threads` (Nuand's test): fails with Nuand's
   libbladeRF and test on every image. Those failures came from three
   libbladeRF bugs and two bugs in the test, not from the images (section
   14). With `host-fixes` the fast variant passes on A and C. In 11 full
   runs since, the only failures were the test's own race (test_gain's
   420 MHz set, left for Nuand to decide) and, with the bladeRF behind a USB
   hub, two stops: the sample streams, which loop back in the FX3, stopped
   (A), and the streams stopped and then the FX3 did not answer until a
   power cycle (C). A fast run on A hung the FX3 the same way. Five such
   hangs since 2026-11-13: four behind the hub, and one (A, 2026-11-18) with
   the bladeRF on its own port, after which the FX3 came back from a port
   reset with corrupted descriptors (upstream [#1118 comment 3]). The
   cause is open (section 14); an FX3 lockup with a known trigger and fix is
   item 10.
   The harness's own run of the test
   (`ctrl_threads-fpga`, with the TX cap and a 5-minute limit) cannot pass:
   the cap clamps the gain thread's TX gains, and the test needs about 10
   minutes. It stays out of unattended campaigns
   (`HWTEST_SKIP=ctrl_threads-fpga`). Earlier runs
   (`results/ctrlthreads-1115-0933/`, `ctrlthreads-fixes-1115-0955/`,
   `ctrlthreads-nuand-1115-1044/`) blamed the Nios time-outs on the 250 ms
   wait and most readbacks of 60 on the TX cap; section 14 shows the time-outs
   are harmless on their own and that 60 is also what RX reads back while
   RX is disabled.
5. Nuand's foxhunt firmware as it is (TX1, TX bias tee on) has not been run:
   the bench terminates TX1 and has no DC block. The bench images differ
   from it in those two lines only (section 12).
6. The first one to four bursts after a stream starts can overrun, on every
   image (section 10; upstream issue [#1139]).
7. Seen in Nuand's designs, not caused by these changes: foxhunt keys at
   twice the speed its firmware asks for, and Nuand's Nios II/e `usleep()`
   runs about 40 % long (section 12); the wlan revision fails recovery timing
   and the adsb revision setup timing in Nuand's own builds (section 11).
8. Both channels' RF at the same time: twoch-check.py does it through a
   2-way splitter (README.md). Receive and transmit pass on A, B and C, in
   both tuning modes, on the xA9 and the xA5 (section 19), and so do TX1's
   and TX2's phase and timing: TX2 at lag 0, its phase stable after
   retunes. The xA5's transmit had two failures at 5.8 GHz (open item 16).
9. Seen on Nuand's image as on ours, on the xA5: TX1 is weaker around
   100-125 MHz than elsewhere (up to 14 dB) and than TX2 at 100 MHz
   (13 dB), and RX1 is 3.5 dB weaker than RX2 at 5.8 GHz (section 13). The
   xA9's TX1 is flat from 70 to 433 MHz and as strong at 100 MHz as the
   xA5's TX2 (section 18), so the TX1 dip belongs to that xA5 board. RX1
   is weaker than RX2 at 5.8 GHz on the xA9 too (2.0 to 2.3 dB, section
   18), so that difference is not one board's. Measured on both channels
   at once, with the two paths exchanged, the xA9's RX2 is 1.5 dB stronger
   than RX1 at 5.8 GHz and 0.8 dB at 3.5 GHz, and the paths add less than
   0.1 dB (section 19). Through the same paths, its TX2 is 1.3 dB weaker
   than TX1 at 5.8 GHz. Measured the same way, the xA5's RX2 is 1.9 dB
   stronger than RX1 at 5.8 GHz and 0.9 dB at 3.5 GHz, and its TX2 0.4 to
   0.5 dB weaker than TX1 (section 19). The start of every
   transmission brings up to ~0.7 s of short, weak, off-frequency bursts
   (section 13; cause not investigated).
10. An FX3 lockup with a known trigger and a fix (2026-11-18, the xA9 on a
    MacBook Air, receive only; not yet a section here, see upstream issue
    [#1118]). A two-channel receive started in FPGA tuning mode in bladeRF-cli's
    order makes Nuand's libbladeRF reset the receive endpoint while the
    stream's transfers are queued. FX3 firmware 2.6.0 then locked up in 4
    of 14 runs, on every FPGA image, and on Linux on the 6th run (the XPS,
    section 16). Branch `fx3-epnak` (46fa04d1: NAK the endpoint around the
    reset, as the FX3 SDK's examples do) locked up in 0 of 40 on the MacBook
    Air, and a 10-minute soak of normal use with it passed. On Linux
    (section 17) it locked up in 0 of 80, and so did the same change
    rebased onto master (`fx3-epnak-master`, FX3 2.6.1). Results, images
    and tools: `results/xA9-mba/`, `results/nak-linux-*`, `nak-master-*` on
    the XPS.
11. To investigate: the FX3 firmware built with the FX3 SDK 1.3.5
    libraries. Nuand's 2.6.0 links the SDK's 1.3.3 libraries: it rebuilds
    byte for byte from the 1.3.5 SDK package with `FX3_SDK_VERSION` "1.3.3"
    in `fx3_firmware/cmake/fx3-toolchain.cmake` (upstream [#1118 comment 2]).
    With "1.3.5", with or without item 10's fix, the firmware stops answering
    FPGA requests after a flash write until the FX3 reboots: libbladeRF's
    next open times out in `get_fpga_version()`. Seen twice on the xA9
    (2026-11-18 14:56 and 16:25, each right after `bladeRF-cli -f` wrote the
    FX3 firmware area). After a flash read it answers, and so does the 1.3.3
    build after a write. A lead: 1.3.5 changed `CyU3PSpiTransmitWords()`,
    which flash writes use (a data-corruption fix, and
    `CyU3PSpiSetTxFlushWaitPeriod()` added). To reproduce: run a
    1.3.5-library build (`fx3-epnak` at 8a0ff54e, or 09c82087 with "1.3.5";
    images in `results/xA9-mba/fx3/`), write a flash region, then open the
    device without `BLADERF_FORCE_NO_FPGA_PRESENT`. Testing writes a board's
    flash, so ask first. `fx3-epnak` reverts the switch (6e06b1d6), so the
    fork stays on the 1.3.3 libraries for now. The SDK packages, their
    release notes as text and the library-matching tool are in
    `~/workarea/fx3-sdk/` and `~/workarea/builds/scripts/fx3libmatch.py` on
    the XPS.

12. libbladeRF reports a timed-out wait as "An unexpected error occurred"
    (BLADERF_ERR_UNEXPECTED) instead of BLADERF_ERR_TIMEOUT, and bladeRF-cli's
    `rx wait <time>` never times out: `thread.h`'s `posix_cond_timedwait()`
    returns -1 where its callers expect ETIMEDOUT (section 17; upstream
    issue [#1025]). Upstream PR [#1060] makes the same fix as our branch
    `pr/cond-timedwait-timeout`.
    Programs that poll `bladerf_sync_rx()` with short time-outs (version 1
    of `lib/wiphy-loop.c`) stop on it.
13. The wlan revision (Nuand's code since 950189d2, 2024-03-06, and the Nios
    V build alike) sends only the first 802.11 frame after an FPGA load as
    the host asked: `fifo_reader.vhd` line 261 (`dma_downcount <=
    packet_len - 1`) ends each TX packet one dword early for wlan_top's
    one-dword reads. With `packet_len` (as before 950189d2) every frame is
    reported (section 17, A/B). 950189d2 made fx3_gpif_packet_tb pass, whose
    consumer holds packet_ready high; a fix must suit both kinds of reader.
    Upstream issue [#1121].
14. The adsb revision loses about 1 decoded message in 10,000 (1,000 frames
    a second through the AD9361 loopback), in a fixed pattern in time, not
    from host stalls (section 17). Probable cause: its RX FIFO is exactly
    one GPIF buffer deep, so the GPIF reads only when the FIFO is full.
    Nuand's own build (Quartus 23.1.1, Nios II) loses frames the same way;
    a deeper FIFO has not been tried. Upstream issue [#1143].
15. Overclocked transmit (122.88 Msps, past the 80 Msps up to which the
    xA9's data port carries every TX bit, section 17) sometimes reaches the
    B200mini with glitches. Short false pulses (tens of ns) came in about
    half of the xA5's channel 1 cases, on A and C alike (section 13), and in
    1 of the xA9's 240 cases (C, channel 2). No false pulses on the xA5's
    channel 2 or the xA9's channel 1. Cause not investigated. (One more
    failure on C was the B200mini's own start, which the harness now drops;
    section 18.)
16. Now and then the tone that the B200mini receives from the bladeRF at
    5.8 GHz reads 2.6 to 5.2 dB low in single bladeRF-cli runs (both
    channels alike when both send), while the other runs at the same point
    read normal (section 19, the xA5): 6 of about 230 runs on the xA5
    (twoch), 1 of about 115 on the xA9 (rf-check, image B, host tuning
    mode, 2026-11-19 23:52, both captures of that run). None at 3.5 GHz or
    below. Seen with A, B and C and in both tuning modes; not seen yet
    with the all-fixes build, in fewer runs (not enough to tell). Not a
    Nios time-out or a reply to another request (upstream issue [#1108]): in
    the four traced low runs the requests, their values and the Nios's
    replies were the same as in normal runs. Cause not found; whether it
    is in the bladeRF or in the B200mini is not known.
17. The XPS 13 refreshes its package cache every 2 hours (`mint-refresh-cache`
    at :11:52 and again at :12:52). Once that stalled a 122.88 Msps SC8 stream
    for 0.4 ms (section 21, the R2 soak): 145 phase losses, then clean. Long
    runs should avoid those minutes, or check losses there against the system
    log.
18. Not yet covered in 1R1T mode (section 21): the xA5 with the fixed
    images (R6, N6) and with every fork change, the TX level across the band
    (only FC + 3 MHz), RF recordings to disk at 122.88 Msps, and the FX3 NAK
    firmware with every fork change and ctrl_threads under it (needs the xA9
    on a root port). A scheduled TX at 122.88 Msps needs `pr/sc8-meta-tx`
    (libbladeRF) and 7c43dc3b (FPGA); the one-sample effect at a timed
    burst's start in 2R2T (both sample sizes) is Nuand's and not changed.
    The other revisions (wlan, adsb, foxhunt) build the same Qsys system but
    do not use 1R1T mode; only hostedxA5 and hostedxA9 were built. FPGA
    version 0.17.0 is a placeholder: a later Nuand v0.17.0 without 1R1T
    support would wrongly get AUTO.

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

<!-- Links to the upstream issues and pull requests (posted 2026-11-21) -->
[#1025]: https://github.com/Nuand/bladeRF/issues/1025
[#1060]: https://github.com/Nuand/bladeRF/pull/1060
[#1108]: https://github.com/Nuand/bladeRF/issues/1108
[#1109]: https://github.com/Nuand/bladeRF/issues/1109
[#1117]: https://github.com/Nuand/bladeRF/issues/1117
[#1118]: https://github.com/Nuand/bladeRF/issues/1118
[#1118 comment 2]: https://github.com/Nuand/bladeRF/issues/1118#issuecomment-7072919401
[#1118 comment 3]: https://github.com/Nuand/bladeRF/issues/1118#issuecomment-7072920046
[#1120]: https://github.com/Nuand/bladeRF/issues/1120
[#1121]: https://github.com/Nuand/bladeRF/issues/1121
[#1122]: https://github.com/Nuand/bladeRF/pull/1122
[#1123]: https://github.com/Nuand/bladeRF/pull/1123
[#1124]: https://github.com/Nuand/bladeRF/pull/1124
[#1125]: https://github.com/Nuand/bladeRF/pull/1125
[#1126]: https://github.com/Nuand/bladeRF/pull/1126
[#1127]: https://github.com/Nuand/bladeRF/pull/1127
[#1128]: https://github.com/Nuand/bladeRF/pull/1128
[#1129]: https://github.com/Nuand/bladeRF/pull/1129
[#1130]: https://github.com/Nuand/bladeRF/pull/1130
[#1131]: https://github.com/Nuand/bladeRF/pull/1131
[#1134]: https://github.com/Nuand/bladeRF/pull/1134
[#1135]: https://github.com/Nuand/bladeRF/pull/1135
[#1137]: https://github.com/Nuand/bladeRF/issues/1137
[#1139]: https://github.com/Nuand/bladeRF/issues/1139
[#1143]: https://github.com/Nuand/bladeRF/issues/1143
[bladeRF-wiphy #16]: https://github.com/Nuand/bladeRF-wiphy/issues/16

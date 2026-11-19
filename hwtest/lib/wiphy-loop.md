# wiphy-loop: packet formats, assumptions, caveats

Version 2 (2026-11-19 afternoon): stream time-out 1 s instead of Nuand's driver's 10,000,000 ms, and
every `bladerf_sync_rx()` call with a 2.5 s time-out instead of polls of up to 1 s. Version 1 stopped
on every hardware run with "bladerf_sync_rx: An unexpected error occurred" and then hung until its
time-out: libbladeRF master reports a timed-out wait as BLADERF_ERR_UNEXPECTED (`thread.h`:
`posix_cond_timedwait()` returns -1, its callers expect ETIMEDOUT; upstream draft I19), and with
the long stream time-out the idle RX transfers could not complete when the device closed. Now an
idle RX stream ends with the stream's own BLADERF_ERR_TIMEOUT after 1 s and the next call starts it
again, so libbladeRF's own timed wait never expires first.

Source roots used for the citations below:

- **[B]** this bladeRF tree (Nuand master 35174c30; the cited files are unchanged on this branch).
- **[W]** Nuand/bladeRF-wiphy 8623814, `fpga/vhdl/`.
- **[D]** Nuand's Linux driver, github.com/Nuand/bladeRF-linux-mac80211 (main), read through a web fetch. `bladeRF-wiphy.h` came back verbatim. `bladeRF-linux-mac80211.c` came back as quoted excerpts, so I cite it by identifier rather than by line number.

## 1. What the program checks

`wiphy-loop -c COUNT [-m status|loop] [-l BYTES] [-r MBPS|all] [-g GAP_MS] [-w WAIT_MS] [-n] [-v] [-V] [-d DEV]`

- **`-m status` (default).** Sends unicast data frames to 02:00:00:00:00:01. Each one makes the FPGA transmit the frame, wait for an ACK, retry once one rate lower, and then send a 4-dword **TX report** of type 3 ("no ACK") that carries the frame's cookie. A frame passes when it gets exactly one type-3 report with bandwidth code 2 and the retry rate. This exercises host→FPGA packets, the wiphy TX state machine, DCF, encoder/modulator (`done` must come), the ACK timeout/retry, and FPGA→host packets. It does **not** exercise the demodulator.
- **`-m loop`.** Sends broadcast data frames (no ACK, so no report). Every frame must come back as a demodulated frame (type 1) that is byte-exact, has a valid FCS, and carries the right rate and bandwidth codes. On the stock design this mode is expected to fail; see section 4.1.
- **`-n`.** RX and TX are configured and enabled exactly as in a real run, nothing is sent for the same duration, and nothing may arrive.
- **Exit codes:** 0 PASS, 1 FAIL, 2 errors. The last line is always `RESULT: PASS|FAIL`.
- **Device access:** the device is only opened when `-c` is given. With no arguments the program prints its usage and exits 2.
- **Checks before running:**
  - Config GPIO bit 28 ("packet core present") must be set. Only the wlan revision sets it ([B] bladerf-wlan.vhd:476; adsb, foxhunt and hosted pass '0').
  - Host tuning mode is required ([B] bladerf2.c:2472-2479 refuses RFIC BIST in FPGA mode).
  - The sample rate must be exactly 20 Msps.
- **Frame content:** 24-byte data header, then "wplb", a run id, the frame index, and pseudo-random bytes. The cookie is `(run id & 0xfff) << 20 | index`. Reports or frames from another run therefore show up as "unknown".
- **Frequency:** 915 MHz. It has no effect in the digital loopback, and it keeps the (weak) emission out of the Wi-Fi bands. It is also Nuand's test_packet default ([B] libbladeRF_test/test_packet/src/main.c:27).
- **TX gain:** 0 dB.

## 2. TX packet: host → FPGA (BLADERF_FORMAT_PACKET_META)

**libbladeRF layer.** `bladerf_sync_tx(dev, buf, n_dwords, &meta, t)` prepends a 16-byte metadata header and sends one USB transfer per call ([B] sync.c:1402-1418). The header written by `metadata_set_packet(buf, 0, 0, num_samples, 0, 0)` ([B] metadata.h:145-167) contains:

- dword0 bits 15:0: the length in dwords of everything after the header.
- flags and core id: 0.
- timestamp: 0, meaning "now" ([B] fx3_gpif.vhd:565; fifo_reader.vhd:266 with meta_p_time = 0-1 = MAX).

**FPGA side of that layer.**

- fx3_gpif writes `len` dwords, plus one pad dword if `len` is odd, to the TX FIFO ([B] fx3_gpif.vhd:555-573).
- fifo_reader takes `packet_len = meta(15:0)` ([B] fifo_reader.vhd:257-264). It hands 32-bit dwords to wlan_top, low half of each 64-bit FIFO word first ([B] fifo_reader.vhd:589-622).
- `pkt_flags` and `pkt_core_id` are never assigned in fifo_reader, so the host's flags and core id do not reach the core.

**bladeRF-wiphy TX header.** This is the first 4 dwords of the payload: `struct bladeRF_wiphy_header_tx` in [D] bladeRF-wiphy.h, parsed in [W] wlan_top.vhd:714-739.

| bytes | field | meaning in wlan_top |
|---|---|---|
| 0-1 | rsvd | ignored |
| 2-3 | flags | latched as tx_packet_flags (:719) and never used (comment at :717: "no ack ... currently reserved") |
| 4-5 | modulation | 0..7 = 6, 9, 12, 18, 24, 36, 48, 54 Mbps; anything else becomes 6 (:722-732) |
| 6-7 | bandwidth | 0 = 5 MHz, 1 = 10 MHz, 2 = 20 MHz; anything else becomes 5 MHz (:734-739). The driver always sends 2. |
| 8-9 | len | MPDU length in **bytes, without FCS** (:716). Converted as signed 16-bit; the TX vector range is 1..4095 ([W] wlan_tx_p.vhd:30). |
| 10-11 | rsvd2 | ignored |
| 12-15 | cookie | echoed in the TX report (:720, :862, :871) |

**Payload.**

- The MPDU follows the header. Byte order inside each dword is little-endian: byte_index 0 is bits 7:0 ([W] wlan_top.vhd:597-602).
- The packet is zero-padded to whole dwords. Its length is `ceil((16 + len) / 4)` dwords, which is the driver's `bytes_to_dwords(frame_len)` with `frame_len = len + sizeof(header)` ([D] `bladerf_tx_frame`).
- The meta flags passed to `bladerf_sync_tx` are ignored in packet mode.

**FCS.** The FPGA appends it. The SIGNAL LENGTH is set to `len + 4` ([W] wlan_framer.vhd:185), and the CRC bytes are muxed in after the data ([W] wlan_framer.vhd:255-281, wlan_crc). The driver sends the mac80211 frame without an FCS.

**ACK decision.** wlan_top decides from the frame itself, using its first 10 bytes and only when the frame is at least 11 bytes long ([W] wlan_top.vhd:761-774). No ACK is expected if the frame-control type is "control" or addr1 is ff:ff:ff:ff:ff:ff. Otherwise:

- the ACK timeout is 2500 tx clocks (:806);
- there is one retry (tx_ack_attempt = 1, :771), one rate lower (:832-845), with the Retry bit set (:588-592);
- then a report of type 3 (:867-876), or type 2 if an ACK arrived (:858-866).

**Limits.**

- Keep len ≤ about 1500. The retry FIFO holds 1600 bytes ([W] wlan_top.vhd:885-902). The RX buffers hold 1600 bytes, and the RX side rejects LENGTH+2 > 1600 ([W] wlan_rx_framer.vhd:334).
- A transfer that is a whole multiple of the USB max packet size gets no short packet to end it ([B] libbladeRF.h:2236-2241). The program refuses lengths whose TX or RX transfer would be a multiple of 512 bytes, for example 474-480 and 985-992.

## 3. RX packet: FPGA → host

**libbladeRF layer.** One packet comes back per `bladerf_sync_rx`. `meta.actual_count` is the packet's length in dwords, and the payload is copied to the caller's buffer without any size check, so the program's buffer holds 65536 dwords ([B] sync.c:859-876). The header fields:

- dword0 bits 15:0: dword count ([B] fifo_writer.vhd:251-253, 325-334).
- flags and core id: 0 ([W] wlan_top.vhd:418-419).
- timestamp: the RX time.
- the low 16 bits of the last header dword: overwritten with the GPIF's underrun flags ([B] fx3_gpif.vhd:308-312).

**bladeRF-wiphy RX header.** 4 dwords; `struct bladeRF_wiphy_header_rx` in [D], built in [W] wlan_top.vhd:472-510.

- dword0 bits 15:0: **type**.
  - 1 = demodulated frame (:474).
  - 2 = TX report, ACK received (:859).
  - 3 = TX report, no ACK (:868).
- dword0 bits 23:16: bandwidth code (2 = 20 MHz).
- dword0 bits 31:24: modulation code (0..7).
- dword1, for frames: bits 15:0 are the **length in bytes, MPDU + 4 FCS bytes** (the SIGNAL LENGTH). wlan_top uses `actual_len = rx_vector.length - 2` (:473, :481), and the RX framer sets rx_vector.length to LENGTH + 2 for SERVICE ([W] wlan_rx_framer.vhd:331-332).
- dword1, for reports: the **cookie** (:862, :871).
- dword2 and dword3: 0. The driver's struct covers only 12 bytes, but it reads the frame from offset 16.

**Payload (frames only).**

- MPDU + FCS, little-endian within dwords, with the last dword zero-padded ([W] wlan_top.vhd:512-557).
- The driver passes `data + 16, len - 4` to mac80211, which strips the FCS ([D] `rx_frame`, tun/tap write).

**Status fields.**

- There is no "CRC ok" flag. Only frames whose FCS matched are committed; the rest are dropped ([W] wlan_rx_packet_buffer.vhd:239-243).
- There is no RSSI field.
- A TX report's modulation code is the rate of the last attempt, i.e. the retry. 6 Mbps stays 6.

**Configuration used.**

- 20 Msps on both channels: 802.11 OFDM 20 MHz, the driver's `TWENTY_MHZ`. The modem runs on a 40 MHz clock with a sample every other cycle ([W] wlan_rx `clock40m`; [B] tb/wlan_fx3_gpif_tb.vhd:116-117).
- PACKET_META on **both** RX and TX. It sets BLADERF_GPIO_PACKET | TIMESTAMP ([B] bladerf2/common.c:115-116), and that one GPIO bit switches packet mode for both directions ([B] bladerf-wlan.vhd:908-939).
- 4096-dword buffers: RX 64 buffers / 16 transfers, TX 16 / 8. The driver uses 4096 × 4096 dwords with 16 transfers.
- Stream timeout 1 s (version 1: 10,000,000 ms, like the driver). A packet-mode RX transfer completes only when a packet arrives, and any libusb timeout shuts the stream down ([B] libusb.c:1060-1063, 1088-1093); `bladerf_sync_rx()` then returns BLADERF_ERR_TIMEOUT, and the next call starts the stream again. After a stream time-out the program waits 5 ms, so the worker is idle before that call.
- Order: open → checks → frequency, rate, bandwidth, TX gain 0 → `set_loopback(RFIC_BIST)` → `sync_config` RX, then TX → enable RX → enable TX → stream → disable TX, then RX → `LB_NONE` → close. RX comes first because fifo_writer drops packets while RX is disabled.
- libbladeRF uses `bladerf2_rfic_init_params_fastagc_burst` for this image ([B] rfic_host.c:110-112). This is irrelevant in BIST.

## 4. What stops a self-loopback

### 4.1 Receiver blanked during its own transmission (hard blocker for `-m loop`)

- The blanking signal:
  - `rx_block <= wlan_tx_ota` ([W] wlan_top.vhd:298);
  - `wlan_tx_ota = tx_ota_ack and tx_ota_req` (:407);
  - `tx_ota_ack => '1'` ([B] bladerf-wlan.vhd:246).
- tx_ota_req is set when the TX FSM leaves IDLE (:624-639). That is 400 tx clocks, about 10 µs, before the first sample. It is cleared at `done` (:788-796), which the symbol shaper raises after the last sample ([W] wlan_symbol_shaper.vhd:214-218, wlan_tx.vhd:336).
- The receiver uses `acquired => acquired_packet and not rx_block` ([W] wlan_rx.vhd:485). Acquisition can only fire 192-254 samples after the burst starts ([W] wlan_acquisition.vhd:236), which is always inside the frame's own TX window.
- Nothing on the host side can change this: `config_reg` is a constant `x"00000000"` ([B] bladerf-wlan.vhd:216), and ACK frames the FPGA sends are blanked too.
- So **the stock design cannot demodulate its own frames.** `-m loop` will report 0 frames back and print a NOTE. `-m status` is the test that can pass.
- With packet_en set, the DAC only takes wlan_top's output ([B] bladerf-wlan.vhd:699-701), so injecting IQ is not an alternative.

### 4.2 ACK generation

- Unicast frames make the TX wait for an ACK and retry; nothing ACKs in a loopback.
- The RX would only ACK frames addressed to the hard-coded 70:b3:d5:7d:80:01 ([W] wlan_rx.vhd:758, wlan_rx_framer.vhd:386-401).
- Status mode uses unicast to 02:00:00:00:00:01 (report expected, no ACK generated). Loop mode uses broadcast (no ACK, no report).

### 4.3 DCF/CSMA

- TX waits for DIFS: rx_quiet (= not burst) for 360 + backoff clocks, about 9 µs ([W] wlan_dcf.vhd:145-160).
- In BIST the RX sees the DAC data. The DAC holds the last valid sample ([B] bladerf-wlan.vhd:699-701), and the shaper ends every frame with a zero sample ([W] wlan_symbol_shaper.vhd:214-218), so the channel reads as quiet between frames.
- wlan_csma's output is left open in wlan_rx, so it does not gate anything.

### 4.4 AGC level (matters only for `-m loop`)

- burst requires an IIR of I²+Q² above 150000, or a single sample above 300000 ([W] wlan_agc.vhd:113-117), on the 12-bit scale (±2048).
- TX output is bb/4 ([W] wlan_top.vhd:411) placed in the DAC's 12 bits ([B] bladerf-wlan.vhd:700). By my estimate the short preamble gives about 3×10⁵ average, so burst should trigger. **This estimate is not verified.**
- gain_ack and gain_nack are tied to '0' and '1' ([B] bladerf-wlan.vhd:240-241), so every gain request is refused. That is harmless in a digital loopback.

### 4.5 Rate and frequency

20 Msps is enforced. The frequency is irrelevant.

## 5. Upstream problem: TX dword accounting (predicted from the code, then seen on hardware)

**The code.**

- [B] fifo_reader.vhd:261 sets `dma_downcount <= packet_len - 1`. This came from commit **950189d2** (2024-03-06, "fix fifo_reader packet mode dma_downcount off-by-one error").
- That fix was checked against tb/fx3_gpif_packet_tb.vhd, whose consumer holds `packet_ready => '1'` (:354).
- wlan_top instead pulls one dword per single-cycle `fifo_read` pulse ([W] wlan_top.vhd:572, 695-708, 741-787).

**The predicted effect.** When the pulls are isolated, the reader emits only len-1 valid dwords and closes the packet (eop) on the second-to-last one ([B] fifo_reader.vhd:288-295, 607-611).

- The core still gets correct data, because the last dword is visible show-ahead.
- But that dword, plus the odd-length pad, stays in the TX FIFO.
- The **next** packet's header therefore starts with the stale dword.
- The TX FIFOs are cleared only by tx_reset, i.e. the FPGA system reset from the FX3 ([B] tx.vhd:118, 142; bladerf-wlan.vhd:825-834). I did not check whether open/close asserts that reset.

**Cycle model.** `wiphy-model/txmodel2.py` and `seqmodel.py` (next to this file) model fifo_reader together with wlan_top's pulls, using the driver's framing.

| HDL version | frames wrong (triples of back-to-back frames) |
|---|---|
| before 950189d2 (`dma_downcount = len`) | 0/228 |
| current | 152/228: every frame after the first of each triple |

In a 6-frame sequence on the current HDL, frame 0 is right. Frames 1 onward are parsed with len = 0 and a garbage rate, bandwidth and cookie. With len = 0 the core never reaches the 11-byte ACK check, so those frames produce no report.

**What a run would show if this is right.** In `-m status`, right after an FPGA load, only frame 0 gets its report; the others get none, or reports with odd cookies. The program then prints a NOTE pointing here. A second run without reloading the FPGA may fail even on frame 0.

**What it would mean.** The failure would come from upstream Nuand HDL that both our build and current Nuand master contain. The cleanest A/B test is a wlan image built from HDL before 950189d2, or with line 261 set back to `packet_len`. Nuand's prebuilt wlan bitstream may be such an image, but I did not check its build date.

**Hardware result (2026-11-19, bladeRF 2.0 micro xA9, `TESTING.md` section 17).** Exactly the predicted pattern, on Nuand's code built with Quartus 23.1.1 and on the Nios V build: right after a load 1 of 20 frames reported (frame 0), 1 of 64 at every rate; a second run without a reload 0 of 20. The same code built again with only line 261 set back to `packet_len`: 20 of 20, 20 of 20 again without a reload, and 64 of 64. A fix has to suit both kinds of reader: 950189d2 made `fx3_gpif_packet_tb` pass, whose consumer holds `packet_ready` high.

## 6. Other reasons for a FAIL that do not mean a broken FPGA

- **Leftovers from earlier activity.** Packets still queued in the TX FIFO, reports still pending, or a misaligned FIFO from an earlier run show up as "unknown cookie" or missing reports. Load the wlan image, or power-cycle, before the first run. A `-n` run right after an aborted run can transmit those leftovers.
- **Wrong setup.** A different FPGA image, or FPGA tuning mode, exits 2 before streaming.
- **Log noise.** libbladeRF logs every stream time-out ("Transfer timed out for RX buffer") at error level. The program lowers libbladeRF's log level to CRITICAL while streaming and until the device is closed, then restores INFO; `-V` keeps the normal level. The program's own `ERROR:` lines are unaffected.
- **FX3 lockups** (the FX3 problems seen on this board) appear as sync errors and exit 2.
- **A different wiphy build.** A report showing a different retry rate (or no retry) is flagged as "report fields unexpected"; the stock rule is one step lower.
- **No hang from garbage lengths.** A garbage TX header with a large length just makes one long transmission, at most about 5.5 ms at 6 Mbps. wlan_top has no data-available back-pressure, so it cannot hang.

## 7. Desk checks done (no hardware)

- Built on Linux (gcc 13) with -Wall -Wextra -Werror: no warnings.
- Built on macOS against a stub libbladeRF that emulates the wiphy replies (`wiphy-loop-stub.c`, next to this file; `STUB=<scenario>`). Every scenario behaved as intended (version 2 again: status, `-n`, silent, misalign):
  - PASS for status replies, for all rates, and for 1496-byte frames.
  - PASS for the `-n` control.
  - FAIL for stale reports, the misalignment pattern (with the NOTE), garbage cookies, silence, loop mode on the stock behaviour (with the NOTE), and corrupted copies.
  - Exit 2 for a non-wlan image, FPGA tuning mode, and an open failure.
  - Option errors never call libbladeRF.
- CRC-32 routine: "123456789" gives cbf43926.

## 8. How to run (when the board is free)

```
cc -O2 -Wall -Wextra -o wiphy-loop wiphy-loop.c -I<bladeRF>/host/libraries/libbladeRF/include -L<host build>/output -lbladeRF -lpthread
BLADERF_DEFAULT_TUNING_MODE=host wiphy-loop -c 20                  # load the wlan image first
BLADERF_DEFAULT_TUNING_MODE=host wiphy-loop -c 20 -n               # control
BLADERF_DEFAULT_TUNING_MODE=host wiphy-loop -c 64 -r all -l 1000
BLADERF_DEFAULT_TUNING_MODE=host wiphy-loop -c 10 -m loop          # expected FAIL on stock HDL
```

In this repository's harness, run it with the TX cap preloaded (`HWTEST_TX=1`, `require_tx_cap` from `common.sh`).

A healthy run gives 20 good reports, each about 1 ms after its send. The whole frame exchange takes under 1 ms on air. `-v` prints every packet.

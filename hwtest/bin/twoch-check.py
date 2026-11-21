#!/usr/bin/env python3
"""Two-channel RF checks of the bladeRF 2.0 micro against the USRP B200mini:
RX1 and RX2 at the same time, and TX1 and TX2 at the same time, through a
2-way splitter.

  twoch-check.py rx [--out results.jsonl] [--points N] [--save DIR]
  twoch-check.py tx [--out results.jsonl] [--points N] [--save DIR]
      with HWTEST_2CH_TX=sync only the sync runs (phase and timing)
  twoch-check.py --analyze DIR [--out results.jsonl]
      re-analyzes the captures that --save kept in DIR (transmits nothing)
  twoch-check.py --self-test
      checks the analysis against synthetic signals (no hardware)

Cabling (README.md, "Cabling for TX runs"; no antennas anywhere). With one
splitter the two setups are cabled one at a time; with two, both at once:
  rx: B200mini "TX/RX" -> 20 dB -> splitter -> bladeRF RX1 and RX2
  tx: bladeRF TX1 and TX2 -> splitter as a combiner -> 20 dB -> B200mini "RX2"
The splitter may be resistive (about 6 dB per path) or a Wilkinson (about
3 dB): every check compares the two channels with each other, or a channel
with itself in another configuration, never an absolute level. Power stays
capped as for rf-check.py, whose B200mini helpers this uses: the bladeRF's TX
gain by lib/tx-cap.c (preloaded with HWTEST_TX=1), the B200mini's by
rf-check.B200_TX_GAIN_MAX. Both subcommands transmit (rx: the B200mini, tx:
the bladeRF): only with HWTEST_TX=1.

rx, at each frequency point (TWOCH_POINTS, part of rf-check's POINTS): the
B200mini sends a CW tone F_OFF above the carrier while one bladeRF-cli run
receives on RX1 and RX2 together (channel=1,2), then again with RX2's gain
10 dB lower, then on RX1 alone and on RX2 alone, then four times after tuning
away and back. A second run receives a periodic chirp from the B200mini on
both channels. Measured: each channel's tone frequency, level, image and DC;
the two channels' level and phase difference, and the phase difference's
spread over blocks of each capture (a lost or extra sample on one channel
moves it by 360 * F_OFF / fs degrees); how each channel's level follows RX2's
gain step; the single-channel levels against the two-channel ones; the phase
difference after each retune; and the lag of RX2 behind RX1, as a whole
number of samples from the chirp's cross-correlation peak and as a fraction
from the slope of the cross-spectrum's phase.

tx, at the same points: bladeRF-cli sends a two-channel file (channel=1,2),
a tone fs/16 above the carrier on TX1 and one 3fs/32 above it on TX2; then the
same with TX2's gain 10 dB lower; then TX1 alone and TX2 alone (one channel,
its own tone). The B200mini receives with its LO 5fs/32 below the bladeRF's
carrier, so that the bladeRF's LO leakage does not fall on the B200mini's own
DC (whose correction would remove it) and every tone, image and LO leak of
either channel, and the B200mini's own image of each, has its own frequency:
in units of fs/32 from the B200mini's LO the tones are at 7 (TX1) and 8 (TX2),
their images at 3 and 2, the LO leakage at 5, the B200mini's images of all of
these at -7 to -2, its DC at 0. Measured: both tones' frequencies and levels,
each channel's image and LO leakage, how each tone follows TX2's gain step,
and the single-channel runs against the two-channel one.

tx, then the sync runs (TX phase and timing; with HWTEST_2CH_TX=sync only
these): three more bladeRF-cli runs of a two-channel file of repeated cycles
(SYNC_* below): a block in which TX1 and TX2 each send a marker tone, then
four blocks in which both send the same multitone, TX2's turned by 0, 90,
180 and 270 degrees. The B200mini, tuned to the carrier, receives the two
channels' sum; from each line's power in the four blocks follows the
cross-spectrum of TX1 and TX2 at the line, without any lock between the two
radios. Measured, per run: TX2's lag behind TX1 (whole samples from the
markers, the fraction from the slope of the cross-spectrum's phase, as in
rx), TX2's phase relative to TX1 at the carrier, and both per cycle (a lost
or extra sample on one channel moves the lag by one). The second and third
runs tune both TX channels elsewhere first (retune_away), so the phase over
the three runs is reported as rx's retunes are.

Every number goes into one JSON record per point and subcommand, with "pass"
and the reasons in "fails"; the thresholds are the named constants below,
each with its reason. The retune phases are reported, not judged: RX1 and
RX2 share the AD9361's RX synthesizer, but nothing in the AD9361
documentation at hand promises the same phase difference after the LO is
tuned away and back (if each channel's quadrature LO comes from a divider of
its own, that divider can start in either half of its cycle: a 180 degree
flip), so a change there is recorded ("class": stable, flips by 180 deg,
varies) for comparison between images. A capture that drops samples
(B200mini under- or overflow, a short bladeRF-cli file, a bladeRF-cli error)
is retried, up to TRIES times.

bladeRF-cli writes two-channel files interleaved per sample (RX1 I, RX1 Q,
RX2 I, RX2 Q, RX1 I, ...: libbladeRF.h, BLADERF_FORMAT_SC16_Q11; the slot
order lib/hwtest-stream.c uses too) and counts the samples of both channels
in n=; it reads two-channel TX files the same way. It exits 0 even when one
of its commands fails, so its error lines and every file's size are checked.
The bladeRF is driven only through bin/bladerf-cli-safe (volatile settings
and streaming; nothing is written to flash).
"""
import argparse
import glob
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("rfcheck", os.path.join(HERE, "rf-check.py"))
rf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rf)

# (center frequency, sample rate, RX tone offset) from rf-check's POINTS: the
# three RX gain tables (below 1.3 GHz, 1.3-4 GHz, above 4 GHz) and both RF
# front-end paths, at four different sample rates. Not rf-check's 100 MHz
# point (the bottom of the AD9361's range) nor its 30.72 Msps point: two
# channels of SC16 at 30.72 Msps are the most USB carries (245.76 MB/s), and
# bladeRF-cli's plain SC16 files cannot show a sample lost on the way.
TWOCH_POINTS = [rf.POINTS[i] for i in (1, 2, 3, 5, 6)]

RX_GAIN = rf.BLADERF_RX_GAIN    # dB, manual, both RX channels (rf-check's: 15)
TX_GAIN = rf.BLADERF_TX_GAIN    # dB overall, both TX channels (rf-check's: 30)
GAIN_STEP_DB = 10               # the step that tells the channels apart
RX_GAIN_LOW = RX_GAIN - GAIN_STEP_DB    # 5 dB: within every band's range (-16..60 or less)
TX_GAIN_LOW = TX_GAIN - GAIN_STEP_DB    # 20 dB
TX_GAIN_MAX = 60                # never more (lib/tx-cap.c caps it there too)
B200_TX_GAIN = rf.B200_TX_GAIN  # dB (50, about -30 dBm out)
# ... except at 5.8 GHz, where the B200mini's signal reaches the bladeRF
# about 30 dB weaker than at 433 MHz. With 50 dB there, the two channels'
# coherence over the chirp's band had a median of 0.68, and only 23 bins
# reached COH_MIN: too few for the fractional-lag fit (the xA9 through the
# TOJOIN splitter with 20 dB on each output, 2026-11-20). 10 dB more gives
# a median of about 0.96, if the bladeRF's own noise stays as it is.
B200_TX_GAIN_AT = {5800e6: 60}
B200_AMP = 0.5                  # B200mini tone and chirp amplitude (as rf-check)
TONE_AMP = 0.5                  # bladeRF TX tone amplitude per channel (as rf-check)
assert TX_GAIN <= TX_GAIN_MAX and TX_GAIN_LOW <= TX_GAIN_MAX
assert max([B200_TX_GAIN] + list(B200_TX_GAIN_AT.values())) <= rf.B200_TX_GAIN_MAX

# bladeRF RX captures. Every capture starts with SKIP_S of samples that are
# not analyzed: libbladeRF starts the stream when the channel is enabled and
# then (host tuning mode) tunes the LO again, and its first buffers can come
# out of order (README.md); 0.1 s is many times either.
SKIP_S = 0.1
RX_DUAL_S = 0.1         # analyzed: both channels, equal gains
RX_STEP_S = 0.05        # RX2 10 dB lower
RX_SINGLE_S = 0.05      # one channel
RX_RETUNE_S = 0.02      # after each retune
RX_BB_S = 0.05          # the chirp
RX_WAIT = "rx wait 10s"
# libbladeRF buffering for bladeRF-cli's captures: 16 MiB (68 ms at the
# highest rate here), not bladeRF-cli's 4 MiB, so that a host stall does not
# drop samples (a plain SC16 file cannot show that it did)
RX_STREAM = "buffers=128 samples=32768 xfers=32 timeout=3000"
RETUNE_NEAR_HZ = 20e6   # tune away by this much (the RF PLL relocks nearby) ...
RETUNES = 4             # ... alternating with a frequency an octave away

BLOCK = 8192            # samples per block for the phase-difference spread
CHIRP_N = 4096          # chirp period (samples); much longer than MAXLAG
CHIRP_SPAN = 0.8        # chirp sweeps -0.4 fs .. +0.4 fs
MAXLAG = 32             # lags searched for the cross-correlation peak
SIDE_EXCL = 3           # +-3 samples around the peak are its main lobe
SEG = 4096              # cross-spectrum segment (Hann window, half overlap)
FIT_BAND = 0.35         # phase slope fitted over |f| <= 0.35 fs (inside every filter)
COH_MIN = 0.8           # ... on bins where the two channels are this coherent
MIN_FIT_BINS = 64

# bladeRF TX runs (bladeRF-cli, one run per configuration): a 0.1 s file
# repeated for 10 s; the B200mini captures every 0.5 s from 2 s on until a
# tone is there (bladeRF-cli takes about 2.5-3 s to start), then once more
# for the measurement, so that the measured capture lies wholly inside the
# transmission (as rf-check.check_tx, which takes the first one)
TX_FILE_S = 0.1
TX_REPEAT = 100
TX_WAIT = "tx wait 20s"
TX_STREAM = RX_STREAM   # 2 x 20 Msps from a file: more buffering than bladeRF-cli's
TX_SEARCH_S = 8.0
B200_CAPTURE_S = 0.05
B200_WARMUP_S = 1.5     # B200mini transmitting before bladeRF-cli starts (as rf-check)
CLI_TIMEOUT_S = 120
TRIES = 3               # tries per point before giving up on a dropped-samples error

# TX phase and timing (the sync runs). A file of repeated cycles of five
# blocks of SYNC_NB samples: in block 0 TX1 sends a marker tone and TX2
# another; in blocks 1 to 4 both send the same multitone, TX2's turned by
# SYNC_PHASES. The B200mini is tuned to the carrier. In each block it sees
# the sum of the two channels, so at line k the block's power is
# |A + e^(j theta) B|^2 = |A|^2 + |B|^2 + 2 Re(e^(j theta) conj(A) B), with A
# and B the two channels' line as the B200mini receives it: the four blocks
# give conj(A) B = ((P0 - P180) + j (P270 - P90)) / 4, TX1's and TX2's
# cross-spectrum at the line, from powers alone. The two radios' frequency
# offset and phase noise turn A and B alike within a block (one LO on each
# side: the AD9361's TX LO serves TX1 and TX2) and leave every power as it
# is, so nothing has to be locked to the bladeRF. The cross-spectrum's phase
# against frequency is then fitted as for rx (align): its slope is TX2's lag
# behind TX1, its intercept TX2's phase relative to TX1 at the carrier.
SYNC_NB = 16384         # samples per block
SYNC_SPACING = 400      # bins of SYNC_NB between lines (0.024 fs)
SYNC_LINE_OFFSET = 133  # lines at SYNC_SPACING * m + 133 bins: never on a line's image (-k), DC or a marker
SYNC_BAND = 0.35        # lines within +-0.35 fs (FIT_BAND): 29 lines
SYNC_MARKER1 = 3133     # TX1's marker tone, bins of SYNC_NB (0.19 fs), half way between two lines
SYNC_MARKER2 = -2867    # TX2's (-0.18 fs), also between two lines and off both markers' images
SYNC_PHASES = (0, 90, 180, 270)     # TX2's turn in blocks 1 to 4, degrees
SYNC_AMP = 0.5          # each channel's peak amplitude, multitone and marker (as TONE_AMP)
SYNC_GUARD = 2048       # samples left out at both ends of a block (filters, block edges)
SYNC_CYCLES = 8         # whole cycles in each capture (and a part of one more)
SYNC_MARKER_GAP = 1024  # samples of silence before and after the markers in block 0 (see sync_cycle)
SYNC_ENV_LEN = 256      # samples: the markers' envelope filter (Hann; the nearest lines 40 dB down)
SYNC_PERIOD_TOL = 64    # samples: one marker block to the next, 5 * SYNC_NB (block edges +-10 at 5.8 GHz)
# (capture, which of retune_away()'s frequencies the bladeRF tunes to first
# or None): the sync runs, each a new bladeRF-cli run and stream
SYNC_RUNS = [("sync0", None), ("sync1", 0), ("sync2", 1)]

# --- Pass/fail thresholds -------------------------------------------------
# A tone is there when it is within 10 ppm (+100 Hz) of where it should be
# and 30 dB above the median of the spectrum: rf-check.tone_found's rule. The
# two radios have independent references, each within a few ppm.
FREQ_PPM = 10e-6
FREQ_ABS_HZ = 100
TONE_SNR_DB = 30
# The two channels' levels at equal gains, RX1 vs RX2 and TX1 vs TX2. Their
# front ends are alike, but the splitter's two paths differ by a few tenths
# of a dB, a resistive splitter lets each port's mismatch reach the other
# (+-1.3 dB per port for a 10 dB return loss), and the board's two RF paths
# can differ by a dB or two at 5.8 GHz. A channel that is a whole step off
# (6 dB and more) fails.
LEVEL_DIFF_MAX_DB = 4.0
# The gain step: the stepped channel must move by -10 dB within +-2.5 dB (the
# AD9361's RX gain table is not exactly 1 dB per index where its LNA, mixer
# and filter gains change over; its TX attenuator is far more exact), and the
# other channel must stay within +-2 dB (the stepped port's input or output
# match changes a little with its gain, which a resistive splitter passes
# on). A gain set on the wrong channel, or on both, moves the other one by
# 10 dB, and a step that does nothing leaves the stepped one where it was.
GAIN_STEP_TOL_DB = 2.5
GAIN_OTHER_MAX_DB = 2.0
# A channel alone (one-channel stream) against the same channel in the
# two-channel stream. With one channel enabled the other port's RF switch is
# off and reflects: through a Wilkinson that changes nothing, but through a
# resistive splitter it moves the active port's level by -6 dB (short) to
# +3.5 dB (open), depending on the reflection's phase. A channel that does
# not receive or transmit alone, or streams the other (disabled, so switched
# off or muted) channel's samples, or uses the wrong band path, is 20 dB or
# more off.
SINGLE_DUAL_MAX_DB = 7.0
# TX alone: the other channel's tone (not in the file) must be at least
# 30 dB below the active one; with nothing sent it is at the noise floor,
# 70-80 dB down.
ABSENT_MAX_DBC = -30.0
# Phase difference between the channels over the blocks of one capture (max
# minus min). Both channels share the LO and the sample clock, so it only
# moves by noise, well under a degree at these levels; one lost or extra
# sample on one channel moves it by 360 * F_OFF / fs: 27-45 degrees here.
PHASE_SPREAD_MAX_DEG = 5.0
# RX1's and RX2's tone frequencies (and TX1's and TX2's tone errors) must
# agree within one FFT bin: they share the LO and the sample clock.
FREQ_AGREE_BINS = 1.0
# Sample alignment: the cross-correlation peak must be at lag 0, at least
# 10 dB above any other lag outside its main lobe (a clean chirp gives 18 dB
# or more), and the fractional lag within a quarter sample. The channels share
# the ADC clock and identical digital filters, so a healthy board's lag is the
# small difference of its analog paths (nanoseconds: 0.1 sample at 20 Msps at
# most); a pipeline or interleaving fault moves it by whole samples.
LAG_SAMPLES = 0
CORR_PEAK_MIN_DB = 10.0
FRAC_LAG_MAX = 0.25
# TX (the sync runs): the same lag and fraction from the multitone's lines,
# and per cycle (5 blocks) the phase at the carrier within PHASE_SPREAD_MAX_DEG
# and the lag within FRAC_LAG_MAX of each other (a sample lost or added on
# TX2 moves the lag of the cycles after it by one). The lines alone repeat
# every SYNC_NB / SYNC_SPACING = 41 samples, so the markers' lag (their
# blocks' centres, a few samples off at 5.8 GHz) picks the whole samples
# first, and the lines' lag must agree with it within SYNC_MARKER_AGREE
# samples (a marker block found in the wrong place shows there). A line is
# fitted when its cross-spectrum stands SYNC_LINE_SNR_DB
# above its scatter over the cycles (a phase error of 18 degrees or less),
# and at least SYNC_MIN_LINES of the 29 must; at least SYNC_MIN_CYCLES whole
# cycles, or the capture is retried (something lost or added samples).
SYNC_MARKER_AGREE = 15.0
SYNC_LINE_SNR_DB = 10.0
SYNC_MIN_LINES = 15
SYNC_MIN_CYCLES = 4
# Report only: retune phases within 5 degrees of the first count as stable.
RETUNE_STABLE_DEG = 5.0

SAVE_DIR = None         # --save


# --- Two-channel SC16 Q11 files -------------------------------------------

def interleave_sc16(chans):
    """bladeRF-cli/libbladeRF stream layout of one or two channels: int16 I,
    Q of channel 1, then I, Q of channel 2, for every sample (each channel
    given as rf-check.to_sc16q11() output, I and Q interleaved)."""
    pairs = [np.asarray(c, dtype=np.int16).reshape(-1, 2) for c in chans]
    n = min(len(p) for p in pairs)
    return np.stack([p[:n] for p in pairs], axis=1).reshape(-1)


def deinterleave_sc16(raw, nch):
    """The channels of an interleaved SC16 Q11 stream, as complex64 scaled as
    rf-check.from_sc16q11() (full scale 2048)."""
    raw = np.asarray(raw)
    raw = raw[:len(raw) // (2 * nch) * (2 * nch)]
    iq = raw.astype(np.float32) / 2048.0
    x = (iq[0::2] + 1j * iq[1::2]).astype(np.complex64)
    return [x[k::nch] for k in range(nch)]


def load_bladerf(path, nch, n_per_ch, skip):
    """One bladeRF-cli capture of n_per_ch samples per channel: checks its
    size (bladeRF-cli exits 0 when a receive stops early) and returns the
    channels without the first skip samples."""
    want = n_per_ch * nch * 4
    got = os.path.getsize(path) if os.path.exists(path) else 0
    if got != want:
        raise RuntimeError("%s: %d of %d samples per channel (bladeRF-cli stopped early)"
                           % (os.path.basename(path), got // (4 * nch), n_per_ch))
    raw = np.fromfile(path, dtype="<i2", offset=skip * nch * 4)
    return deinterleave_sc16(raw, nch)


# --- Spectra and tones ----------------------------------------------------

def freq_tol(fc):
    return FREQ_PPM * fc + FREQ_ABS_HZ


def spectrum(x, fs):
    """Blackman-windowed spectrum of the longest power-of-two start of x,
    scaled like rf-check's analyze() (a tone of amplitude A peaks at A), and
    the window's equivalent noise bandwidth in bins."""
    n = 1 << int(np.floor(np.log2(len(x))))
    w = np.blackman(n)
    s = np.fft.fftshift(np.fft.fft(np.asarray(x[:n], dtype=np.complex128) * w)) / np.sum(w)
    f = np.fft.fftshift(np.fft.fftfreq(n, 1.0 / fs))
    return f, s, float(n * np.sum(w ** 2) / np.sum(w) ** 2)


def tone_at(f, s, enbw, fe, tol):
    """The strongest bin within tol (plus 4 bins) of fe: its frequency, refined
    by a parabola through the dB values of its neighbours, and its level in
    dBFS, from the power within +-4 bins (the Blackman main lobe) divided by
    the noise bandwidth: the same whether the tone sits on a bin or between
    two."""
    df = f[1] - f[0]
    p = np.abs(s) ** 2
    lo = max(int(np.searchsorted(f, fe - tol - 4 * df)), 5)
    hi = min(int(np.searchsorted(f, fe + tol + 4 * df, side="right")), len(f) - 5)
    if hi <= lo:
        raise ValueError("%.0f Hz is outside the spectrum" % fe)
    k = lo + int(np.argmax(p[lo:hi]))
    a, b, c = 10 * np.log10(p[k - 1:k + 2] + 1e-30)
    den = a - 2 * b + c
    d = float(np.clip(0.5 * (a - c) / den, -0.5, 0.5)) if den < 0 else 0.0
    return float(f[k] + d * df), float(10 * np.log10(np.sum(p[k - 4:k + 5]) / enbw + 1e-30))


def channel_tone(x, fs, fe, fc):
    """One channel's tone, expected fe from the LO: frequency error, level,
    image and DC (relative to the tone), median floor, and whether it is
    there (rf-check's rule)."""
    f, s, enbw = spectrum(x, fs)
    df = float(f[1] - f[0])
    floor = float(10 * np.log10(np.median(np.abs(s) ** 2) + 1e-30))
    fq, lev = tone_at(f, s, enbw, fe, freq_tol(fc))
    _, img = tone_at(f, s, enbw, -fq, 2 * df)
    _, dc = tone_at(f, s, enbw, 0.0, 2 * df)
    err = fq - fe
    return {"freq_err_hz": err, "level_dbfs": lev, "image_dbc": img - lev, "dc_dbc": dc - lev,
            "floor_dbfs": floor, "snr_db": lev - floor, "bin_hz": df,
            "present": bool(lev >= floor + TONE_SNR_DB and abs(err) <= freq_tol(fc))}


def wrap_deg(d):
    return (np.asarray(d) + 180.0) % 360.0 - 180.0


def tone_blocks(x1, x2, fs, ftone, block=BLOCK):
    """Phase of channel 2 relative to channel 1 at the tone, over the whole
    capture and per block: each channel mixed down by ftone and averaged over
    the block (which leaves out DC and the image), then z2 * conj(z1). A
    frequency error moves both channels alike and drops out."""
    nb = min(len(x1), len(x2)) // block
    if nb < 2:
        raise ValueError("capture too short for blocks")
    lo = np.exp(-2j * np.pi * ftone * np.arange(nb * block) / fs)
    z1 = (x1[:nb * block] * lo).reshape(nb, block).mean(axis=1)
    z2 = (x2[:nb * block] * lo).reshape(nb, block).mean(axis=1)
    d = z2 * np.conj(z1)
    tot = np.sum(d)
    dev = np.degrees(np.angle(d * np.conj(tot)))
    ratio = 20 * np.log10(np.abs(z2) / (np.abs(z1) + 1e-30) + 1e-30)
    return {"phase_diff_deg": float(np.degrees(np.angle(tot))),
            "phase_spread_deg": float(dev.max() - dev.min()),
            "phase_std_deg": float(dev.std()), "blocks": int(nb),
            "level_diff_spread_db": float(ratio.max() - ratio.min())}


def chirp(n, fs, span=CHIRP_SPAN, amp=B200_AMP):
    """A linear chirp from -span/2 to +span/2 of fs in n samples, whose phase
    is back at 0 after n samples, so that it repeats without a phase step."""
    k = np.arange(n, dtype=np.float64)
    f0, rate = -0.5 * span * fs, span * fs / n
    return (amp * np.exp(2j * np.pi * (f0 * k + 0.5 * rate * k * k) / fs)).astype(np.complex64)


def align(x1, x2, fs):
    """Lag of channel 2 behind channel 1 (positive: channel 2 later), from a
    broadband signal on both: the whole number of samples at the peak of the
    cross-correlation, and the fraction two ways, from the slope of the
    cross-spectrum's phase (weighted fit over the coherent bins of |f| <=
    FIT_BAND fs) and from the cross-correlation interpolated 16 times. The
    fit's intercept is channel 2's phase relative to channel 1 at the
    carrier."""
    n = min(len(x1), len(x2))
    a1 = np.asarray(x1[:n], dtype=np.complex128)
    a2 = np.asarray(x2[:n], dtype=np.complex128)
    nfft = 1 << int(np.ceil(np.log2(2 * n)))
    r = np.fft.ifft(np.fft.fft(a2, nfft) * np.conj(np.fft.fft(a1, nfft)))
    lags = np.arange(-MAXLAG, MAXLAG + 1)
    mag = np.abs(r[lags % nfft])
    i = int(np.argmax(mag))
    lag = int(lags[i])
    side = mag[np.abs(lags - lag) > SIDE_EXCL]
    out = {"lag_samples": lag,
           "peak_ratio_db": float(20 * np.log10(mag[i] / (side.max() + 1e-30)))}
    # cross-spectrum (Welch), with the whole-sample lag taken out
    hop = SEG // 2
    nseg = (n - SEG) // hop + 1
    if nseg < 4:
        raise ValueError("capture too short for the cross-spectrum")
    idx = np.arange(SEG)[None, :] + hop * np.arange(nseg)[:, None]
    w = np.hanning(SEG)
    f1 = np.fft.fft(a1[idx] * w, axis=1)
    f2 = np.fft.fft(a2[idx] * w, axis=1)
    s12 = np.mean(f2 * np.conj(f1), axis=0)
    coh = np.abs(s12) ** 2 / (np.mean(np.abs(f1) ** 2, axis=0) * np.mean(np.abs(f2) ** 2, axis=0) + 1e-30)
    f = np.fft.fftfreq(SEG, 1.0 / fs)
    s12 = s12 * np.exp(2j * np.pi * f * lag / fs)
    sel = (np.abs(f) <= FIT_BAND * fs) & (np.abs(f) >= 3 * fs / SEG) & (coh >= COH_MIN)
    out["fit_bins"] = int(np.sum(sel))
    out["coherence_median"] = float(np.median(coh[np.abs(f) <= FIT_BAND * fs]))
    out["lag_est_samples"] = None
    if out["fit_bins"] >= MIN_FIT_BINS:
        order = np.argsort(f[sel])
        fk = f[sel][order]
        ph = np.unwrap(np.angle(s12[sel][order]))
        wk = np.minimum(coh[sel][order] / (1 - coh[sel][order] + 1e-12), 1e6)
        sw = np.sqrt(wk)
        (c0, c1), _, _, _ = np.linalg.lstsq(np.vstack([sw, sw * fk]).T, sw * ph, rcond=None)
        res = ph - (c0 + c1 * fk)
        out["lag_est_samples"] = float(lag - c1 * fs / (2 * np.pi))
        out["phase_at_fc_deg"] = float(wrap_deg(np.degrees(c0)))
        out["fit_rms_deg"] = float(np.degrees(np.sqrt(np.sum(wk * res ** 2) / np.sum(wk))))
    # cross-correlation interpolated 16 times around the remaining lag
    up = 16
    sp = np.zeros(SEG * up, dtype=np.complex128)
    sp[:SEG // 2] = s12[:SEG // 2]
    sp[-SEG // 2:] = s12[-SEG // 2:]
    ru = np.abs(np.fft.ifft(sp))
    near = np.arange(-2 * up, 2 * up + 1)
    j = int(np.argmax(ru[near % len(ru)]))
    if 0 < j < len(near) - 1:
        a, b, c = ru[near[j - 1:j + 2] % len(ru)]
        den = a - 2 * b + c
        d = 0.5 * (a - c) / den if den < 0 else 0.0
    else:
        d = 0.0
    out["lag_xcorr_samples"] = float(lag + (near[j] + d) / up)
    return out


def retune_summary(phases):
    """How the phase difference moved over retunes: the largest deviation
    from the first, the spread around the circular mean, and a class."""
    p = np.asarray(phases, dtype=np.float64)
    dev0 = wrap_deg(p - p[0])
    z = np.exp(1j * np.radians(p))
    dm = np.degrees(np.angle(z * np.conj(np.sum(z))))
    near180 = np.minimum(np.abs(dev0), 180 - np.abs(dev0))
    if np.all(np.abs(dev0) <= RETUNE_STABLE_DEG):
        cls = "stable"
    elif np.all(near180 <= RETUNE_STABLE_DEG):
        cls = "flips by 180 deg"
    else:
        cls = "varies"
    return {"dev_from_first_deg": [float(v) for v in dev0],
            "max_dev_deg": float(np.max(np.abs(dev0))),
            "spread_deg": float(dm.max() - dm.min()), "class": cls}


def retune_away(fc):
    """Where each retune goes before it comes back: alternately 20 MHz up
    (down near 6 GHz) and an octave away (another LO divider setting, and
    across 3 GHz another front-end path)."""
    near = fc + RETUNE_NEAR_HZ if fc + RETUNE_NEAR_HZ <= 6e9 else fc - RETUNE_NEAR_HZ
    far = fc * 2 if fc * 2 <= 5.9e9 else fc / 2
    return [near if i % 2 == 0 else far for i in range(RETUNES)]


def tx_plan(fs):
    """TX1's tone, TX2's tone and how far below the bladeRF's carrier the
    B200mini tunes: 2, 3 and 5 units of fs/32. From the B200mini's LO the
    bladeRF's components are then at TX1 7, TX2 8, their images 3 and 2, LO
    leakage 5 (and second harmonics 9 and 11, third-order images -1 and -4,
    intermodulation 6 and 9), the B200mini's own images of the five at -7 to
    -2 and its DC at 0: nothing that is measured shares a frequency with
    anything else, all within +-fs/4 of the B200mini's LO, and both tones
    complete whole cycles in any file of a multiple of 32 samples."""
    u = fs / 32.0
    return 2 * u, 3 * u, 5 * u


def tx_positions(f1, f2, delta):
    return {"t1": f1 + delta, "t2": f2 + delta, "i1": delta - f1, "i2": delta - f2, "lo": delta}


# --- TX phase and timing (the sync runs) ----------------------------------

def sync_lines():
    """The multitone's lines, in bins of SYNC_NB from the carrier, ascending."""
    kmax = int(SYNC_BAND * SYNC_NB)
    m = np.arange(-(kmax // SYNC_SPACING) - 1, kmax // SYNC_SPACING + 2)
    k = SYNC_SPACING * m + SYNC_LINE_OFFSET
    return k[np.abs(k) <= kmax]


def sync_cycle():
    """One cycle of the sync file, TX1 and TX2 (5 * SYNC_NB samples each):
    block 0 the two markers, with SYNC_MARKER_GAP samples of silence on both
    sides (the multitone cut off at a block's edge spreads over the markers'
    frequencies: next to a marker it moved the marker's edges by up to 5
    samples, depending on TX2's phase); blocks 1-4 the multitone, TX2's
    turned by SYNC_PHASES. Every line completes whole cycles in a block, so
    TX1's multitone runs on without a step from block 1 to block 4. Newman's
    phases keep its peak low (crest factor 1.4)."""
    n = np.arange(SYNC_NB)
    k = sync_lines()
    ph = np.pi * np.arange(len(k)) ** 2 / len(k)
    m = np.exp(2j * np.pi * np.outer(n, k) / SYNC_NB + 1j * ph).sum(axis=1)
    m *= SYNC_AMP / np.max(np.abs(m))
    gate = (n >= SYNC_MARKER_GAP) & (n < SYNC_NB - SYNC_MARKER_GAP)
    mk = lambda kb: SYNC_AMP * np.exp(2j * np.pi * kb * n / SYNC_NB) * gate
    x1 = np.concatenate([mk(SYNC_MARKER1)] + [m] * len(SYNC_PHASES))
    x2 = np.concatenate([mk(SYNC_MARKER2)] + [m * np.exp(1j * np.radians(t)) for t in SYNC_PHASES])
    return x1, x2


def sync_capture_len():
    """B200mini samples per sync capture: SYNC_CYCLES whole cycles wherever it
    starts, and the marker block that ends the last one."""
    return int((SYNC_CYCLES + 1.25) * (1 + len(SYNC_PHASES)) * SYNC_NB)


def sync_markers(y, fs, fc):
    """TX1's and TX2's markers in a sync capture: each one's frequency and
    level (rf-check's rule for "there"), and the centre of every block in
    which it is on, where its envelope (y mixed down to the marker and
    averaged over SYNC_ENV_LEN samples, Hann-weighted) crosses half its
    level, interpolated between samples. Blocks cut off by the capture's
    ends, or not as long as a marker (+-SYNC_NB / 8), are left out."""
    f, s, enbw = spectrum(y, fs)
    floor = float(10 * np.log10(np.median(np.abs(s) ** 2) + 1e-30))
    n = np.arange(len(y))
    la = SYNC_ENV_LEN
    h = np.hanning(la + 2)[1:-1]
    nf = 1 << int(np.ceil(np.log2(len(y) + la)))
    hf = np.fft.fft(h / np.sum(h), nf)
    out = {}
    for name, kb in (("tx1", SYNC_MARKER1), ("tx2", SYNC_MARKER2)):
        fe = kb * fs / SYNC_NB
        fq, lev = tone_at(f, s, enbw, fe, freq_tol(fc))
        z = np.fft.ifft(np.fft.fft(y * np.exp(-2j * np.pi * fq * n / fs), nf) * hf)
        e = np.abs(z[la // 2:la // 2 + len(y)])
        half = 0.5 * float(np.percentile(e, 99))      # the marker is on in 1/5 of the samples
        on = e > half
        edges = np.flatnonzero(on[1:] != on[:-1])     # between samples i and i + 1
        rises, falls = edges[~on[edges]], edges[on[edges]]
        centers = []
        for r in rises:
            j = np.searchsorted(falls, r)
            if j == len(falls):
                break
            fl = falls[j]
            if abs((fl - r) - (SYNC_NB - 2 * SYNC_MARKER_GAP)) > SYNC_NB // 8 or r < la or fl > len(y) - la:
                continue
            xr = r + (half - e[r]) / (e[r + 1] - e[r])
            xf = fl + (e[fl] - half) / (e[fl] - e[fl + 1])
            centers.append(float(0.5 * (xr + xf)))
        inside = np.zeros(len(y), dtype=bool)
        for c in centers:
            inside[int(c - SYNC_NB / 4):int(c + SYNC_NB / 4)] = True
        contrast = (20 * np.log10(np.median(e[inside]) / (np.median(e[~inside]) + 1e-30) + 1e-30)
                    if centers else None)
        out[name] = {"freq_err_hz": fq - fe, "level_dbfs": lev, "snr_db": lev - floor,
                     "present": bool(lev >= floor + TONE_SNR_DB and abs(fq - fe) <= freq_tol(fc)),
                     "blocks": len(centers), "contrast_db": contrast, "centers": centers}
    return out


def _wfit(x, y, w):
    """Weighted straight line y = c0 + c1 x: (c0, c1)."""
    sw = np.sqrt(w)
    (c0, c1), _, _, _ = np.linalg.lstsq(np.vstack([sw, sw * x]).T, sw * y, rcond=None)
    return float(c0), float(c1)


def analyze_sync(y, meta):
    """TX2's lag behind TX1 and phase relative to TX1 from one sync capture
    (the B200mini tuned to the carrier): the cycles, from TX1's marker
    blocks; in each whole cycle each line's power in blocks 1-4, so its
    cross-spectrum conj(A) B; over the cycles, each line's mean and scatter;
    the lines' phase against frequency, the markers' lag taken out first,
    fitted as in align() (lag_est_samples, phase_at_fc_deg); and the same per
    cycle. Raises RuntimeError when the capture holds no TX1 marker or too
    few whole cycles (retried as a capture problem)."""
    fs, fc = meta["fs_b200"], meta["fc"]
    mk = sync_markers(y, fs, fc)
    m1, m2 = mk["tx1"], mk["tx2"]
    if not m1["present"] or m1["blocks"] < 2:
        raise RuntimeError("sync: TX1's marker is not there (%.1f dB above the floor, %d blocks)"
                           % (m1["snr_db"], m1["blocks"]))
    period = (1 + len(SYNC_PHASES)) * SYNC_NB
    c1 = np.asarray(m1["centers"])
    gaps = np.diff(c1)
    whole = np.abs(gaps - period) <= SYNC_PERIOD_TOL
    starts = c1[:-1][whole] - SYNC_NB / 2
    out = {"freq_err_hz": m1["freq_err_hz"], "markers": {k: {kk: v[kk] for kk in v if kk != "centers"}
                                                          for k, v in mk.items()},
           "cycles": int(len(starts)), "irregular_cycles": int(np.sum(~whole))}
    if len(starts) < SYNC_MIN_CYCLES:
        raise RuntimeError("sync: %d whole cycles (%d irregular; limit %d): samples lost or added"
                           % (len(starts), out["irregular_cycles"], SYNC_MIN_CYCLES))
    lags = []
    for c in c1:
        d = [c2 - c for c2 in m2["centers"] if abs(c2 - c) < SYNC_NB / 2]
        if d:
            lags.append(d[0])
    out["marker_lag_samples"] = float(np.median(lags)) if (lags and m2["present"]) else None
    if out["marker_lag_samples"] is not None:
        out["marker_lag_spread_samples"] = float(np.max(lags) - np.min(lags))

    # every line's power in blocks 1-4 of every whole cycle: a single-bin
    # DFT (Blackman) at the line plus the radios' offset (TX1's marker's)
    k = sync_lines()
    fk = k * fs / SYNC_NB
    na = SYNC_NB - 2 * SYNC_GUARD
    w = np.blackman(na)
    dft = np.exp(-2j * np.pi * np.outer(fk + m1["freq_err_hz"], np.arange(na)) / fs) * (w / np.sum(w))
    P = np.empty((len(starts), len(SYNC_PHASES), len(k)))
    for c, st in enumerate(starts):
        for b in range(len(SYNC_PHASES)):
            s0 = int(round(st)) + (b + 1) * SYNC_NB + SYNC_GUARD
            P[c, b] = np.abs(dft @ y[s0:s0 + na]) ** 2
    X = ((P[:, 0] - P[:, 2]) + 1j * (P[:, 3] - P[:, 1])) / 4    # conj(A) B, cycle x line
    nc = len(starts)
    xm = X.mean(axis=0)
    sem = np.sqrt(np.sum(np.abs(X - xm) ** 2, axis=0) / (nc * (nc - 1)))
    snr = np.abs(xm) / (sem + 1e-30)
    sel = snr >= 10 ** (SYNC_LINE_SNR_DB / 20)
    out.update(lines=int(len(k)), fit_lines=int(np.sum(sel)),
               line_snr_median_db=float(20 * np.log10(np.median(snr) + 1e-30)),
               lag_samples=None, lag_est_samples=None, phase_at_fc_deg=None)
    if out["fit_lines"] < SYNC_MIN_LINES:
        return out
    # the markers' lag first (whole samples), then the slope of what is left
    # (normalized frequency u = f / fs: the slope is -2 pi times the lag)
    l0 = int(round(out["marker_lag_samples"])) if out["marker_lag_samples"] is not None else 0
    u = fk / fs
    rot = np.exp(2j * np.pi * u * l0)
    wk = np.minimum(snr ** 2, 1e6)[sel]
    ph = np.unwrap(np.angle(xm[sel] * rot[sel]))
    c0, cs = _wfit(u[sel], ph, wk)
    res = ph - (c0 + cs * u[sel])
    lag = l0 - cs / (2 * np.pi)
    out.update(lag_samples=int(np.round(lag)), lag_est_samples=float(lag),
               phase_at_fc_deg=float(wrap_deg(np.degrees(c0))),
               fit_rms_deg=float(np.degrees(np.sqrt(np.sum(wk * res ** 2) / np.sum(wk)))))
    # per cycle: the same fit to what each cycle adds to the mean's
    cph, clag = [], []
    for c in range(nc):
        r = np.angle(X[c, sel] * rot[sel] * np.exp(-1j * (c0 + cs * u[sel])))
        d0, ds = _wfit(u[sel], r, wk)
        cph.append(float(wrap_deg(np.degrees(c0 + d0))))
        clag.append(float(lag - ds / (2 * np.pi)))
    dev = wrap_deg(np.asarray(cph) - out["phase_at_fc_deg"])
    out.update(cycle_phases_deg=cph, cycle_lags=clag,
               cycle_phase_spread_deg=float(dev.max() - dev.min()),
               cycle_lag_spread_samples=float(max(clag) - min(clag)))
    return out


# --- Analysis (shared by the hardware runs and --analyze) -----------------

def analyze_rx(meta, caps):
    """The rx record from the captures (each channel without its first
    SKIP_S): caps[name] is a list with one array per channel streamed."""
    fc, fs, f_off = meta["fc"], meta["fs"], meta["f_off"]
    m = {"test": "twoch_rx", "fc": fc, "fs": fs, "f_off": f_off,
         "gain_db": meta["gain"], "gain_low_db": meta["gain_low"]}
    fails = []
    x1, x2 = caps["dual"]
    c1, c2 = channel_tone(x1, fs, f_off, fc), channel_tone(x2, fs, f_off, fc)
    if not c1["present"] and not c2["present"]:
        # the B200mini's tone is not there at all: retried as a capture problem
        raise RuntimeError("no tone on RX1 or RX2 (%.1f / %.1f dB above the floor)"
                           % (c1["snr_db"], c2["snr_db"]))
    m["rx1"], m["rx2"] = c1, c2
    for name, c in (("RX1", c1), ("RX2", c2)):
        if not c["present"]:
            fails.append("%s: no tone (%.1f dBFS, %.1f dB above the floor, %+.0f Hz off)"
                         % (name, c["level_dbfs"], c["snr_db"], c["freq_err_hz"]))
    both = c1["present"] and c2["present"]
    ftone = f_off + (0.5 * (c1["freq_err_hz"] + c2["freq_err_hz"]) if both else
                     (c1 if c1["present"] else c2)["freq_err_hz"])
    if both:
        m["level_diff_db"] = c2["level_dbfs"] - c1["level_dbfs"]
        if abs(m["level_diff_db"]) > LEVEL_DIFF_MAX_DB:
            fails.append("RX2 %+.1f dB from RX1 at equal gains (limit %.1f)"
                         % (m["level_diff_db"], LEVEL_DIFF_MAX_DB))
        m["freq_diff_hz"] = c2["freq_err_hz"] - c1["freq_err_hz"]
        if abs(m["freq_diff_hz"]) > FREQ_AGREE_BINS * c1["bin_hz"]:
            fails.append("RX2's tone %+.1f Hz from RX1's (limit %.1f Hz, one bin)"
                         % (m["freq_diff_hz"], FREQ_AGREE_BINS * c1["bin_hz"]))
        m.update(tone_blocks(x1, x2, fs, ftone))
        if m["phase_spread_deg"] > PHASE_SPREAD_MAX_DEG:
            fails.append("phase difference spread %.1f deg over %d blocks (limit %.1f)"
                         % (m["phase_spread_deg"], m["blocks"], PHASE_SPREAD_MAX_DEG))

    # RX2's gain 10 dB lower: RX2 must drop by 10 dB, RX1 stay
    y1, y2 = caps["rx2low"]
    g1, g2 = channel_tone(y1, fs, f_off, fc), channel_tone(y2, fs, f_off, fc)
    step = {"rx1": g1, "rx2": g2,
            "rx1_change_db": g1["level_dbfs"] - c1["level_dbfs"],
            "rx2_change_db": g2["level_dbfs"] - c2["level_dbfs"]}
    if c1["present"] and abs(step["rx1_change_db"]) > GAIN_OTHER_MAX_DB:
        fails.append("RX1 moved %+.1f dB when RX2's gain went down %d dB (limit %.1f)"
                     % (step["rx1_change_db"], GAIN_STEP_DB, GAIN_OTHER_MAX_DB))
    if c2["present"] and abs(step["rx2_change_db"] + GAIN_STEP_DB) > GAIN_STEP_TOL_DB:
        fails.append("RX2 moved %+.1f dB when its gain went down %d dB (limit -%d +-%.1f)"
                     % (step["rx2_change_db"], GAIN_STEP_DB, GAIN_STEP_DB, GAIN_STEP_TOL_DB))
    if g1["present"] and g2["present"]:
        b = tone_blocks(y1, y2, fs, ftone)
        step["phase_spread_deg"] = b["phase_spread_deg"]
        if both:
            step["phase_shift_deg"] = float(wrap_deg(b["phase_diff_deg"] - m["phase_diff_deg"]))
        if b["phase_spread_deg"] > PHASE_SPREAD_MAX_DEG:
            fails.append("RX2 gain step: phase difference spread %.1f deg (limit %.1f)"
                         % (b["phase_spread_deg"], PHASE_SPREAD_MAX_DEG))
    m["gain_step"] = step

    # each channel alone against the two-channel stream
    single = {}
    for name, cap, ref in (("rx1", "rx1", c1), ("rx2", "rx2", c2)):
        s = channel_tone(caps[cap][0], fs, f_off, fc)
        s["vs_dual_db"] = s["level_dbfs"] - ref["level_dbfs"]
        single[name] = s
        if not s["present"]:
            fails.append("%s alone: no tone (%.1f dB above the floor)" % (name.upper(), s["snr_db"]))
        elif ref["present"] and abs(s["vs_dual_db"]) > SINGLE_DUAL_MAX_DB:
            fails.append("%s alone %+.1f dB from %s in the two-channel stream (limit %.1f)"
                         % (name.upper(), s["vs_dual_db"], name.upper(), SINGLE_DUAL_MAX_DB))
    m["single"] = single

    # phase difference after tuning away and back (reported, not judged; each
    # capture's own spread is judged like any other)
    phases = [m["phase_diff_deg"]] if both else []
    spreads = []
    for i in range(len(meta["retune_away"])):
        r1, r2 = caps["retune%d" % i]
        t1, t2 = channel_tone(r1, fs, f_off, fc), channel_tone(r2, fs, f_off, fc)
        if not (t1["present"] and t2["present"]):
            fails.append("retune %d: no tone on %s" % (i, "RX1" if not t1["present"] else "RX2"))
            continue
        b = tone_blocks(r1, r2, fs, f_off + 0.5 * (t1["freq_err_hz"] + t2["freq_err_hz"]))
        phases.append(b["phase_diff_deg"])
        spreads.append(b["phase_spread_deg"])
        if b["phase_spread_deg"] > PHASE_SPREAD_MAX_DEG:
            fails.append("retune %d: phase difference spread %.1f deg (limit %.1f)"
                         % (i, b["phase_spread_deg"], PHASE_SPREAD_MAX_DEG))
    rt = {"away_hz": meta["retune_away"], "phases_deg": phases, "spreads_deg": spreads}
    if len(phases) >= 2:
        rt.update(retune_summary(phases))
    m["retune"] = rt

    # sample alignment, from the chirp
    a = align(caps["bb"][0], caps["bb"][1], fs)
    m["align"] = a
    if a["peak_ratio_db"] < CORR_PEAK_MIN_DB:
        fails.append("no clear cross-correlation peak (%.1f dB above the other lags, limit %.1f)"
                     % (a["peak_ratio_db"], CORR_PEAK_MIN_DB))
    elif a["lag_samples"] != LAG_SAMPLES:
        fails.append("RX2 is %d samples %s RX1" % (abs(a["lag_samples"]),
                                                    "behind" if a["lag_samples"] > 0 else "ahead of"))
    elif a["lag_est_samples"] is None:
        fails.append("fractional lag: only %d coherent bins" % a["fit_bins"])
    elif abs(a["lag_est_samples"]) > FRAC_LAG_MAX:
        fails.append("RX2 lags RX1 by %+.3f samples (limit %.2f)"
                     % (a["lag_est_samples"], FRAC_LAG_MAX))
    m["pass"] = not fails
    m["fails"] = fails
    return m


def tx_measure(y, meta):
    """One B200mini capture of a TX run: both tones (searched within the
    frequency tolerance), then the images and LO leakage where the tones'
    common frequency offset puts them, levels in dBFS."""
    fc, fsb = meta["fc"], meta["fs_b200"]
    pos = tx_positions(meta["f1"], meta["f2"], meta["delta"])
    f, s, enbw = spectrum(y[len(y) // 8:], fsb)     # skip the receiver settling (as rf-check)
    df = float(f[1] - f[0])
    floor = float(10 * np.log10(np.median(np.abs(s) ** 2) + 1e-30))
    out = {"floor_dbfs": floor, "bin_hz": df}
    errs = []
    for k in ("t1", "t2"):
        fq, lev = tone_at(f, s, enbw, pos[k], freq_tol(fc))
        err = fq - pos[k]
        present = bool(lev >= floor + TONE_SNR_DB and abs(err) <= freq_tol(fc))
        out[k] = {"freq_err_hz": err, "level_dbfs": lev, "snr_db": lev - floor, "present": present}
        if present:
            errs.append(err)
    # every component of the bladeRF's is off by the same amount (the two
    # radios' LO offset), so an absent tone, the images and the LO leakage are
    # measured just where they would be
    eps = float(np.mean(errs)) if errs else 0.0
    for k in ("t1", "t2"):
        if not out[k]["present"] and errs:
            lev = tone_at(f, s, enbw, pos[k] + eps, 2 * df)[1]
            out[k].update(level_dbfs=lev, snr_db=lev - floor)
    for k in ("i1", "i2", "lo"):
        out[k] = tone_at(f, s, enbw, pos[k] + eps, 2 * df)[1]
    return out


def tx_levels(meta, caps, m, fails):
    """The tone runs' part of the tx record (into m and fails)."""
    fc = meta["fc"]
    d = tx_measure(caps["dual"][0], meta)
    t1, t2 = d["t1"], d["t2"]
    m["tx1"] = dict(t1, image_dbc=d["i1"] - t1["level_dbfs"])
    m["tx2"] = dict(t2, image_dbc=d["i2"] - t2["level_dbfs"])
    m["dual_lo_dbc"] = d["lo"] - max(t1["level_dbfs"], t2["level_dbfs"])
    m["floor_dbfs"], m["bin_hz"] = d["floor_dbfs"], d["bin_hz"]
    for name, t in (("TX1", t1), ("TX2", t2)):
        if not t["present"]:
            fails.append("%s: no tone in the two-channel run (%.1f dB above the floor, %+.0f Hz off)"
                         % (name, t["snr_db"], t["freq_err_hz"]))
    both = t1["present"] and t2["present"]
    if both:
        m["level_diff_db"] = t2["level_dbfs"] - t1["level_dbfs"]
        if abs(m["level_diff_db"]) > LEVEL_DIFF_MAX_DB:
            fails.append("TX2 %+.1f dB from TX1 at equal gains (limit %.1f)"
                         % (m["level_diff_db"], LEVEL_DIFF_MAX_DB))
        m["freq_diff_hz"] = t2["freq_err_hz"] - t1["freq_err_hz"]
        if abs(m["freq_diff_hz"]) > FREQ_AGREE_BINS * d["bin_hz"]:
            fails.append("TX2's tone error %+.1f Hz from TX1's (limit %.1f Hz, one bin)"
                         % (m["freq_diff_hz"], FREQ_AGREE_BINS * d["bin_hz"]))

    # TX2's gain 10 dB lower: only TX2's tone may drop, by 10 dB
    low = tx_measure(caps["tx2low"][0], meta)
    step = {"tx1_change_db": low["t1"]["level_dbfs"] - t1["level_dbfs"],
            "tx2_change_db": low["t2"]["level_dbfs"] - t2["level_dbfs"],
            "tx1": low["t1"], "tx2": low["t2"]}
    if t1["present"] and abs(step["tx1_change_db"]) > GAIN_OTHER_MAX_DB:
        fails.append("TX1's tone moved %+.1f dB when TX2's gain went down %d dB (limit %.1f)"
                     % (step["tx1_change_db"], GAIN_STEP_DB, GAIN_OTHER_MAX_DB))
    if t2["present"] and abs(step["tx2_change_db"] + GAIN_STEP_DB) > GAIN_STEP_TOL_DB:
        fails.append("TX2's tone moved %+.1f dB when its gain went down %d dB (limit -%d +-%.1f)"
                     % (step["tx2_change_db"], GAIN_STEP_DB, GAIN_STEP_DB, GAIN_STEP_TOL_DB))
    m["gain_step"] = step

    # each channel alone: its own tone, at the two-channel level; the other
    # channel's tone absent; its image and LO leakage
    single = {}
    for name, own, other, img, ref in (("tx1", "t1", "t2", "i1", t1), ("tx2", "t2", "t1", "i2", t2)):
        s = tx_measure(caps[name][0], meta)
        lev = s[own]["level_dbfs"]
        r = {"freq_err_hz": s[own]["freq_err_hz"], "level_dbfs": lev, "snr_db": s[own]["snr_db"],
             "present": s[own]["present"], "vs_dual_db": lev - ref["level_dbfs"],
             "other_dbc": s[other]["level_dbfs"] - lev,
             "image_dbc": s[img] - lev, "lo_dbc": s["lo"] - lev}
        single[name] = r
        nm = name.upper()
        if not r["present"]:
            fails.append("%s alone: no tone (%.1f dB above the floor, %+.0f Hz off)"
                         % (nm, r["snr_db"], r["freq_err_hz"]))
            continue
        if ref["present"] and abs(r["vs_dual_db"]) > SINGLE_DUAL_MAX_DB:
            fails.append("%s alone %+.1f dB from %s in the two-channel run (limit %.1f)"
                         % (nm, r["vs_dual_db"], nm, SINGLE_DUAL_MAX_DB))
        if r["other_dbc"] > ABSENT_MAX_DBC:
            fails.append("%s alone: the other channel's tone is there at %.1f dBc (limit %.0f)"
                         % (nm, r["other_dbc"], ABSENT_MAX_DBC))
    m["single"] = single


def tx_sync(meta, caps, m, fails):
    """The sync runs' part of the tx record (into m and fails): in every run
    TX2's lag and phase relative to TX1, judged as rx's alignment and phase
    spread; the phases over the runs (each a new stream, the later ones
    after tuning elsewhere first) reported as rx's retunes."""
    runs = {}
    for name, away in SYNC_RUNS:
        if name not in caps:
            continue
        s = analyze_sync(caps[name][0], meta)
        s["away_hz"] = None if away is None else meta["retune_away"][away]
        runs[name] = s
        tag = name if away is None else "%s (tuned to %.0f MHz first)" % (name, s["away_hz"] / 1e6)
        if s["marker_lag_samples"] is None:
            fails.append("%s: TX2's marker is not there (%.1f dB above the floor)"
                         % (tag, s["markers"]["tx2"]["snr_db"]))
        if s["lag_est_samples"] is None:
            fails.append("%s: %d lines %.0f dB above their scatter (limit %d of %d)"
                         % (tag, s["fit_lines"], SYNC_LINE_SNR_DB, SYNC_MIN_LINES, s["lines"]))
            continue
        if s["lag_samples"] != LAG_SAMPLES:
            fails.append("%s: TX2 is %d samples %s TX1" % (tag, abs(s["lag_samples"]),
                                                           "behind" if s["lag_samples"] > 0 else "ahead of"))
        elif abs(s["lag_est_samples"]) > FRAC_LAG_MAX:
            fails.append("%s: TX2 lags TX1 by %+.3f samples (limit %.2f)"
                         % (tag, s["lag_est_samples"], FRAC_LAG_MAX))
        if (s["marker_lag_samples"] is not None and
                abs(s["marker_lag_samples"] - s["lag_est_samples"]) > SYNC_MARKER_AGREE):
            fails.append("%s: the markers' lag %+.1f samples, the lines' %+.3f (limit %.0f apart)"
                         % (tag, s["marker_lag_samples"], s["lag_est_samples"], SYNC_MARKER_AGREE))
        if s["cycle_phase_spread_deg"] > PHASE_SPREAD_MAX_DEG:
            fails.append("%s: phase difference spread %.1f deg over %d cycles (limit %.1f)"
                         % (tag, s["cycle_phase_spread_deg"], s["cycles"], PHASE_SPREAD_MAX_DEG))
        if s["cycle_lag_spread_samples"] > FRAC_LAG_MAX:
            fails.append("%s: TX2's lag moved %.2f samples over %d cycles (limit %.2f)"
                         % (tag, s["cycle_lag_spread_samples"], s["cycles"], FRAC_LAG_MAX))
    phases = [r["phase_at_fc_deg"] for r in runs.values() if r.get("phase_at_fc_deg") is not None]
    sync = {"runs": runs, "phases_deg": phases}
    if len(phases) >= 2:
        sync.update(retune_summary(phases))
    m["sync"] = sync


def analyze_tx(meta, caps):
    """The tx record from the B200mini captures: the tone runs' part
    (tx_levels) and the sync runs' (tx_sync), each when its captures are
    there (HWTEST_2CH_TX=sync takes only the sync runs)."""
    m = {"test": "twoch_tx", "fc": meta["fc"], "fs": meta["fs"], "f1": meta["f1"], "f2": meta["f2"],
         "b200_offset_hz": -meta["delta"], "gain_db": meta["gain"], "gain_low_db": meta["gain_low"],
         "runs": [k for k in meta["caps"] if k in caps]}
    fails = []
    if "dual" in caps:
        tx_levels(meta, caps, m, fails)
    if any(name in caps for name, _ in SYNC_RUNS):
        tx_sync(meta, caps, m, fails)
    m["pass"] = not fails
    m["fails"] = fails
    return m


# --- The bladeRF and the B200mini (hardware) ------------------------------

CLI_ERROR = re.compile(r"^\s*Error:", re.M)


def cli(cmds, timeout=CLI_TIMEOUT_S):
    """bladeRF-cli through bin/bladerf-cli-safe, like rf-check.run_cli(), but
    also failing on bladeRF-cli's error lines: it stops at the first command
    that fails and still exits 0 (its input loop always returns 0)."""
    args = [rf.CLI]
    for c in cmds:
        args += ["-e", c]
    p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0 or CLI_ERROR.search(p.stderr):
        raise RuntimeError("bladeRF-cli failed (%d): %s %s"
                           % (p.returncode, p.stdout[-1500:], p.stderr[-1500:]))
    return p.stdout + p.stderr


def work_dir():
    """Captures go to RAM (as pulse-check's), so that writing them keeps up."""
    return tempfile.TemporaryDirectory(dir="/dev/shm" if os.path.isdir("/dev/shm") else None)


def b200_tx_gain(fc):
    """The B200mini's TX gain at fc (rx subcommand)."""
    return B200_TX_GAIN_AT.get(fc, B200_TX_GAIN)


def with_b200_tx(u, proto, fc, fs, fn):
    """Runs fn() while the B200mini sends proto over and over at fc (rf-check's
    b200_transmit, gain capped there); fails on a late underflow."""
    stop = threading.Event()
    txr = {}

    def run():
        try:
            rf.b200_transmit(u, proto, fc, fs, b200_tx_gain(fc), stop, txr)
        except Exception as e:      # reported below
            txr["error"] = str(e)

    t = threading.Thread(target=run)
    t.start()
    try:
        time.sleep(B200_WARMUP_S)
        if "error" not in txr:
            out = fn()
    finally:
        stop.set()
        t.join()
    if "error" in txr:
        raise RuntimeError("B200mini transmit failed: %s" % txr["error"])
    if "underflows" not in txr:
        raise RuntimeError("B200mini transmit thread failed")
    late = [s for s in txr["underflows"] if s > 1.0]    # the first second is start-up
    if late:
        raise RuntimeError("B200mini TX underflowed %d times while the bladeRF was receiving"
                           % len(late))
    return out


def rx_meta(point):
    fc, fs, f_off = point
    n = lambda t: int(round((SKIP_S + t) * fs))
    caps = {"dual": {"chans": [1, 2], "n": n(RX_DUAL_S)},
            "rx2low": {"chans": [1, 2], "n": n(RX_STEP_S)},
            "rx1": {"chans": [1], "n": n(RX_SINGLE_S)},
            "rx2": {"chans": [2], "n": n(RX_SINGLE_S)}}
    away = retune_away(fc)
    for i in range(len(away)):
        caps["retune%d" % i] = {"chans": [1, 2], "n": n(RX_RETUNE_S)}
    caps["bb"] = {"chans": [1, 2], "n": n(RX_BB_S)}
    return {"mode": "rx", "fc": fc, "fs": fs, "f_off": f_off, "gain": RX_GAIN,
            "gain_low": RX_GAIN_LOW, "b200_tx_gain": b200_tx_gain(fc), "skip_n": int(round(SKIP_S * fs)),
            "retune_away": away, "chirp_n": CHIRP_N, "chirp_span": CHIRP_SPAN, "caps": caps}


def rx_setup_cmds(meta):
    """Both RX channels set up alike (the RX LO, sample rate and bandwidth are
    shared in the AD9361, but an application using both sets both)."""
    fc, fs = meta["fc"], meta["fs"]
    c = ["set loopback none", "set rx_mux baseband"]
    for ch in (1, 2):
        c += ["set frequency rx%d %d" % (ch, fc),
              "set samplerate rx%d %d" % (ch, fs),
              "set bandwidth rx%d %d" % (ch, min(fs, 56e6))]
    return c + ["set agc off", "set gain rx1 %d" % meta["gain"], "set gain rx2 %d" % meta["gain"]]


def rx_capture_cmds(meta, name, path):
    cap = meta["caps"][name]
    return ["rx config file=%s format=bin n=%d channel=%s %s"
            % (path, cap["n"] * len(cap["chans"]), ",".join(str(c) for c in cap["chans"]), RX_STREAM),
            "rx start", RX_WAIT]


def rx_tone_cmds(meta, path):
    """One bladeRF-cli run: both channels, RX2 10 dB lower, each channel
    alone, and the retunes."""
    fc = meta["fc"]
    c = rx_setup_cmds(meta) + rx_capture_cmds(meta, "dual", path("dual"))
    c += ["set gain rx2 %d" % meta["gain_low"]] + rx_capture_cmds(meta, "rx2low", path("rx2low"))
    c += ["set gain rx2 %d" % meta["gain"]]
    c += rx_capture_cmds(meta, "rx1", path("rx1")) + rx_capture_cmds(meta, "rx2", path("rx2"))
    for i, away in enumerate(meta["retune_away"]):
        c += ["set frequency rx1 %d" % away, "set frequency rx2 %d" % away,
              "set frequency rx1 %d" % fc, "set frequency rx2 %d" % fc]
        c += rx_capture_cmds(meta, "retune%d" % i, path("retune%d" % i))
    return c


def rx_chirp_cmds(meta, path):
    return rx_setup_cmds(meta) + rx_capture_cmds(meta, "bb", path("bb"))


def load_rx_caps(meta, path):
    return {name: load_bladerf(path(name), len(cap["chans"]), cap["n"], meta["skip_n"])
            for name, cap in meta["caps"].items()}


def save_point(meta, files, arrays):
    """--save: the raw captures (bladeRF-cli files as they are, B200mini
    captures as complex64 .npy) and a manifest that --analyze reads."""
    os.makedirs(SAVE_DIR, exist_ok=True)
    tag = "twoch-%s-%.2fMHz-%s-%s" % (meta["mode"], meta["fc"] / 1e6,
                                      os.environ.get("BLADERF_DEFAULT_TUNING_MODE") or "default",
                                      time.strftime("%Y%m%d-%H%M%S"))
    man = {"twoch_manifest": 1, "meta": meta, "files": {}}
    for name, src in files.items():
        man["files"][name] = "%s-%s.bin" % (tag, name)
        shutil.copyfile(src, os.path.join(SAVE_DIR, man["files"][name]))
    for name, x in arrays.items():
        man["files"][name] = "%s-%s.npy" % (tag, name)
        np.save(os.path.join(SAVE_DIR, man["files"][name]), np.asarray(x, dtype=np.complex64))
    with open(os.path.join(SAVE_DIR, tag + ".json"), "w") as f:
        json.dump(man, f, indent=1)


def check_rx(u, point):
    """B200mini transmits (a tone, then a chirp), bladeRF receives on both
    channels."""
    meta = rx_meta(point)
    fc, fs, f_off = point
    with work_dir() as d:
        path = lambda name: os.path.join(d, name + ".bin")
        tone = rf.tone(int(fs * 0.01), fs, f_off, B200_AMP).astype(np.complex64)
        with_b200_tx(u, tone, fc, fs, lambda: cli(rx_tone_cmds(meta, path)))
        with_b200_tx(u, chirp(CHIRP_N, fs), fc, fs, lambda: cli(rx_chirp_cmds(meta, path)))
        caps = load_rx_caps(meta, path)
        if SAVE_DIR:
            save_point(meta, {name: path(name) for name in meta["caps"]}, {})
    return analyze_rx(meta, caps)


def tx_meta(point):
    fc, fs, _ = point
    f1, f2, delta = tx_plan(fs)
    caps = {k: {"kind": "b200"} for k in ("dual", "tx2low", "tx1", "tx2")}
    caps.update({name: {"kind": "b200_sync"} for name, _ in SYNC_RUNS})
    return {"mode": "tx", "fc": fc, "fs": fs, "fs_b200": fs, "f1": f1, "f2": f2, "delta": delta,
            "gain": TX_GAIN, "gain_low": TX_GAIN_LOW, "b200_rx_gain": rf.B200_RX_GAIN,
            "retune_away": retune_away(fc), "caps": caps}


# (capture, file, channels, TX1 gain, TX2 gain): the four TX runs
TX_RUNS = [("dual", "dual", [1, 2], TX_GAIN, TX_GAIN),
           ("tx2low", "dual", [1, 2], TX_GAIN, TX_GAIN_LOW),
           ("tx1", "tx1", [1], TX_GAIN, TX_GAIN),
           ("tx2", "tx2", [2], TX_GAIN, TX_GAIN)]


def tx_cmds(meta, path, chans, g1, g2, away=None):
    """One bladeRF-cli run that sends path; away: both TX channels tuned
    there first, then to the carrier (the sync runs' retunes)."""
    if max(g1, g2) > TX_GAIN_MAX:
        raise ValueError("bladeRF TX gain above %d dB" % TX_GAIN_MAX)
    fc, fs = meta["fc"], meta["fs"]
    c = ["set loopback none"]
    if away is not None:
        c += ["set frequency tx1 %d" % away, "set frequency tx2 %d" % away]
    for ch in (1, 2):
        c += ["set frequency tx%d %d" % (ch, fc),
              "set samplerate tx%d %d" % (ch, fs),
              "set bandwidth tx%d %d" % (ch, min(fs, 56e6))]
    return c + ["set gain tx1 %d" % g1, "set gain tx2 %d" % g2,
                "tx config file=%s format=bin repeat=%d channel=%s %s"
                % (path, TX_REPEAT, ",".join(str(ch) for ch in chans), TX_STREAM),
                "tx start", TX_WAIT]


def write_tx_files(meta, d):
    """TX1's tone alone, TX2's alone, and both interleaved (TX1 first)."""
    fs = meta["fs"]
    n = int(round(fs * TX_FILE_S))
    t1 = rf.to_sc16q11(rf.tone(n, fs, meta["f1"], TONE_AMP))
    t2 = rf.to_sc16q11(rf.tone(n, fs, meta["f2"], TONE_AMP))
    files = {"dual": interleave_sc16([t1, t2]), "tx1": t1, "tx2": t2}
    paths = {}
    for k, v in files.items():
        paths[k] = os.path.join(d, "tx-%s.bin" % k)
        v.astype("<i2").tofile(paths[k])
    return paths


def write_sync_file(meta, d):
    """The sync file: whole cycles (sync_cycle), about TX_FILE_S long, both
    channels interleaved (TX1 first)."""
    x1, x2 = sync_cycle()
    reps = max(1, int(round(TX_FILE_S * meta["fs"] / len(x1))))
    path = os.path.join(d, "tx-sync.bin")
    interleave_sc16([rf.to_sc16q11(np.tile(x1, reps)),
                     rf.to_sc16q11(np.tile(x2, reps))]).astype("<i2").tofile(path)
    return path


def sync_present(y, meta):
    """Whether a capture holds TX1's marker 30 dB above the floor (it is on
    in a fifth of the time, so in a capture's spectrum it stands about 14 dB
    lower than a steady tone would)."""
    f, s, enbw = spectrum(y, meta["fs_b200"])
    floor = float(10 * np.log10(np.median(np.abs(s) ** 2) + 1e-30))
    lev = tone_at(f, s, enbw, SYNC_MARKER1 * meta["fs_b200"] / SYNC_NB, freq_tol(meta["fc"]))[1]
    return lev >= floor + TONE_SNR_DB


def tx_run(u, meta, path, chans, g1, g2, away=None, sync=False):
    """One bladeRF transmission (bladeRF-cli in a thread, as rf-check.check_tx)
    and the B200mini capture to measure it; returns (capture, seconds until
    the first tone). sync: the sync file, the B200mini tuned to the carrier,
    a capture of sync_capture_len() samples."""
    fc = meta["fc"]
    want = ["t%d" % ch for ch in chans]
    n_find = int(meta["fs_b200"] * B200_CAPTURE_S)
    if sync:
        b200_fc, n = fc, sync_capture_len()
        there = lambda y: sync_present(y, meta)
    else:
        b200_fc, n = fc - meta["delta"], n_find
        there = lambda y: any(tx_measure(y, meta)[k]["present"] for k in want)
    result = {}

    def bladerf_tx():
        try:
            result["out"] = cli(tx_cmds(meta, path, chans, g1, g2, away))
        except Exception as e:      # re-raised below
            result["error"] = e

    t = threading.Thread(target=bladerf_tx)
    t.start()
    t0, found, y, ended = time.time(), None, None, False
    try:
        time.sleep(2.0)
        while found is None and time.time() - t0 < TX_SEARCH_S and t.is_alive():
            y = rf.b200_capture(u, n_find, b200_fc, meta["fs_b200"], rf.B200_RX_GAIN)
            if there(y):
                found = round(time.time() - t0, 1)
            else:
                time.sleep(0.5)
        if found is not None:
            time.sleep(rf.TX_SETTLE_S)      # past the start-up bursts (see rf-check)
            y = rf.b200_capture(u, n, b200_fc, meta["fs_b200"], rf.B200_RX_GAIN)
            ended = not t.is_alive()
    finally:
        t.join()
    if "error" in result:
        raise result["error"]
    if found is None:
        raise RuntimeError("no tone while the bladeRF transmitted on TX%s"
                           % "+".join(str(c) for c in chans))
    if ended:
        raise RuntimeError("the bladeRF transmission ended during the measurement")
    return y, found


def sync_only():
    """HWTEST_2CH_TX=sync: tx makes only the sync runs (phase and timing)."""
    v = os.environ.get("HWTEST_2CH_TX", "")
    if v not in ("", "all", "sync"):
        sys.exit("HWTEST_2CH_TX=%s: all (the default) or sync" % v)
    return v == "sync"


def check_tx(u, point):
    """bladeRF transmits (the tone runs: both channels, TX2 lower, each
    alone; the sync runs), B200mini measures."""
    meta = tx_meta(point)
    caps, found = {}, {}
    with work_dir() as d:
        if not sync_only():
            files = write_tx_files(meta, d)
            for name, fname, chans, g1, g2 in TX_RUNS:
                y, found[name] = tx_run(u, meta, files[fname], chans, g1, g2)
                caps[name] = [y]
        path = write_sync_file(meta, d)
        for name, away in SYNC_RUNS:
            y, found[name] = tx_run(u, meta, path, [1, 2], TX_GAIN, TX_GAIN, sync=True,
                                    away=None if away is None else meta["retune_away"][away])
            caps[name] = [y]
    meta["caps"] = {k: v for k, v in meta["caps"].items() if k in caps}
    if SAVE_DIR:
        save_point(meta, {}, {k: v[0] for k, v in caps.items()})
    m = analyze_tx(meta, caps)
    m["tone_after_s"] = found
    return m


# --- --analyze ------------------------------------------------------------

def analyze_dir(d):
    """Records for every manifest that --save wrote into d."""
    recs = []
    for man_path in sorted(glob.glob(os.path.join(d, "*.json"))):
        with open(man_path) as f:
            man = json.load(f)
        if man.get("twoch_manifest") != 1:
            continue
        meta = man["meta"]
        path = lambda name: os.path.join(d, man["files"][name])
        try:
            if meta["mode"] == "rx":
                m = analyze_rx(meta, load_rx_caps(meta, path))
            else:
                m = analyze_tx(meta, {k: [np.load(path(k))] for k in meta["caps"]})
        except Exception as e:  # recorded
            m = {"test": "twoch_" + meta["mode"], "fc": meta["fc"], "fs": meta["fs"],
                 "pass": False, "fails": [str(e)], "error": str(e)}
        m["analyzed"] = os.path.basename(man_path)
        recs.append(m)
    return recs


# --- Output ---------------------------------------------------------------

def _json_default(o):
    if isinstance(o, np.bool_):
        return bool(o)
    if isinstance(o, np.integer):
        return int(o)
    if isinstance(o, np.floating):
        return float(o)
    if isinstance(o, np.ndarray):
        return o.tolist()
    raise TypeError("not JSON serializable: %r" % type(o))


def write_record(out, m):
    out.write(json.dumps(m, default=_json_default) + "\n")
    out.flush()
    print("%s %.2f MHz: %s" % (m.get("test"), m.get("fc", 0) / 1e6,
                               "PASS" if m.get("pass") else "FAIL " + "; ".join(m.get("fails", []))),
          file=sys.stderr)


# --- Self-test (synthetic signals, no hardware) ---------------------------

def self_test():
    """The analysis against synthetic captures with known answers. Returns
    the number of failed checks."""
    rng = np.random.default_rng(20261115)
    bad = []

    def check(cond, what):
        print("  %s %s" % ("ok  " if cond else "FAIL", what))
        if not cond:
            bad.append(what)

    def noise(n, db):
        return (10 ** (db / 20) / np.sqrt(2)) * (rng.standard_normal(n) + 1j * rng.standard_normal(n))

    def tone(n, fs, f, amp, ph=0.0):
        return amp * np.exp(1j * (2 * np.pi * f * np.arange(n) / fs + ph))

    def delayed(x, d):
        """x delayed by d samples (circularly; exact for a periodic x)."""
        f = np.fft.fftfreq(len(x))
        return np.fft.ifft(np.fft.fft(x) * np.exp(-2j * np.pi * f * d))

    fc, fs, f_off = TWOCH_POINTS[0]
    print("deinterleaving")
    a = rng.integers(-2048, 2048, size=(1000, 2)).astype(np.int16)
    b = rng.integers(-2048, 2048, size=(1000, 2)).astype(np.int16)
    raw = interleave_sc16([a.reshape(-1), b.reshape(-1)])
    check(list(raw[:8]) == [a[0, 0], a[0, 1], b[0, 0], b[0, 1], a[1, 0], a[1, 1], b[1, 0], b[1, 1]],
          "layout RX1 I, RX1 Q, RX2 I, RX2 Q, ... (libbladeRF.h)")
    y1, y2 = deinterleave_sc16(raw, 2)
    check(np.array_equal(y1, (a[:, 0] + 1j * a[:, 1]) / 2048.0) and
          np.array_equal(y2, (b[:, 0] + 1j * b[:, 1]) / 2048.0), "two channels back")
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "x.bin")
        raw.astype("<i2").tofile(p)
        z1, z2 = load_bladerf(p, 2, 1000, 10)
        check(np.array_equal(z1, y1[10:]) and np.array_equal(z2, y2[10:]), "file, first samples skipped")
        try:
            load_bladerf(p, 2, 1001, 0)
            check(False, "short file refused")
        except RuntimeError:
            check(True, "short file refused")
        t = rf.to_sc16q11(tone(1000, fs, f_off, 0.5))
        q = os.path.join(d, "one.bin")
        t.tofile(q)
        check(np.allclose(deinterleave_sc16(np.fromfile(q, dtype="<i2"), 1)[0], rf.from_sc16q11(q)),
              "one channel as rf-check.from_sc16q11")

    print("tone level, frequency, phase")
    n = 1 << 17
    eps = 2e-6 * fc                 # B200mini 2 ppm off
    x1 = tone(n, fs, f_off + eps, 0.05) + noise(n, -75)
    x2 = tone(n, fs, f_off + eps, 0.05 * 10 ** (-1.2 / 20), np.radians(37.0)) + noise(n, -75)
    c1, c2 = channel_tone(x1, fs, f_off, fc), channel_tone(x2, fs, f_off, fc)
    check(abs(c1["level_dbfs"] - 20 * np.log10(0.05)) < 0.05, "level %.3f dBFS (-26.02)" % c1["level_dbfs"])
    check(abs(c1["freq_err_hz"] - eps) < 0.1 * c1["bin_hz"], "frequency error %.1f Hz (%.1f)" % (c1["freq_err_hz"], eps))
    check(abs(c2["level_dbfs"] - c1["level_dbfs"] + 1.2) < 0.05, "level difference -1.2 dB")
    b0 = tone_blocks(x1, x2, fs, f_off + c1["freq_err_hz"])
    check(abs(b0["phase_diff_deg"] - 37.0) < 0.2 and b0["phase_spread_deg"] < 0.5,
          "phase difference %.2f deg (37), spread %.2f deg" % (b0["phase_diff_deg"], b0["phase_spread_deg"]))
    xs = np.concatenate([x2[:n // 2], x2[n // 2 + 1:], x2[-1:]])     # one sample lost on RX2
    b1 = tone_blocks(x1, xs, fs, f_off + c1["freq_err_hz"])
    check(abs(b1["phase_spread_deg"] - 360 * f_off / fs) < 3,
          "one lost sample: spread %.1f deg (%.1f)" % (b1["phase_spread_deg"], 360 * f_off / fs))
    off = tone(n, fs, f_off + 0.3 * c1["bin_hz"], 0.05)
    co = channel_tone(off, fs, f_off, fc)
    check(abs(co["level_dbfs"] - 20 * np.log10(0.05)) < 0.05, "level between bins %.3f dBFS" % co["level_dbfs"])
    nz = channel_tone(noise(n, -60), fs, f_off, fc)
    check(not nz["present"], "noise alone: no tone (%.1f dB above the floor)" % nz["snr_db"])

    print("lag")
    base = np.tile(chirp(CHIRP_N, fs), 32).astype(np.complex128)
    for d, snr in ((0.0, 40), (0.3, 40), (-0.2, 30), (2.3, 40), (-1.6, 40), (0.07, 20)):
        r = align(base + noise(len(base), -6 - snr), delayed(base, d) + noise(len(base), -6 - snr), fs)
        li = int(np.round(d))
        check(r["lag_samples"] == li and abs(r["lag_est_samples"] - d) < 0.01 and
              abs(r["lag_xcorr_samples"] - d) < 0.03 and r["peak_ratio_db"] > CORR_PEAK_MIN_DB,
              "lag %+.2f at %d dB SNR: %d, %+.4f (phase slope), %+.4f (correlation), peak %.1f dB"
              % (d, snr, r["lag_samples"], r["lag_est_samples"], r["lag_xcorr_samples"], r["peak_ratio_db"]))
    r = align(base * np.exp(1j * 1.0), base * np.exp(1j * 1.5), fs)
    check(abs(r["phase_at_fc_deg"] - np.degrees(0.5)) < 0.1, "phase at the carrier %.2f deg (28.65)" % r["phase_at_fc_deg"])
    r = align(noise(len(base), -20), noise(len(base), -20), fs)
    check(r["peak_ratio_db"] < CORR_PEAK_MIN_DB and r["lag_est_samples"] is None,
          "uncorrelated noise: no clear peak (%.1f dB), no fit" % r["peak_ratio_db"])

    print("retune classes")
    check(retune_summary([10, 10.5, 9.2, 11])["class"] == "stable", "stable")
    check(retune_summary([10, 190.5, 9.2, -170.5])["class"] == "flips by 180 deg", "flips by 180 deg")
    check(retune_summary([10, 70, 200, 10])["class"] == "varies", "varies")
    check(abs(retune_summary([179, -179])["max_dev_deg"] - 2) < 1e-9, "wraps at 180 deg")

    print("rx analysis")
    meta = rx_meta((fc, fs, f_off))
    nn = 1 << 15

    def rx_caps(lvl=(0.05, 0.05), ph=40.0, low=(0.0, -10.0), single=(0.0, 0.0), lag=0.0,
                retune=(40.0, 40.0, 40.0, 40.0), slip=False, drop_rx2=False, snr=60):
        """Synthetic rx captures: the tone at lvl (snr dB above a fixed noise
        floor for 0.05), RX2 ph degrees from RX1, the gain-step capture with
        the channels moved by low dB, the single-channel ones by single dB,
        the retune captures at the given phases, the chirp lag samples late
        on RX2."""
        nl = 20 * np.log10(0.05) - snr

        def pair(a1, a2, p, k=nn):
            u1 = tone(k, fs, f_off + eps, a1) + noise(k, nl)
            u2 = tone(k, fs, f_off + eps, a2, np.radians(p)) + noise(k, nl)
            if slip:
                u2 = np.concatenate([u2[:k // 2], u2[k // 2 + 1:], u2[-1:]])
            return [u1, u2]
        a2 = 0.0 if drop_rx2 else lvl[1]
        g = lambda a, db: a * 10 ** (db / 20)
        c = {"dual": pair(lvl[0], a2, ph),
             "rx2low": pair(g(lvl[0], low[0]), g(a2, low[1]), ph + 3),
             "rx1": [pair(g(lvl[0], single[0]), 0, 0)[0]],
             "rx2": [pair(g(lvl[1], single[1]), 0, 0)[0]]}
        for i in range(RETUNES):
            c["retune%d" % i] = pair(lvl[0], a2, retune[i], 4 * BLOCK)
        bb = np.tile(chirp(CHIRP_N, fs), 16).astype(np.complex128) * 0.1
        c["bb"] = [bb + noise(len(bb), -60), delayed(bb, lag) + noise(len(bb), -60)]
        return c

    def has(m, text):
        return any(text in f for f in m["fails"])

    m = analyze_rx(meta, rx_caps())
    check(m["pass"], "good channels pass %s" % m["fails"])
    check(abs(m["phase_diff_deg"] - 40) < 0.3 and abs(m["gain_step"]["rx2_change_db"] + 10) < 0.1 and
          abs(m["gain_step"]["phase_shift_deg"] - 3) < 0.3 and m["retune"]["class"] == "stable",
          "phase 40 deg, RX2 step -10 dB, gain-step phase shift 3 deg, retunes stable")
    m = analyze_rx(meta, rx_caps(lvl=(0.05, 0.05 * 10 ** (-6 / 20))))
    check(not m["pass"] and has(m, "from RX1 at equal gains"), "RX2 6 dB low fails: %s" % m["fails"])
    m = analyze_rx(meta, rx_caps(low=(-10.0, 0.0)))
    check(has(m, "RX1 moved") and has(m, "RX2 moved"), "gain on the wrong channel fails: %s" % m["fails"])
    m = analyze_rx(meta, rx_caps(low=(-10.0, -10.0)))
    check(has(m, "RX1 moved") and not has(m, "RX2 moved"), "gain on both channels fails: %s" % m["fails"])
    m = analyze_rx(meta, rx_caps(low=(0.0, -8.0)))
    check(m["pass"], "a step of -8 dB passes (+-%.1f)" % GAIN_STEP_TOL_DB)
    m = analyze_rx(meta, rx_caps(single=(0.0, -25.0)))
    check(has(m, "RX2 alone"), "RX2 alone 25 dB low fails: %s" % m["fails"])
    m = analyze_rx(meta, rx_caps(single=(3.5, -6.0)))
    check(m["pass"], "single-channel shifts of +3.5 and -6 dB (resistive splitter) pass")
    m = analyze_rx(meta, rx_caps(slip=True))
    check(has(m, "phase difference spread"), "a lost sample fails: %s" % m["fails"][:1])
    m = analyze_rx(meta, rx_caps(lag=1.0))
    check(has(m, "1 samples behind"), "RX2 one sample late fails: %s" % m["fails"])
    m = analyze_rx(meta, rx_caps(lag=-0.4))
    check(has(m, "lags RX1 by -0.4"), "RX2 0.4 samples early fails: %s" % m["fails"])
    m = analyze_rx(meta, rx_caps(lag=0.1, snr=40))
    check(m["pass"] and abs(m["align"]["lag_est_samples"] - 0.1) < 0.02, "0.1 sample lag at 40 dB SNR passes")
    m = analyze_rx(meta, rx_caps(drop_rx2=True))
    check(has(m, "RX2: no tone") and not m["pass"], "no tone on RX2 fails: %s" % m["fails"][:1])
    m = analyze_rx(meta, rx_caps(retune=(40.0, 220.0, 41.0, 219.0)))
    check(m["pass"] and m["retune"]["class"] == "flips by 180 deg", "180 deg retune flips: reported, pass")
    try:
        analyze_rx(meta, rx_caps(lvl=(0.0, 0.0)))
        check(False, "no tone at all raises (retried)")
    except RuntimeError:
        check(True, "no tone at all raises (retried)")

    print("tx frequency plan")
    for fsx in sorted(set([p[1] for p in rf.POINTS] + [2e6, 61.44e6])):
        f1, f2, delta = tx_plan(fsx)
        u = fsx / 32
        pos = tx_positions(f1, f2, delta)
        meas = list(pos.values())
        others = [2 * f1 + delta, 2 * f2 + delta, delta - 3 * f1, delta - 3 * f2,
                  2 * f1 - f2 + delta, 2 * f2 - f1 + delta, 0.0] + [-v for v in meas]
        sep = min(abs(a - b) for i, a in enumerate(meas) for b in meas[i + 1:] + others)
        n_file = int(round(fsx * TX_FILE_S))
        cyc = [f * n_file / fsx for f in (f1, f2)]
        check(sep >= u - 1e-6 and max(abs(v) for v in meas) <= fsx / 4 + 1e-6 and
              all(abs(c - round(c)) < 1e-9 for c in cyc) and f2 not in (f1, -f1, 0),
              "%.2f Msps: components %s fs/32 apart (>= 1), within fs/4, whole cycles"
              % (fsx / 1e6, round(sep / u, 3)))

    print("tx analysis")
    tm = tx_meta((fc, fs, f_off))
    tp = tx_positions(tm["f1"], tm["f2"], tm["delta"])
    nt = 1 << 16
    teps = 1.5e-6 * fc

    def b200(a1=0.02, a2=0.02, img=(-40.0, -45.0), lo=-35.0, extra=0.0):
        y = noise(nt, -100)
        for k, a, ph in (("t1", a1, 0.3), ("t2", a2, 1.1)):
            if a:
                y = y + tone(nt, fs, tp[k] + teps, a, ph)
        for k, a, dbc in (("i1", a1, img[0]), ("i2", a2, img[1])):
            if a:
                y = y + tone(nt, fs, tp[k] + teps, a * 10 ** (dbc / 20))
        y = y + tone(nt, fs, tp["lo"] + teps, max(a1, a2) * 10 ** (lo / 20))
        if extra:
            y = y + tone(nt, fs, tp["t2"] + teps, a1 * 10 ** (extra / 20))
        return [y]

    good = {"dual": b200(), "tx2low": b200(a2=0.02 * 10 ** (-10 / 20)),
            "tx1": b200(a2=0.0), "tx2": b200(a1=0.0)}
    m = analyze_tx(tm, good)
    check(m["pass"], "good channels pass %s" % m["fails"])
    check(abs(m["tx1"]["image_dbc"] + 40) < 0.5 and abs(m["tx2"]["image_dbc"] + 45) < 0.5 and
          abs(m["single"]["tx1"]["lo_dbc"] + 35) < 0.5 and abs(m["single"]["tx2"]["image_dbc"] + 45) < 0.5 and
          abs(m["tx1"]["freq_err_hz"] - teps) < 0.2 * m["bin_hz"],
          "images -40/-45 dBc, LO -35 dBc, frequency error %.1f Hz (%.1f)" % (m["tx1"]["freq_err_hz"], teps))
    check(m["single"]["tx1"]["other_dbc"] < -60, "TX1 alone: TX2's tone %.1f dBc" % m["single"]["tx1"]["other_dbc"])
    bad_tx = dict(good, tx2low=b200(a1=0.02 * 10 ** (-10 / 20)))
    m = analyze_tx(tm, bad_tx)
    check(has(m, "TX1's tone moved") and has(m, "TX2's tone moved"), "gain on the wrong channel fails: %s" % m["fails"])
    m = analyze_tx(tm, dict(good, dual=b200(a2=0.0)))
    check(has(m, "TX2: no tone"), "no TX2 tone fails: %s" % m["fails"][:1])
    m = analyze_tx(tm, dict(good, tx1=b200(a2=0.0, extra=-12.0)))
    check(has(m, "the other channel's tone"), "TX2's tone in TX1's run fails: %s" % m["fails"])
    m = analyze_tx(tm, dict(good, tx2=b200(a1=0.0, a2=0.002)))
    check(has(m, "TX2 alone -20.0 dB"), "TX2 alone 20 dB low fails: %s" % m["fails"])
    m = analyze_tx(tm, dict(good, dual=b200(a2=0.02 * 10 ** (5 / 20))))
    check(has(m, "from TX1 at equal gains"), "TX2 5 dB high fails")

    print("tx sync (phase and timing)")
    k = sync_lines()
    marks = np.array([SYNC_MARKER1, SYNC_MARKER2])
    others = np.concatenate([-k, marks, -marks, [0]])
    sep = min(np.min(np.abs(kk - others)) for kk in k)
    check(len(k) == 29 and k.min() >= -SYNC_BAND * SYNC_NB and k.max() <= SYNC_BAND * SYNC_NB and sep >= 50,
          "29 lines within +-%.2f fs, %d bins or more from any line's image, the markers, their images "
          "and DC" % (SYNC_BAND, sep))
    sx1, sx2 = sync_cycle()
    gate = np.abs(sx1[:SYNC_NB]) > 0
    check(len(sx1) == len(sx2) == 5 * SYNC_NB and max(np.max(np.abs(sx1)), np.max(np.abs(sx2))) <= SYNC_AMP + 1e-9
          and np.sum(gate) == SYNC_NB - 2 * SYNC_MARKER_GAP,
          "cycle of 5 blocks, peaks %.3f/%.3f (<= %.1f), markers with silence around them"
          % (np.max(np.abs(sx1)), np.max(np.abs(sx2)), SYNC_AMP))
    blk = lambda x, b: x[b * SYNC_NB:(b + 1) * SYNC_NB]
    check(all(np.allclose(blk(sx2, b + 1), blk(sx1, b + 1) * np.exp(1j * np.radians(t)))
              for b, t in enumerate(SYNC_PHASES)), "TX2 = TX1 turned by 0, 90, 180, 270 degrees in blocks 1-4")
    sm = tx_meta((fc, fs, f_off))
    seps = 2e-6 * fc
    ncap = sync_capture_len()

    def sync_cap(lag=0.0, dphi=40.0, g2_db=-0.4, snr=60.0, slip=False, tx2=True, start=None):
        """A synthetic sync capture: the file's cycles through two paths (TX1
        2.7 samples late, TX2 lag samples later still, g2_db and dphi from
        TX1), images, LO leakage, the radios' offset, noise snr dB below a
        steady tone (in a 2^18 Blackman spectrum); slip: TX2 one sample
        later from the middle on."""
        reps = SYNC_CYCLES + 4
        a = delayed(np.tile(sx1, reps), 2.7) * 0.02
        b = delayed(np.tile(sx2, reps), 2.7 + lag) * 0.02 * 10 ** (g2_db / 20) * np.exp(1j * np.radians(dphi))
        if slip:
            b = np.concatenate([b[:len(b) // 2], b[len(b) // 2 - 1:-1]])
        if not tx2:
            b = 0 * b
        z = a + b + 0.01 * np.conj(a) + 0.006 * np.conj(b) + 0.0004
        st = int(rng.integers(0, len(sx1))) if start is None else start
        z = z[st:st + ncap] * np.exp(2j * np.pi * seps * np.arange(ncap) / fs)
        sig2 = (0.5 * 0.02) ** 2 * 2 ** 18 / (1.73 * 10 ** (snr / 10))
        return z + noise(ncap, 10 * np.log10(sig2))

    s0 = analyze_sync(sync_cap(), sm)
    check(s0["lag_samples"] == 0 and abs(s0["lag_est_samples"]) < 0.01 and abs(s0["phase_at_fc_deg"] - 40) < 0.5
          and s0["cycles"] >= SYNC_CYCLES and s0["fit_lines"] == 29 and s0["cycle_phase_spread_deg"] < 1
          and abs(s0["marker_lag_samples"]) < 2 and abs(s0["freq_err_hz"] - seps) < 50,
          "lag %+.4f, phase %.2f deg (40), %d cycles, %d lines, spread %.2f deg, markers' lag %+.2f, offset %.0f Hz"
          % (s0["lag_est_samples"], s0["phase_at_fc_deg"], s0["cycles"], s0["fit_lines"],
             s0["cycle_phase_spread_deg"], s0["marker_lag_samples"], s0["freq_err_hz"]))
    s1 = analyze_sync(sync_cap(lag=0.17, dphi=180.0, g2_db=0.0, snr=52.0), sm)
    check(abs(s1["lag_est_samples"] - 0.17) < 0.02 and abs(wrap_deg(s1["phase_at_fc_deg"] - 180)) < 1.5 and
          s1["cycle_phase_spread_deg"] < PHASE_SPREAD_MAX_DEG,
          "TX2 opposite TX1 (one block cancels), 0.17 samples late, 52 dB: lag %+.3f, phase %.1f deg, spread %.1f deg"
          % (s1["lag_est_samples"], s1["phase_at_fc_deg"], s1["cycle_phase_spread_deg"]))
    s2 = analyze_sync(sync_cap(lag=37.0), sm)
    check(s2["lag_samples"] == 37 and abs(s2["marker_lag_samples"] - 37) < 3,
          "37 samples late (the lines alone would say -4): %d, markers %+.1f"
          % (s2["lag_samples"], s2["marker_lag_samples"]))
    try:
        analyze_sync(noise(ncap, -60), sm)
        check(False, "no marker raises (retried)")
    except RuntimeError:
        check(True, "no marker raises (retried)")
    sync_caps = lambda **kw: {name: [sync_cap(**kw)] for name, _ in SYNC_RUNS}
    m = analyze_tx(sm, sync_caps())
    check(m["pass"] and m["runs"] == [n for n, _ in SYNC_RUNS] and m["sync"]["class"] == "stable",
          "sync runs alone pass, phases stable %s" % m["fails"])
    m = analyze_tx(sm, sync_caps(lag=1.0))
    check(has(m, "TX2 is 1 samples behind TX1"), "TX2 one sample late fails: %s" % m["fails"][:1])
    m = analyze_tx(sm, sync_caps(lag=-0.4))
    check(has(m, "TX2 lags TX1 by -0.4"), "TX2 0.4 samples early fails: %s" % m["fails"][:1])
    m = analyze_tx(sm, sync_caps(slip=True))
    check(has(m, "TX2's lag moved"), "a sample added on TX2 fails: %s" % m["fails"][:1])
    m = analyze_tx(sm, sync_caps(tx2=False))
    check(has(m, "TX2's marker is not there") and has(m, "lines"), "no TX2 fails: %s" % m["fails"][:2])
    caps = sync_caps()
    caps["sync1"] = [sync_cap(dphi=220.0)]
    m = analyze_tx(sm, caps)
    check(m["pass"] and m["sync"]["class"] == "flips by 180 deg", "a 180 degree flip after a retune: reported, pass")
    m = analyze_tx(sm, dict(good, **sync_caps()))
    check(m["pass"] and "single" in m and "sync" in m, "tone and sync runs together pass")

    print("--save / --analyze")
    with tempfile.TemporaryDirectory() as d:
        global SAVE_DIR
        saved, SAVE_DIR = SAVE_DIR, d
        try:
            sm = dict(meta, skip_n=1000)
            sm["caps"] = {k: dict(v, n=nn + 1000) for k, v in meta["caps"].items()}
            for i in range(RETUNES):
                sm["caps"]["retune%d" % i]["n"] = 4 * BLOCK + 1000
            sm["caps"]["bb"]["n"] = 16 * CHIRP_N + 1000
            caps = rx_caps()
            files = {}
            for name, chans in caps.items():
                # a wild start (as a capture's first samples can be) that must be skipped
                junk = [noise(1000, 0) for _ in chans]
                full = [np.concatenate([j, c]) for j, c in zip(junk, chans)]
                files[name] = os.path.join(d, "src-%s.bin" % name)
                interleave_sc16([rf.to_sc16q11(np.clip(c.real, -0.99, 0.99) + 1j * np.clip(c.imag, -0.99, 0.99))
                                 for c in full]).astype("<i2").tofile(files[name])
            save_point(sm, files, {})
            saved_tx = dict(good, **sync_caps())
            save_point(dict(tm, caps={k: tm["caps"][k] for k in saved_tx}), {},
                       {k: v[0] for k, v in saved_tx.items()})
            for f in files.values():
                os.remove(f)
            recs = analyze_dir(d)
            check(len(recs) == 2 and all(r["pass"] for r in recs) and
                  any(len(r.get("sync", {}).get("runs", {})) == len(SYNC_RUNS) for r in recs),
                  "both saved points (tx with its sync runs) analyzed and pass %s" % [r.get("fails") for r in recs])
            try:
                back = [json.loads(json.dumps(r, default=_json_default, allow_nan=False)) for r in recs]
                check(back[0]["pass"] is True and isinstance(back[0]["fc"], float),
                      "records are plain JSON (no numpy types, no NaN)")
            except (TypeError, ValueError) as e:
                check(False, "records are plain JSON: %s" % e)
            direct = analyze_rx(sm, load_rx_caps(sm, lambda k: os.path.join(
                d, json.load(open(glob.glob(os.path.join(d, "twoch-rx-*.json"))[0]))["files"][k])))
            check(abs(direct["phase_diff_deg"] - recs[0 if recs[0]["test"] == "twoch_rx" else 1]["phase_diff_deg"]) < 1e-9,
                  "same numbers as analyzed directly")
        finally:
            SAVE_DIR = saved

    print("safety and commands")
    b200_gains = sorted(set(b200_tx_gain(p[0]) for p in TWOCH_POINTS))
    check(max(b200_gains) <= rf.B200_TX_GAIN_MAX and b200_tx_gain(5800e6) == 60 and b200_tx_gain(433e6) == B200_TX_GAIN,
          "B200mini TX gains %s <= %g" % (b200_gains, rf.B200_TX_GAIN_MAX))
    check(TX_GAIN == rf.BLADERF_TX_GAIN == 30 and max(r[3] for r in TX_RUNS) <= TX_GAIN_MAX and
          max(r[4] for r in TX_RUNS) <= TX_GAIN_MAX, "bladeRF TX gains %s <= %d"
          % (sorted(set(r[3] for r in TX_RUNS) | set(r[4] for r in TX_RUNS)), TX_GAIN_MAX))
    try:
        tx_cmds(tm, "x.bin", [1], 61, 30)
        check(False, "TX gain above 60 refused")
    except ValueError:
        check(True, "TX gain above 60 refused")
    lists = [rx_tone_cmds(meta, lambda k: "/dev/shm/x/%s.bin" % k), rx_chirp_cmds(meta, lambda k: "/dev/shm/x/bb.bin")]
    lists += [tx_cmds(tm, "/dev/shm/x/tx.bin", ch, g1, g2) for _, _, ch, g1, g2 in TX_RUNS]
    lists += [tx_cmds(tm, "/dev/shm/x/tx-sync.bin", [1, 2], TX_GAIN, TX_GAIN,
                      None if aw is None else tm["retune_away"][aw]) for _, aw in SYNC_RUNS]
    check(any(c.startswith("set frequency tx2 %d" % tm["retune_away"][1]) for c in lists[-1]) and
          lists[-1].index("set frequency tx2 %d" % tm["retune_away"][1]) <
          lists[-1].index("set frequency tx2 %d" % tm["fc"]), "sync2 tunes away first, then to the carrier")
    words = re.compile(r"flash|fx3|jump|erase|recover|poke|calibrate|biastee|trimdac|^run\b|^load", re.I)
    check(not any(words.search(c) for cl in lists for c in cl), "no flash/FX3/bootloader/bias-tee commands")
    check(all(";" not in c for cl in lists for c in cl), "no ';' in any command")
    env = dict(os.environ)
    env.pop("HWTEST_TX", None)
    p = subprocess.run([sys.executable, os.path.abspath(__file__), "rx"], capture_output=True, text=True, env=env)
    check(p.returncode != 0 and "HWTEST_TX=1" in p.stderr, "refuses to run without HWTEST_TX=1")
    bash = shutil.which("bash", path="/bin:/usr/bin")
    if bash is None:
        print("  skip bladerf-cli-safe (no bash)")
    else:
        with tempfile.TemporaryDirectory() as d:
            fake = os.path.join(d, "output", "bladeRF-cli")
            os.makedirs(os.path.dirname(fake))
            with open(fake, "w") as f:
                f.write("#!/bin/sh\necho FAKE-BLADERF-CLI-OK\n")
            os.chmod(fake, 0o755)
            # bladerf-cli-safe runs "$HOST_BUILD/output/bladeRF-cli" (an absolute
            # path): with HOST_BUILD here that is the fake, never a real one
            env = {"PATH": "/usr/bin:/bin", "HOME": d, "HOST_BUILD": d, "HWTEST_TX": "1"}

            def safe(cmds):
                args = [bash, rf.CLI]
                for c in cmds:
                    args += ["-e", c]
                return subprocess.run(args, capture_output=True, text=True, env=env, timeout=60)

            ok = [safe(cl) for cl in lists]
            check(all(q.returncode == 0 and "FAKE-BLADERF-CLI-OK" in q.stdout for q in ok),
                  "every command list passes bin/bladerf-cli-safe %s"
                  % [q.stderr.strip()[-200:] for q in ok if q.returncode != 0])
            q = safe(["flash_image x"])
            check(q.returncode == 97 and "FAKE" not in q.stdout, "bin/bladerf-cli-safe still refuses flash_image")
    print("self-test: %s" % ("PASS" if not bad else "%d FAILED" % len(bad)))
    return len(bad)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", nargs="?", choices=["rx", "tx"])
    ap.add_argument("--out", default="-")
    ap.add_argument("--points", type=int, default=len(TWOCH_POINTS),
                    help="only the first N frequency points")
    ap.add_argument("--save", help="directory for the raw captures and a manifest per point")
    ap.add_argument("--analyze", metavar="DIR", help="re-analyze what --save kept in DIR (transmits nothing)")
    ap.add_argument("--self-test", action="store_true", help="check the analysis on synthetic signals")
    a = ap.parse_args()
    if a.self_test:
        return 1 if self_test() else 0
    out = sys.stdout if a.out == "-" else open(a.out, "a")
    if a.analyze:
        recs = analyze_dir(a.analyze)
        if not recs:
            sys.exit("no twoch-check.py manifests in %s" % a.analyze)
        for m in recs:
            write_record(out, m)
        return 1 if any(not m.get("pass") for m in recs) else 0
    if not a.mode:
        ap.error("rx, tx, --analyze or --self-test is required")
    sync_only()     # refuses a value it does not know
    if os.environ.get("HWTEST_TX") != "1":
        # rx makes the B200mini transmit, tx the bladeRF
        sys.exit("twoch-check.py transmits: set HWTEST_TX=1 only with the two-channel "
                 "cabling in README.md (attenuators, no antennas)")
    global SAVE_DIR
    SAVE_DIR = a.save
    u = rf.usrp()
    fn = check_rx if a.mode == "rx" else check_tx
    fails = 0
    for point in TWOCH_POINTS[:a.points]:
        errors = []
        for _ in range(TRIES):      # retry captures that dropped samples
            try:
                m = fn(u, point)
                break
            except Exception as e:  # keep going; record the failure
                errors.append(str(e))
        else:
            m = {"test": "twoch_" + a.mode, "fc": point[0], "fs": point[1],
                 "pass": False, "fails": errors[-1:], "error": errors[-1]}
        m["retries"] = len(errors) if "error" not in m else TRIES - 1
        if errors:
            m["retry_errors"] = errors
        m["time"] = time.time()
        m["tuning_mode"] = os.environ.get("BLADERF_DEFAULT_TUNING_MODE", "")
        if not m.get("pass"):
            fails += 1
        write_record(out, m)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

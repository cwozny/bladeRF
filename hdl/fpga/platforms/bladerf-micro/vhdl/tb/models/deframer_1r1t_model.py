#!/usr/bin/env python3
"""Cycle model of the receive decode in
hdl/fpga/ip/analogdevicesinc/hdl/library/axi_ad9361/axi_ad9361_dev_if_alt.v.

Word stream: each sample is four 6-bit words in time order I msb, Q msb, I lsb,
Q lsb (as the 2R2T cases imply). 2R2T: ch1 sample (frame high) then ch2 sample
(frame low). R1: frame high on the two msb words. The stream is cut into l_clk
cycles of four words at each rotation; word 0 of a cycle is rx_data_0 and
rx_frame[0]. The RTL registers the frame and data (rx_frame, rx_data_N) and
uses the next cycle's words (rx_data_N_s) as lookahead.
"""
import random

def words_of(sample):
    i, q = sample
    return [(i >> 6) & 63, (q >> 6) & 63, i & 63, q & 63]

def stream(r1, n):
    rnd = random.Random(1)
    samples, words, frame = [], [], []
    for k in range(n):
        if r1:
            s = (rnd.randrange(4096), rnd.randrange(4096))
            samples.append(('ch1', s))
            words += words_of(s)
            frame += [1, 1, 0, 0]
        else:
            s1 = (rnd.randrange(4096), rnd.randrange(4096))
            s2 = (rnd.randrange(4096), rnd.randrange(4096))
            samples.append(('pair', s1, s2))
            words += words_of(s1) + words_of(s2)
            frame += [1, 1, 1, 1, 0, 0, 0, 0]
    return samples, words, frame

def decode(r1_mode, words, frame, rot):
    w, f = words[rot:], frame[rot:]
    ncyc = len(w) // 4
    cyc = [(w[4*c:4*c+4], f[4*c:4*c+4]) for c in range(ncyc)]
    out = []
    adc_lo = 0
    for c in range(ncyc - 1):
        d, fr = cyc[c]          # registered: rx_data_N, rx_frame
        ds, _ = cyc[c + 1]      # lookahead: rx_data_N_s
        pat = ''.join(str(fr[b]) for b in (3, 2, 1, 0))   # rx_frame[3:0]
        key = ('1' if r1_mode else '0') + pat
        def m(qh, ql, ih, il):
            return ((ih << 6) | il, (qh << 6) | ql)
        d0, d1, d2, d3 = d
        s0, s1, s2, s3 = ds
        table = {
            # R1 cases
            '10011': (1, m(d1, d3, d0, d2)),
            '10110': (1, m(d2, s0, d1, d3)),
            '11100': (1, m(d3, s1, d2, s0)),
            '11001': (1, m(s0, s2, d3, s1)),
            # 2R2T cases (unchanged from Nuand's code)
            '01111': (1, m(d1, d3, d0, d2)),
            '01110': (1, m(d2, s0, d1, d3)),
            '01100': (1, m(d3, s1, d2, s0)),
            '01000': (1, m(s0, s2, d3, s1)),
            '00000': (0, m(d1, d3, d0, d2)),
            '00001': (0, m(d2, s0, d1, d3)),
            '00011': (0, m(d3, s1, d2, s0)),
            '00111': (0, m(s0, s2, d3, s1)),
        }
        valid_r2, data_r2 = table.get(key, (0, (0, 0)))
        if key not in table:
            data_r2 = None
        if r1_mode:
            if valid_r2:
                out.append(('ch1', data_r2))
        else:
            if valid_r2:
                adc_lo = data_r2
            else:
                out.append(('pair', adc_lo, data_r2))
    return out

for r1 in (False, True):
    samples, words, frame = stream(r1, 400)
    for rot in range(4 if r1 else 8):
        out = decode(r1, words, frame, rot)
        # find where the decoded stream lines up with the sent samples
        ok = False
        for off in range(4):
            seg = out[off:off + 300]
            for start in range(len(samples) - 300):
                if seg == samples[start:start + 300]:
                    ok = True
                    break
            if ok:
                break
        print('%s rotation %d: %s' % ('R1  ' if r1 else '2R2T', rot,
                                       'decodes every sample' if ok else 'MISMATCH'))

#!/usr/bin/env python3
"""Cycle model of fifo_writer.vhd (meta FSM + sample FIFO FSM), one channel enabled (x1),
NUM_STREAMS = 2, meta_en = 1, packet_en = 0, fifo_enough always true, no FIFO full.
Samples arrive every clock (1R1T) or every other clock (2R2T). The host pairs the k-th meta
word with the k-th group of (BUF - 4) DWORDs of samples. Prints, per buffer, the meta
timestamp minus the timestamp of the first sample of its group (0 = consistent)."""
import sys

def run(period, eight_bit, buf=2048, nbuf=6, b2b=False):
    # timestamp(t) = t: the tamer counts samples; with period 2 it counts every other clock
    st = dict(state='CLEAR', fifo_write=0, samples_left=0, ebd=0, in_r=None, data=[])
    mt = dict(state='IDLE', down=0, meta_write=0, written=0, data=None)
    words, metas = [], []
    per_word = 4 if eight_bit else 2
    sample = 0
    t = 0
    while len(metas) < nbuf + 1 and t < 10**6:
        valid = (t % period) == 0
        ins = (sample, t // period) if valid else None   # (index, timestamp at arrival)
        if valid:
            sample += 1
        # ---- sample FIFO FSM (combinational)
        fu = dict(st)
        fu['fifo_write'] = 0
        fu['in_r'] = ins
        if st['in_r'] is not None:              # pack every cycle
            fu['data'] = (st['data'] + [st['in_r'][0]])[-per_word:]
        def handle(fu):
            if mt['written'] == 1:
                wr = st['in_r'] is not None
                if wr:
                    if st['samples_left'] == 0:
                        fu['samples_left'] = 1
                        fu['fifo_write'] = 1
                        if eight_bit:
                            fu['fifo_write'] = 1 if st['ebd'] else 0
                            fu['ebd'] = 1 - st['ebd']
                    else:
                        fu['samples_left'] = st['samples_left'] - 1
            else:
                fu['fifo_write'] = 0
        if st['state'] == 'CLEAR':
            fu['samples_left'] = 1
            fu['state'] = 'WRITE_SAMPLES'
        elif st['state'] == 'WRITE_SAMPLES':
            handle(fu)
            if mt['written'] == 0:
                fu['fifo_write'] = 0
                fu['state'] = 'HOLDOFF'
        elif st['state'] == 'HOLDOFF':
            fu['state'] = 'WRITE_SAMPLES'
            handle(fu)
        # ---- meta FSM (combinational)
        mf = dict(mt)
        mf['meta_write'] = 0
        mf['data'] = t // period                  # timestamp now
        if mt['state'] == 'IDLE':
            mf['down'] = buf - 4
            mf['state'] = 'META_WRITE'
        elif mt['state'] == 'META_WRITE':
            if ins is not None:
                mf['meta_write'] = 1
                mf['written'] = 1
                mf['state'] = 'META_DOWNCOUNT'
        elif mt['state'] == 'META_DOWNCOUNT' and b2b:
            # proposed back-to-back branch: count each word when it is decided; the
            # decision of a buffer's last word writes the next buffer's meta at once,
            # with the timestamp of the sample that arrives in that cycle
            if fu['fifo_write'] == 1:
                if mt['down'] <= 2:
                    mf['down'] = buf - 4
                    mf['meta_write'] = 1
                else:
                    mf['down'] = mt['down'] - 2
        elif mt['state'] == 'META_DOWNCOUNT':
            if st['fifo_write'] == 1 and mt['meta_write'] == 0:
                mf['down'] = mt['down'] - 2
            if mt['down'] <= 2:
                if eight_bit:
                    if fu['ebd'] == 1 and fu['samples_left'] == 0:
                        mf['state'] = 'IDLE'
                else:
                    mf['state'] = 'IDLE'
        # ---- outputs of the current state
        if st['fifo_write']:
            words.append(list(st['data']))
        if mt['meta_write']:
            metas.append(mt['data'])
        st, mt = fu, mf
        t += 1
    # the host: buffer k = metas[k] + words[k*G:(k+1)*G], G = (buf-4)/2 words
    g = (buf - 4) // 2
    out = []
    for k in range(min(nbuf, len(metas), len(words) // g)):
        first = words[k * g][0]                   # first sample index of the group
        out.append(metas[k] - first)              # timestamp minus sample index
    return out

for b2b in (False, True):
    print('back_to_back_en = %d' % b2b)
    for period, name in ((1, '1R1T (every clock)'), (2, '2R2T (every other clock)')):
        if b2b and period == 2:
            continue
        for eb in (True, False):
            print('  %-26s %s: meta timestamp - first sample of its group, per buffer: %s' % (
                name, 'SC8 ' if eb else 'SC16', run(period, eb, nbuf=8, b2b=b2b)))

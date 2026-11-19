#!/usr/bin/env python3
"""Cycle model of the TX packet path of the bladeRF wlan revision:
fifo_reader.vhd (packet mode, NUM_STREAMS=2, 64-bit show-ahead sample FIFO, 128-bit meta FIFO)
feeding wlan_top.vhd's TX FSM (WAIT_FOR_SOP .. READ_PAYLOAD) which pulls dwords with
tx_packet_ready = fifo_read or ready_for_packet.  Purpose: check how many dwords the reader
emits per packet and whether the next packet's header is read aligned.
Two-process style: every register's next value is computed from the current values only."""
import copy, sys

MAXTS = (1 << 64) - 1

class Meta:  # fifo_reader meta FSM registers
    def __init__(s):
        s.state = 'LOAD'; s.dma = 0; s.sop = 0; s.eop = 0; s.skip = 0; s.read = 0
        s.cache = None; s.p_time = 0; s.p_time_r = 0; s.go = 0; s.empty = 1; s.data = None

class Fifo:  # fifo_reader sample FSM registers (READ_PACKET only)
    def __init__(s):
        s.sl = 1; s.data = 0; s.cache = 0; s.valid = 0; s.sop = 0; s.eop = 0; s.rd = 0

class Tx:  # wlan_top TX FSM registers
    def __init__(s):
        s.fsm = 'IDLE'; s.timer = 0; s.ready = 0; s.rd = 0; s.read_words = 0; s.header = []
        s.tx_word = 0; s.byte_index = 0; s.read_bytes = 0; s.vec_valid = 0

def run(packets, byte_period, start_delay=20, done_delay=50, max_cycles=200000, verbose=False, dmaoff=1, extra_read=0):
    # host side: sample FIFO of 64-bit words (lo, hi), meta FIFO of (len, ts)
    sfifo = []
    mfifo = []
    for p in packets:
        d = list(p)
        if len(d) % 2:
            d.append(0xDEADBEEF)          # GPIF writes len+1 dwords for odd len (fx3_gpif.vhd:562-563)
        for i in range(0, len(d), 2):
            sfifo.append((d[i], d[i + 1]))
        mfifo.append((len(p), 0))         # ts 0 = now
    mc, fc, tc = Meta(), Fifo(), Tx()
    got = []                               # per packet: (header dwords, payload bytes)
    cur = None
    wlan = None                            # wlan_tx byte reader model: dict(next, left, done_at)
    for cyc in range(max_cycles):
        fifo_empty = 0 if sfifo else 1
        fifo_data = sfifo[0] if sfifo else (0, 0)
        meta_empty = 0 if mfifo else 1
        meta_data = mfifo[0] if mfifo else (0, 0)
        packet_ready = 1 if (tc.rd or tc.ready) else 0
        packet_empty = 1 if (mc.empty == 1 and mc.state != 'WAIT') else 0
        # wlan_tx model: fifo_re and done
        fifo_re = 0; done = 0
        if wlan is not None:
            if wlan['left'] > 0 and cyc >= wlan['next']:
                fifo_re = 1
            if wlan['left'] == 0 and cyc >= wlan['done_at']:
                done = 1
        # ---------------- meta FSM comb ----------------
        mf = copy.copy(mc)
        mf.read = 0; mf.sop = 0; mf.eop = 0
        mf.empty = meta_empty; mf.data = meta_data
        mt = ((mc.data[1] if mc.data else 0) - 1) & MAXTS
        mf.p_time_r = mt
        if mc.state == 'LOAD':
            mf.skip = 0; mf.p_time = mc.p_time_r; mf.cache = mc.data
            if mc.dma == 2: mf.dma = 0
            if mc.empty == 0 and packet_ready:
                mf.read = 1; mf.state = 'WAIT'; mf.go = 0
            else:
                mf.go = 0
        elif mc.state == 'WAIT':
            mf.dma = mc.cache[0] - dmaoff
            if (mc.p_time == MAXTS) and packet_ready:
                mf.go = 1; mf.state = 'DOWN'; mf.sop = 1
            else:
                mf.go = 0
        elif mc.state == 'DOWN':
            mf.go = 1
            if fc.valid: mf.dma = mc.dma - 1
            if mc.dma <= 1 and packet_ready:
                mf.state = 'LOAD'; mf.eop = 1
            if mc.cache[0] & 1: mf.skip = 1
        # ---------------- sample FSM comb (READ_PACKET) ----------------
        ff = copy.copy(fc)
        ff.rd = 0; ff.sop = mc.sop; ff.valid = 0
        if mc.eop and mc.skip: ff.sl = 1
        if fc.sl == 0: ff.data = fc.cache
        else: ff.data = fifo_data[0]; ff.cache = fifo_data[1]
        ff.eop = 0
        if mc.go and mc.dma > 0 and packet_ready:
            if mc.dma == 1: ff.eop = 1
            ff.valid = 1
            if fc.sl == 1: ff.rd = 1
            ff.sl = 1 if fc.sl == 0 else fc.sl - 1
        # ---------------- wlan_top TX FSM comb ----------------
        tf = copy.copy(tc); tf.header = list(tc.header)
        tf.rd = 0; tf.vec_valid = 0; tf.ready = 0
        pc_sop, pc_valid, pc_data = fc.sop, fc.valid, fc.data
        if tc.fsm == 'IDLE':
            if packet_empty == 0:
                tf.timer = 400; tf.fsm = 'WAIT_TO_TX'; tf.read_words = 0
            else:
                tf = Tx()
        elif tc.fsm == 'WAIT_TO_TX':
            if tc.timer == 0:
                tf.ready = 1; tf.fsm = 'WAIT_FOR_SOP'
            else:
                tf.timer = tc.timer - 1
        elif tc.fsm == 'WAIT_FOR_SOP':
            if pc_sop:
                tf.header = [pc_data]; tf.read_words = tc.read_words + 1; tf.ready = 0; tf.fsm = 'READ_HEADER'
            else:
                tf.ready = 1
        elif tc.fsm == 'READ_HEADER':
            tf.rd = 1 if tc.read_words <= 2 else 0
            if pc_valid:
                tf.header = tc.header + [pc_data]; tf.read_words = tc.read_words + 1
                if tc.read_words == 2: tf.rd = 0
            if tc.read_words >= 3: tf.fsm = 'READ_VECTOR'
        elif tc.fsm == 'READ_VECTOR':
            tf.fsm = 'VALID_VECTOR'
        elif tc.fsm == 'VALID_VECTOR':
            tf.tx_word = pc_data; tf.rd = 1; tf.read_words = tc.read_words + 1
            tf.vec_valid = 1; tf.fsm = 'READ_PAYLOAD'; tf.read_bytes = 1
            hdr = tc.header[-4:] if len(tc.header) >= 4 else tc.header
            L = (hdr[2] & 0xFFFF) if len(hdr) >= 3 else 0
            cur = {'hdr': list(tc.header), 'bytes': []}
            got.append(cur)
            wlan = {'next': cyc + start_delay, 'left': L + extra_read, 'done_at': cyc + start_delay + done_delay}
        elif tc.fsm == 'READ_PAYLOAD':
            if fifo_re:
                tf.read_bytes = tc.read_bytes + 1
                cur['bytes'].append((tc.tx_word >> (8 * tc.byte_index)) & 0xFF) if len(cur['bytes']) < (tc.header[2] & 0xFFFF) else None
                if tc.byte_index == 3:
                    tf.rd = 1; tf.read_words = tc.read_words + 1; tf.tx_word = pc_data; tf.byte_index = 0
                else:
                    tf.byte_index = tc.byte_index + 1
            if done:
                tf = Tx(); wlan = None
        # ---------------- clock edge ----------------
        if fifo_re and wlan is not None:
            wlan['left'] -= 1; wlan['next'] = cyc + byte_period
            if wlan['left'] == 0: wlan['done_at'] = cyc + done_delay
        if fc.rd and sfifo: sfifo.pop(0)
        if mc.read and mfifo: mfifo.pop(0)
        if verbose and (fc.valid or packet_ready):
            print(cyc, mc.state, 'dma', mc.dma, 'ready', packet_ready, 'valid', fc.valid, 'sop', fc.sop,
                  'eop', fc.eop, 'data %08x' % fc.data, tc.fsm)
        mc, fc, tc = mf, ff, tf
        if not mfifo and tc.fsm == 'IDLE' and len(got) == len(packets) and wlan is None:
            break
    return got, sfifo, mfifo

def wiphy_packet(L, mod, cookie, extra=0):
    frame = bytes((0x08, 0x00, 0, 0) + (0xff,) * 6 + tuple((i * 7 + 3) & 0xff for i in range(L - 10)))
    hdr = [0, (2 << 16) | mod, L, cookie]
    body = frame + b'\0' * ((-len(frame)) % 4)
    dw = [int.from_bytes(body[i:i + 4], 'little') for i in range(0, len(body), 4)]
    return hdr + dw + [0x5A5A0000 | i for i in range(extra)], frame

if __name__ == '__main__':
    for dmaoff, name in ((0, 'pre-950189d2 (dma_downcount = len)'), (1, 'current (dma_downcount = len - 1)')):
        for extra_read in (0, 1):
            res = {}
            for extra in (0, 1, 2, 3):
                bad = 0; tot = 0
                for L in list(range(10, 80)) + [100, 101, 102, 103, 500, 1500]:
                    pk = [wiphy_packet(L, 0, 0x1000 + k, extra) for k in range(3)]
                    got, s, m = run([p for p, _ in pk], 3, dmaoff=dmaoff, extra_read=extra_read)
                    for k, (p, frame) in enumerate(pk):
                        tot += 1
                        if not (k < len(got) and got[k]['hdr'] == p[:4] and bytes(got[k]['bytes']) == frame):
                            bad += 1
                res[extra] = '%d/%d bad' % (bad, tot)
            print('%-36s framer reads L+%d bytes: ' % (name, extra_read) + '; '.join('pad %d dw: %s' % kv for kv in res.items()))

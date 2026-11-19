import sys
sys.argv = ['x']
exec(open('txmodel2.py').read().split("if __name__ == '__main__':")[0])
def show(L, n, dmaoff):
    pk = [wiphy_packet(L, 0, 0x1000 + k, 0) for k in range(n)]
    got, s, m = run([p for p, _ in pk], 3, dmaoff=dmaoff)
    out = []
    for k, g in enumerate(got):
        h = g['hdr'][-4:] if len(g['hdr']) >= 4 else g['hdr'] + [0] * (4 - len(g['hdr']))
        ok = k < len(pk) and g['hdr'] == pk[k][0][:4] and bytes(g['bytes']) == pk[k][1]
        out.append('%s(len=%d rate=%d bw=%d cookie=%08x)' % ('ok' if ok else 'BAD', h[2] & 0xffff, h[1] & 0xffff, h[1] >> 16, h[3]))
    print('dma_downcount=len-%d L=%d: %d packets queued, core started %d: %s; sample FIFO words left %d, metas left %d' % (dmaoff, L, n, len(got), ', '.join(out), len(s), len(m)))
for L in (100, 101):
    show(L, 6, 1)
show(100, 6, 0)

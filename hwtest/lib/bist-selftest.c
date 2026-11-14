/*
 * Self-test of hwtest-stream's bist checker on synthetic data (no hardware;
 * built and run by bin/build-host.sh): learning a period, voting it, single
 * bit errors, a lost stretch of samples, finding the phase in a stream with
 * 30 % bad samples, garbage, a tone, constant samples.
 */
#define main hwtest_stream_main
#include "hwtest-stream.c"
#undef main

static uint32_t lfsr = 1;
static uint32_t prbs_bit(void)          /* PRBS15: x^15 + x^14 + 1 */
{
    uint32_t b = ((lfsr >> 14) ^ (lfsr >> 13)) & 1;
    lfsr = ((lfsr << 1) | b) & 0x7fff;
    return b;
}

static uint32_t *make_pattern(uint64_t n)
{
    uint32_t *x = malloc(n * sizeof(*x));
    for (uint64_t j = 0; j < n; j++) {
        uint32_t v = 0;
        for (int b = 0; b < 24; b++) {
            v = v << 1 | prbs_bit();
        }
        /* 12-bit I and Q, sign-extended to 16 bits as SC16 delivers them */
        uint32_t i = v & 0xfff, q = (v >> 12) & 0xfff;
        i |= (i & 0x800) ? 0xf000 : 0;
        q |= (q & 0x800) ? 0xf000 : 0;
        x[j] = i | q << 16;
    }
    return x;
}

static int fails;
#define EXPECT(c) do { if (!(c)) { printf("EXPECT FAILED line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    const uint64_t P = 32767;
    uint32_t *pat = make_pattern(P);
    struct bist_slot s;
    uint64_t j, pos = 0, start;

    /* 1. learn from a clean stream with an arbitrary phase */
    memset(&s, 0, sizeof(s));
    s.acc = malloc(BIST_LEARN * sizeof(uint32_t));
    start = 12345;
    for (j = 0; j < BIST_LEARN; j++) {
        s.acc[s.nacc++] = pat[(start + j) % P];
    }
    bist_learn(&s, 0);
    EXPECT(!s.failed);
    EXPECT(s.period == P);
    EXPECT(s.bad == 0);
    EXPECT(s.checked == BIST_LEARN);
    EXPECT(bist_index(&s) == 0);

    /* 2. checking pass: new phase, 3 single-bit errors, one dropped stretch */
    start = 777;
    uint64_t k = start;
    for (j = 0; j < 4000000; j++, k++) {
        uint32_t v = pat[k % P];
        if (j == 100000) v ^= 1u << 3;          /* I bit 3 */
        if (j == 200000) v ^= 1u << (16 + 11);  /* Q bit 11 */
        if (j == 300000) v ^= 1u << 0;
        if (j == 2000000) k += 1000;            /* lost 1000 samples */
        bist_feed(&s, v, 0, pos++);
        if (j == 1000000) {
            EXPECT(s.bad == 3 && s.ibit[3] == 1 && s.qbit[11] == 1 &&
                   s.ibit[0] == 1);
        }
    }
    printf("after pass 2: checked %llu bad %llu resyncs %llu unchecked %llu\n",
           (unsigned long long)s.checked, (unsigned long long)s.bad,
           (unsigned long long)s.resyncs, (unsigned long long)s.unchecked);
    EXPECT(s.resyncs == 1);
    EXPECT(s.bad == 3 + BIST_LOST);
    EXPECT(s.unchecked == 0);

    /* 3. a golden-file style run on a stream with 30 % bad samples */
    struct bist_slot g;
    memset(&g, 0, sizeof(g));
    g.period = P;
    g.gold = malloc(P * sizeof(uint32_t));
    memcpy(g.gold, pat, P * sizeof(uint32_t));
    g.acc = malloc(BIST_ALIGN * sizeof(uint32_t));
    EXPECT(bist_index(&g) == 0);
    srand(1);
    uint64_t injected = 0;
    k = 31000;
    for (j = 0; j < 2000000; j++, k++) {
        uint32_t v = pat[k % P];
        if (rand() % 10 < 3) {
            v ^= 1u << (rand() % 8 + 4);        /* I bits 4..11 */
            injected++;
        }
        bist_feed(&g, v, 0, j);
    }
    printf("pass 3: checked %llu bad %llu (injected %llu) resyncs %llu unchecked %llu\n",
           (unsigned long long)g.checked, (unsigned long long)g.bad,
           (unsigned long long)injected, (unsigned long long)g.resyncs,
           (unsigned long long)g.unchecked);
    EXPECT(g.resyncs == 0);
    EXPECT(g.checked == 2000000);
    EXPECT(g.bad == injected);

    /* 4. garbage never aligns */
    struct bist_slot z;
    memset(&z, 0, sizeof(z));
    z.period = P;
    z.gold = g.gold;
    z.ix = g.ix;
    z.acc = malloc(BIST_ALIGN * sizeof(uint32_t));
    for (j = 0; j < 100000; j++) {
        bist_feed(&z, (uint32_t)rand(), 0, j);
    }
    EXPECT(z.checked == 0);
    EXPECT(z.unchecked > 90000);

    /* 5. a tone-like pattern with period 32 */
    struct bist_slot t;
    memset(&t, 0, sizeof(t));
    t.acc = malloc(BIST_LEARN * sizeof(uint32_t));
    for (j = 0; j < BIST_LEARN; j++) {
        double ph = 2 * M_PI * (double)(j % 32) / 32;
        uint32_t i = (uint32_t)(int32_t)lround(2047 * cos(ph)) & 0xffff;
        uint32_t q = (uint32_t)(int32_t)lround(2047 * sin(ph)) & 0xffff;
        t.acc[t.nacc++] = i | q << 16;
    }
    bist_learn(&t, 0);
    EXPECT(t.period == 32 && t.bad == 0 && !t.failed);

    /* 6. constant data fails */
    struct bist_slot c;
    memset(&c, 0, sizeof(c));
    c.acc = calloc(BIST_LEARN, sizeof(uint32_t));
    c.nacc = BIST_LEARN;
    bist_learn(&c, 0);
    EXPECT(c.failed);

    printf("%s (%d failures)\n", fails ? "SELFTEST FAILED" : "SELFTEST OK", fails);
    return fails != 0;
}

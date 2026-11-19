#include "inflate.h"

#include <stdlib.h>
#include <string.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288
#define WIN_SIZE 65536u /* >= 32 KB history + headroom; power of two */
#define WIN_MASK (WIN_SIZE - 1)
#define FLUSH_AT 32768u

typedef struct {
    const uint8_t *in;
    uint32_t in_len, in_pos;
    uint32_t bitbuf;
    int bitcnt;
    int err;
    uint8_t *win;
    uint64_t pos, flushed;
    inflate_sink sink;
    void *ctx;
    int stopped;
} St;

typedef struct {
    int16_t count[MAXBITS + 1];
    int16_t symbol[FIXLCODES];
} Huff;

static int bits(St *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) {
            s->err = 1;
            return 0;
        }
        val |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

/* Hands everything not yet flushed to the sink (in at most two pieces of the ring). */
static void flush(St *s) {
    while (!s->stopped && s->flushed < s->pos) {
        uint32_t start = (uint32_t)(s->flushed & WIN_MASK);
        uint32_t n = (uint32_t)(s->pos - s->flushed);
        if (n > WIN_SIZE - start) n = WIN_SIZE - start;
        if (s->sink(s->ctx, s->flushed, s->win + start, n)) s->stopped = 1;
        s->flushed += n;
    }
}

static inline void put(St *s, uint8_t b) {
    s->win[s->pos & WIN_MASK] = b;
    if (++s->pos - s->flushed >= FLUSH_AT) flush(s);
}

static int stored(St *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->in_len - s->in_pos < 4) return -1;
    uint32_t len = s->in[s->in_pos] | (s->in[s->in_pos + 1] << 8);
    uint32_t nlen = s->in[s->in_pos + 2] | (s->in[s->in_pos + 3] << 8);
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF) || len > s->in_len - s->in_pos) return -1;
    for (uint32_t i = 0; i < len && !s->stopped; i++) put(s, s->in[s->in_pos + i]);
    s->in_pos += len;
    return 0;
}

static int decode(St *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* Returns 0 if complete, >0 if incomplete, <0 if over-subscribed. */
static int construct(Huff *h, const int16_t *length, int n) {
    int16_t offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (int16_t)sym;
    return left;
}

static int codes(St *s, const Huff *lencode, const Huff *distcode) {
    static const int16_t lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int16_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int16_t dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const int16_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    while (!s->stopped) {
        int sym = decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            put(s, (uint8_t)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            uint32_t len = (uint32_t)(lbase[sym] + bits(s, lext[sym]));
            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return -1;
            uint32_t dist = (uint32_t)(dbase[dsym] + bits(s, dext[dsym]));
            if (s->err || dist > s->pos) return -1;
            for (uint32_t i = 0; i < len && !s->stopped; i++) put(s, s->win[(s->pos - dist) & WIN_MASK]);
        }
    }
    return 0;
}

static int fixed(St *s) {
    static Huff lencode, distcode;
    static int built;
    if (!built) {
        int16_t lengths[FIXLCODES];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(St *s) {
    static const int16_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int16_t lengths[MAXLCODES + MAXDCODES];
    Huff lencode, distcode;

    int nlen = bits(s, 5) + 257;
    int ndist = bits(s, 5) + 1;
    int ncode = bits(s, 4) + 4;
    if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) return -1;

    int idx;
    for (idx = 0; idx < ncode; idx++) lengths[order[idx]] = (int16_t)bits(s, 3);
    for (; idx < 19; idx++) lengths[order[idx]] = 0;
    if (s->err || construct(&lencode, lengths, 19) != 0) return -1;

    idx = 0;
    while (idx < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[idx++] = (int16_t)sym;
        } else {
            int16_t len = 0;
            int rep;
            if (sym == 16) {
                if (idx == 0) return -1;
                len = lengths[idx - 1];
                rep = 3 + bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + bits(s, 3);
            } else {
                rep = 11 + bits(s, 7);
            }
            if (s->err || idx + rep > nlen + ndist) return -1;
            while (rep--) lengths[idx++] = len;
        }
    }
    if (lengths[256] == 0) return -1;

    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return -1;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return -1;
    return codes(s, &lencode, &distcode);
}

int zlib_inflate_stream(const uint8_t *in, uint32_t in_len, inflate_sink sink, void *ctx) {
    if (in_len < 2) return -1;
    uint8_t cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return -1;

    St s;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.in_len = in_len;
    s.in_pos = 2;
    s.sink = sink;
    s.ctx = ctx;
    s.win = (uint8_t *)malloc(WIN_SIZE);
    if (!s.win) return -1;

    int last, r;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.err) {
            r = -1;
            break;
        }
        r = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (s.err) r = -1;
    } while (!last && r == 0 && !s.stopped);

    if (r == 0) flush(&s);
    free(s.win);
    if (r != 0) return -1;
    return s.stopped ? 1 : 0;
}

/* ---- whole-buffer convenience --------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    uint32_t len, cap, max;
    int overflow;
} Grow;

static int grow_sink(void *ctx, uint64_t pos, const uint8_t *data, uint32_t len) {
    Grow *g = (Grow *)ctx;
    (void)pos;
    if (len > g->max - g->len) {
        g->overflow = 1;
        return 1;
    }
    if (g->len + len > g->cap) {
        uint32_t cap = g->cap ? g->cap : 65536;
        while (cap < g->len + len) cap = cap > g->max / 2 ? g->max : cap * 2;
        uint8_t *p = (uint8_t *)realloc(g->buf, cap);
        if (!p) {
            g->overflow = 1;
            return 1;
        }
        g->buf = p;
        g->cap = cap;
    }
    memcpy(g->buf + g->len, data, len);
    g->len += len;
    return 0;
}

int zlib_inflate(const uint8_t *in, uint32_t in_len, uint32_t max_out, uint8_t **out, uint32_t *out_len) {
    Grow g;
    memset(&g, 0, sizeof g);
    g.max = max_out;
    *out = NULL;
    *out_len = 0;
    if (zlib_inflate_stream(in, in_len, grow_sink, &g) != 0 || g.overflow) {
        free(g.buf);
        return -1;
    }
    *out = g.buf ? g.buf : (uint8_t *)malloc(1);
    *out_len = g.len;
    return *out ? 0 : -1;
}

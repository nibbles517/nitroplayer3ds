#include "twosf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inflate.h"
#include "ncsf.h"
#include "nds_rom.h"

#define TWOSF_MAX_FILE (64u * 1024 * 1024)
#define MAX_PIECES 2048
#define PIECE_PATH 40

static void set_err(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

static uint8_t *read_file(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n > 0 && (unsigned long)n <= TWOSF_MAX_FILE && fseek(f, 0, SEEK_SET) == 0) {
        buf = (uint8_t *)malloc((size_t)n + 1);
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            buf[n] = 0;
            *size = (uint32_t)n;
        } else {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

/* PSF layout: "PSF" ver, u32 reserved size, u32 program size, u32 crc, reserved, program, [TAG]. */
static int psf_split(const uint8_t *b, uint32_t size, uint8_t ver, const uint8_t **prog, uint32_t *prog_len,
                     char **tags) {
    if (size < 16 || memcmp(b, "PSF", 3) != 0 || b[3] != ver) return -1;
    uint32_t rsv = rd32(b + 4), pl = rd32(b + 8);
    if (rsv > size - 16 || pl > size - 16 - rsv) return -1;
    *prog = b + 16 + rsv;
    *prog_len = pl;
    uint32_t t = 16 + rsv + pl;
    *tags = (size - t >= 5 && memcmp(b + t, "[TAG]", 5) == 0) ? (char *)b + t + 5 : NULL;
    return 0;
}

static int prefix_ci(const char *s, const char *pre) {
    for (; *pre; s++, pre++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
    return 1;
}

/* ---- library: stream the game image, keep the sound files ------------------------------- */

typedef struct {
    uint32_t rom_off, len, blob_off;
    char path[PIECE_PATH];
} Piece;

typedef struct {
    int64_t rom_base; /* rom position of output byte 8 (after the 8-byte load header) */
    uint8_t ldr[8];
    uint8_t hdr[0x200];
    uint8_t *fnt, *fat;
    uint32_t fnt_off, fnt_len, fat_off, fat_len;
    uint32_t got_hdr, got_fnt, got_fat; /* bytes captured */
    int tables_done;
    Piece *piece;
    int npieces;
    uint8_t *blob;
    uint32_t blob_len;
    uint64_t captured, needed;
    int pass;
} Cap;

/* Copies the overlap of [rom, rom+len) with [off, off+want) into dst; returns bytes copied. */
static uint32_t overlap(uint64_t rom, const uint8_t *d, uint32_t len, uint32_t off, uint32_t want, uint8_t *dst) {
    uint64_t a = rom > off ? rom : off;
    uint64_t b = rom + len < (uint64_t)off + want ? rom + len : (uint64_t)off + want;
    if (a >= b) return 0;
    memcpy(dst + (a - off), d + (a - rom), (size_t)(b - a));
    return (uint32_t)(b - a);
}

static int visit_sound(const NdsFile *f, void *user) {
    Cap *c = (Cap *)user;
    if (!prefix_ci(f->path, "/SOUND/BGM/") && !prefix_ci(f->path, "/SOUND/ME/")) return 0;
    if (f->size == 0 || c->npieces >= MAX_PIECES || strlen(f->path) >= PIECE_PATH) return 0;
    if (f->size > TWOSF_MAX_FILE - c->blob_len) return 0;
    Piece *p = &c->piece[c->npieces++];
    p->rom_off = f->offset;
    p->len = f->size;
    p->blob_off = c->blob_len;
    memcpy(p->path, f->path, strlen(f->path) + 1); /* length checked above */
    c->blob_len += f->size;
    return 0;
}

static void start_tables(Cap *c) {
    c->fnt_off = rd32(c->hdr + 0x40);
    c->fnt_len = rd32(c->hdr + 0x44);
    c->fat_off = rd32(c->hdr + 0x48);
    c->fat_len = rd32(c->hdr + 0x4C);
    if (c->fnt_len < 8 || c->fnt_len > 0x400000 || c->fat_len > 0x400000) return;
    c->fnt = (uint8_t *)malloc(c->fnt_len);
    c->fat = (uint8_t *)malloc(c->fat_len ? c->fat_len : 1);
}

static void finish_tables(Cap *c) {
    c->tables_done = 1;
    nds_walk_tables(c->fnt, c->fnt_len, c->fat, c->fat_len, 0xFFFFFFFFu, visit_sound, c);
    if (c->blob_len) c->blob = (uint8_t *)malloc(c->blob_len);
    if (!c->blob) c->npieces = 0;
    for (int i = 0; i < c->npieces; i++) c->needed += c->piece[i].len;
}

static int cap_sink(void *ctx, uint64_t pos, const uint8_t *d, uint32_t len) {
    Cap *c = (Cap *)ctx;
    while (pos < 8 && len) { /* first 8 output bytes: u32 load offset, u32 size */
        c->ldr[pos] = *d;
        if (pos == 7) c->rom_base = (int64_t)rd32(c->ldr);
        pos++;
        d++;
        len--;
    }
    if (!len) return 0;
    uint64_t rom = (uint64_t)(c->rom_base + (int64_t)(pos - 8));

    if (c->pass == 0) {
        if (c->got_hdr < sizeof c->hdr) {
            c->got_hdr += overlap(rom, d, len, 0, sizeof c->hdr, c->hdr);
            if (c->got_hdr >= sizeof c->hdr) start_tables(c);
        }
        if (c->fnt && c->fat && !c->tables_done) {
            c->got_fnt += overlap(rom, d, len, c->fnt_off, c->fnt_len, c->fnt);
            c->got_fat += overlap(rom, d, len, c->fat_off, c->fat_len, c->fat);
            if (c->got_fnt >= c->fnt_len && c->got_fat >= c->fat_len) finish_tables(c);
        }
    }
    if (c->blob)
        for (int i = 0; i < c->npieces; i++) {
            Piece *p = &c->piece[i];
            c->captured += overlap(rom, d, len, p->rom_off, p->len, c->blob + p->blob_off);
        }
    return c->blob && c->captured >= c->needed; /* stop once every sound file is in */
}

int twosf_lib_load(TwoSfLib *lib, const char *path, char *err, size_t errlen) {
    memset(lib, 0, sizeof *lib);
    uint32_t size = 0;
    uint8_t *buf = read_file(path, &size);
    if (!buf) {
        set_err(err, errlen, "Missing .2sflib");
        return -1;
    }
    const uint8_t *prog;
    uint32_t prog_len;
    char *tags;
    int rc = -1;
    Cap *c = (Cap *)calloc(1, sizeof *c);
    if (c) c->piece = (Piece *)calloc(MAX_PIECES, sizeof(Piece));
    if (!c || !c->piece) {
        set_err(err, errlen, "Out of memory");
        goto done;
    }
    if (psf_split(buf, size, 0x24, &prog, &prog_len, &tags) != 0 || prog_len == 0) {
        set_err(err, errlen, "Not a 2SF library");
        goto done;
    }
    /* Pass 1 grabs the header, the file tables and (usually) every sound file. A second
     * pass is only needed if a sound file came before the tables in the image. */
    for (c->pass = 0; c->pass < 2; c->pass++) {
        if (c->pass == 1) c->captured = 0;
        if (zlib_inflate_stream(prog, prog_len, cap_sink, c) < 0) {
            set_err(err, errlen, "2SF library is corrupt");
            goto done;
        }
        if (!c->tables_done) {
            set_err(err, errlen, "2SF library has no file system");
            goto done;
        }
        if (!c->blob || c->captured >= c->needed) break;
    }
    if (!c->blob || c->captured < c->needed) {
        set_err(err, errlen, "No DSE music in this 2SF set");
        goto done;
    }

    lib->blob = c->blob;
    lib->blob_len = c->blob_len;
    c->blob = NULL;
    snprintf(lib->path, sizeof lib->path, "%s", path);
    DataSource src = ds_mem(lib->blob, lib->blob_len);
    dse_set_init(&lib->dse, &src);
    for (int i = 0; i < c->npieces; i++)
        dse_set_add_file(&lib->dse, c->piece[i].path, c->piece[i].blob_off, c->piece[i].len);
    if (dse_set_finish(&lib->dse) == 0) {
        set_err(err, errlen, "No DSE music in this 2SF set");
        twosf_lib_free(lib);
        goto done;
    }
    rc = 0;
done:
    if (c) {
        free(c->fnt);
        free(c->fat);
        free(c->blob);
        free(c->piece);
    }
    free(c);
    free(buf);
    return rc;
}

void twosf_lib_free(TwoSfLib *lib) {
    dse_set_free(&lib->dse);
    free(lib->blob);
    memset(lib, 0, sizeof *lib);
}

/* ---- mini2sf ---------------------------------------------------------------------------- */

static void tag_value(const char *tags, const char *name, char *out, size_t n) {
    out[0] = 0;
    if (!tags) return;
    size_t nl = strlen(name);
    for (const char *line = tags; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t ll = end ? (size_t)(end - line) : strlen(line);
        const char *p = line;
        while (p < line + ll && isspace((unsigned char)*p)) p++;
        if ((size_t)(line + ll - p) > nl && prefix_ci(p, name)) {
            const char *q = p + nl;
            while (q < line + ll && isspace((unsigned char)*q)) q++;
            if (q < line + ll && *q == '=') {
                q++;
                while (q < line + ll && isspace((unsigned char)*q)) q++;
                const char *e = line + ll;
                while (e > q && isspace((unsigned char)e[-1])) e--;
                size_t vl = (size_t)(e - q);
                if (vl >= n) vl = n - 1;
                memcpy(out, q, vl);
                out[vl] = 0;
                return;
            }
        }
        line = end ? end + 1 : NULL;
    }
}

int mini2sf_read(const char *path, Mini2sf *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    uint32_t size = 0;
    uint8_t *buf = read_file(path, &size);
    if (!buf) {
        set_err(err, errlen, "Couldn't read file");
        return -1;
    }
    const uint8_t *prog;
    uint32_t prog_len;
    char *tags;
    int rc = -1;
    if (psf_split(buf, size, 0x24, &prog, &prog_len, &tags) != 0) {
        set_err(err, errlen, "Not a 2SF file");
        goto done;
    }
    char v[64];
    tag_value(tags, "_lib", out->lib, sizeof out->lib);
    tag_value(tags, "title", out->title, sizeof out->title);
    tag_value(tags, "game", out->game, sizeof out->game);
    tag_value(tags, "length", v, sizeof v);
    out->length_ms = ncsf_parse_time(v);
    tag_value(tags, "fade", v, sizeof v);
    out->fade_ms = ncsf_parse_time(v);

    if (prog_len) { /* patch: u32 offset, u32 size, data -> the song number */
        uint8_t *p;
        uint32_t pl;
        if (zlib_inflate(prog, prog_len, 0x10000, &p, &pl) == 0) {
            uint32_t n = pl >= 8 ? rd32(p + 4) : 0;
            if (n >= 1 && n <= 4 && pl >= 8 + n) {
                uint32_t val = 0;
                for (uint32_t i = 0; i < n; i++) val |= (uint32_t)p[8 + i] << (8 * i);
                out->number = val;
                out->has_number = 1;
            }
            free(p);
        }
    }
    if (!out->lib[0]) {
        set_err(err, errlen, "2SF file has no _lib");
        goto done;
    }
    if (!out->has_number) {
        set_err(err, errlen, "Unsupported 2SF set (no song number)");
        goto done;
    }
    rc = 0;
done:
    free(buf);
    return rc;
}

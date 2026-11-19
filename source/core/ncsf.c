#include "ncsf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datasource.h"
#include "inflate.h"

#define NCSF_MAX_FILE (32u * 1024 * 1024)
#define NCSF_MAX_SDAT (64u * 1024 * 1024)
#define NCSF_MAX_DEPTH 4

static void set_err(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

static uint8_t *read_file(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n > 0 && (unsigned long)n <= NCSF_MAX_FILE && fseek(f, 0, SEEK_SET) == 0) {
            buf = (uint8_t *)malloc((size_t)n + 1);
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                buf[n] = 0;
                *size = (uint32_t)n;
            } else {
                free(buf);
                buf = NULL;
            }
        }
    }
    fclose(f);
    return buf;
}

uint32_t ncsf_parse_time(const char *s) {
    if (!s) return 0;
    double total = 0, part = 0, frac_div = 0;
    int any = 0;
    for (; *s; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            if (frac_div > 0) {
                part += (c - '0') / frac_div;
                frac_div *= 10;
            } else {
                part = part * 10 + (c - '0');
            }
            any = 1;
        } else if (c == ':') {
            total = (total + part) * 60;
            part = 0;
            frac_div = 0;
        } else if ((c == '.' || c == ',') && frac_div == 0) {
            frac_div = 10;
        } else if (!isspace((unsigned char)c)) {
            break;
        }
    }
    if (!any) return 0;
    total += part;
    return total > 0 ? (uint32_t)(total * 1000.0 + 0.5) : 0;
}

typedef struct {
    char lib[256];
    int has_lib;
} TagCtx;

static void copy_tag(char *dst, size_t n, const char *v) {
    snprintf(dst, n, "%s", v);
}

static void parse_tags(char *text, Ncsf *out, TagCtx *tc) {
    char *line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char *name = line, *val = eq + 1;
            while (isspace((unsigned char)*name)) name++;
            char *e = name + strlen(name);
            while (e > name && isspace((unsigned char)e[-1])) *--e = 0;
            while (isspace((unsigned char)*val)) val++;
            e = val + strlen(val);
            while (e > val && isspace((unsigned char)e[-1])) *--e = 0;
            for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);

            if (!strcmp(name, "_lib")) {
                copy_tag(tc->lib, sizeof tc->lib, val);
                tc->has_lib = 1;
            } else if (!strcmp(name, "title")) {
                copy_tag(out->title, sizeof out->title, val);
            } else if (!strcmp(name, "game")) {
                copy_tag(out->game, sizeof out->game, val);
            } else if (!strcmp(name, "artist")) {
                copy_tag(out->artist, sizeof out->artist, val);
            } else if (!strcmp(name, "length")) {
                out->length_ms = ncsf_parse_time(val);
            } else if (!strcmp(name, "fade")) {
                out->fade_ms = ncsf_parse_time(val);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

static int load_rec(const char *path, Ncsf *out, int depth, char *err, size_t errlen) {
    if (depth > NCSF_MAX_DEPTH) {
        set_err(err, errlen, "_lib chain too deep");
        return -1;
    }
    uint32_t size = 0;
    uint8_t *buf = read_file(path, &size);
    if (!buf) {
        set_err(err, errlen, depth ? "Missing .ncsflib" : "Couldn't read file");
        return -1;
    }
    int rc = -1;
    if (size < 16 || memcmp(buf, "PSF\x25", 4) != 0) {
        set_err(err, errlen, "Not an NCSF file");
        goto done;
    }
    uint32_t rsv = rd32(buf + 4), prog = rd32(buf + 8);
    if (rsv > size - 16 || prog > size - 16 - rsv) {
        set_err(err, errlen, "Corrupt NCSF header");
        goto done;
    }
    const uint8_t *rsv_p = buf + 16;
    const uint8_t *prog_p = rsv_p + rsv;
    uint32_t tag_off = 16 + rsv + prog;

    /* Tags: this file's tags take priority, so parse them into a scratch copy. */
    Ncsf mine;
    memset(&mine, 0, sizeof mine);
    TagCtx tc;
    memset(&tc, 0, sizeof tc);
    if (size - tag_off >= 5 && memcmp(buf + tag_off, "[TAG]", 5) == 0) parse_tags((char *)buf + tag_off + 5, &mine, &tc);

    if (tc.has_lib && tc.lib[0]) {
        char libpath[512];
        const char *slash = strrchr(path, '/');
        const char *bslash = strrchr(path, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        int dirlen = slash ? (int)(slash - path + 1) : 0;
        snprintf(libpath, sizeof libpath, "%.*s%s", dirlen, path, tc.lib);
        if (load_rec(libpath, out, depth + 1, err, errlen) != 0) goto done;
    }

    if (prog > 0) {
        uint8_t *sdat;
        uint32_t sdat_size;
        if (zlib_inflate(prog_p, prog, NCSF_MAX_SDAT, &sdat, &sdat_size) != 0) {
            set_err(err, errlen, "Couldn't decompress SDAT");
            goto done;
        }
        free(out->sdat); /* a non-empty program overrides the library's */
        out->sdat = sdat;
        out->sdat_size = sdat_size;
    }
    if (rsv >= 4) out->seq = rd32(rsv_p);

    if (mine.title[0]) memcpy(out->title, mine.title, sizeof out->title);
    if (mine.game[0]) memcpy(out->game, mine.game, sizeof out->game);
    if (mine.artist[0]) memcpy(out->artist, mine.artist, sizeof out->artist);
    if (mine.length_ms) out->length_ms = mine.length_ms;
    if (mine.fade_ms) out->fade_ms = mine.fade_ms;
    rc = 0;

done:
    free(buf);
    return rc;
}

int ncsf_load(const char *path, Ncsf *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (load_rec(path, out, 0, err, errlen) != 0) {
        ncsf_free(out);
        return -1;
    }
    if (!out->sdat || out->sdat_size < 0x40 || memcmp(out->sdat, "SDAT", 4) != 0) {
        set_err(err, errlen, "NCSF contains no SDAT");
        ncsf_free(out);
        return -1;
    }
    return 0;
}

void ncsf_free(Ncsf *n) {
    free(n->sdat);
    memset(n, 0, sizeof *n);
}

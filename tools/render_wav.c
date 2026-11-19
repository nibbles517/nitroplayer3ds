/*
 * render_wav - renders a song with the same core the 3DS app uses, to a .wav file.
 *
 *   render_wav game.nds  out.wav [-d sdat#] [-q seq#] [-t strm#] [-m max_seconds] [-r rate]
 *   render_wav game.nds  out.wav -a arc# -q entry#      (sound effect from a sequence archive)
 *   render_wav song.ncsf out.wav [-m max_seconds]
 *
 * Build (from the repo root):
 *   cc -std=c99 -O2 -Isource/core tools/render_wav.c source/core/[a-z]*.c -lm -o render_wav
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ncsf.h"
#include "nds_music.h"
#include "song.h"
#include "twosf.h"

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}
static void put16(FILE *f, uint16_t v) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, f);
}

static int ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), m = strlen(ext);
    if (n < m) return 0;
    for (size_t i = 0; i < m; i++) {
        char c = s[n - m + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != ext[i]) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.nds|in.ncsf|in.mini2sf out.wav [-d sdat#] [-q seq#] [-t strm#] [-a arc# -q entry#]\n"
                        "       [-b dse_bgm#] [-m max_s] [-r rate]\n", argv[0]);
        return 2;
    }
    int sdat_i = 0, strm_i = -1, arc_i = -1, bgm_i = -1, max_s = 300;
    uint32_t seq_i = 0, rate = 32728;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-d")) sdat_i = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-q")) seq_i = (uint32_t)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-t")) strm_i = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-a")) arc_i = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-b")) bgm_i = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-m")) max_s = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-r")) rate = (uint32_t)atoi(argv[i + 1]);
    }

    NdsMusic rom;
    Ncsf ncsf;
    Sdat nsdat;
    int is_ncsf = ends_with(argv[1], ".ncsf") || ends_with(argv[1], ".minincsf");
    int is_2sf = ends_with(argv[1], ".mini2sf") || ends_with(argv[1], ".2sf");
    const Sdat *sd = NULL;
    TwoSfLib lib;
    Mini2sf mini;
    uint32_t len_ms = 0, fade_ms = 0;
    memset(&lib, 0, sizeof lib);
    Song *g = NULL;
    memset(&rom, 0, sizeof rom);

    if (is_2sf) {
        char err[128], libpath[1024];
        if (mini2sf_read(argv[1], &mini, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        const char *slash = strrchr(argv[1], '/');
        snprintf(libpath, sizeof libpath, "%.*s%s", slash ? (int)(slash - argv[1] + 1) : 0, argv[1], mini.lib);
        if (twosf_lib_load(&lib, libpath, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", libpath, err);
            return 1;
        }
        int idx = dse_set_find(&lib.dse, DSE_GROUP_BGM, mini.number);
        printf("2SF: \"%s\", song %u (%s), %d DSE songs in library, length %u ms, fade %u ms\n", mini.title,
               (unsigned)mini.number, idx >= 0 ? lib.dse.songs[idx].label : "missing", lib.dse.num,
               (unsigned)mini.length_ms, (unsigned)mini.fade_ms);
        g = idx >= 0 ? song_open_dse(&lib.dse, idx, rate) : NULL;
        len_ms = mini.length_ms;
        fade_ms = mini.fade_ms;
    } else if (is_ncsf) {
        char err[128];
        if (ncsf_load(argv[1], &ncsf, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        DataSource ds = ds_mem(ncsf.sdat, ncsf.sdat_size);
        if (sdat_open(&nsdat, &ds) != 0) {
            fprintf(stderr, "bad SDAT in NCSF\n");
            return 1;
        }
        sd = &nsdat;
        seq_i = ncsf.seq;
        printf("NCSF: \"%s\" from \"%s\", seq %u, length %u ms, fade %u ms\n", ncsf.title, ncsf.game, (unsigned)seq_i,
               (unsigned)ncsf.length_ms, (unsigned)ncsf.fade_ms);
    } else {
        NdsMusicResult r = nds_music_open(&rom, argv[1]);
        if (r != NDSM_OK) {
            fprintf(stderr, "%s\n", nds_music_strerror(r));
            return 1;
        }
        if (rom.dse.num && (bgm_i >= 0 || rom.num_sdats == 0)) {
            int idx = dse_set_find(&rom.dse, DSE_GROUP_BGM, (uint32_t)(bgm_i < 0 ? 0 : bgm_i));
            printf("DSE: %d songs; playing bgm%04d (%s)\n", rom.dse.num, bgm_i < 0 ? 0 : bgm_i,
                   idx >= 0 ? rom.dse.songs[idx].label : "missing");
            g = idx >= 0 ? song_open_dse(&rom.dse, idx, rate) : NULL;
        } else if (sdat_i < 0 || sdat_i >= rom.num_sdats) {
            fprintf(stderr, "no SDAT %d\n", sdat_i);
            return 1;
        } else {
            sd = &rom.sdat[sdat_i];
        }
    }

    if (sd)
        g = strm_i >= 0  ? song_open_strm(sd, (uint32_t)strm_i, rate)
            : arc_i >= 0 ? song_open_ssar(sd, (uint32_t)arc_i, seq_i, rate)
                         : song_open_seq(sd, seq_i, rate);
    if (!g) {
        fprintf(stderr, "couldn't open %s %d\n", strm_i >= 0 ? "stream" : arc_i >= 0 ? "effect" : "sequence",
                strm_i >= 0 ? strm_i : (int)seq_i);
        if (is_ncsf) {
            sdat_close(&nsdat);
            ncsf_free(&ncsf);
        } else {
            nds_music_close(&rom);
        }
        twosf_lib_free(&lib);
        return 1;
    }
    if (is_ncsf && ncsf.length_ms) song_set_length(g, ncsf.length_ms, ncsf.fade_ms);
    if (is_2sf && len_ms) song_set_length(g, len_ms, fade_ms);
    if (arc_i >= 0) song_set_loops(g, 1, 1500); /* same as the 3DS app for effects */

    FILE *f = fopen(argv[2], "wb");
    if (!f) {
        perror(argv[2]);
        return 1;
    }
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, rate);
    put32(f, rate * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data\0\0\0\0", 1, 8, f);

    int16_t buf[2048 * 2];
    uint64_t total = 0, limit = (uint64_t)max_s * rate;
    int peak = 0;
    while (!song_done(g) && total < limit) {
        song_render(g, buf, 2048);
        for (int i = 0; i < 4096; i++) {
            int v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > peak) peak = v;
        }
        fwrite(buf, 4, 2048, f);
        total += 2048;
    }
    uint32_t bytes = (uint32_t)(total * 4);
    fseek(f, 4, SEEK_SET);
    put32(f, 36 + bytes);
    fseek(f, 40, SEEK_SET);
    put32(f, bytes);
    fclose(f);
    printf("wrote %.2f s, loops=%d, peak=%d, ended=%s\n", (double)total / rate, song_loops(g), peak,
           song_done(g) ? "yes" : "no (hit -m limit)");

    song_close(g);
    if (is_ncsf) {
        sdat_close(&nsdat);
        ncsf_free(&ncsf);
    } else if (!is_2sf) {
        nds_music_close(&rom);
    }
    twosf_lib_free(&lib);
    return 0;
}

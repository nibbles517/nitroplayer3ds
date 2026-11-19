/*
 * twosf.h - plays 2SF sets of DSE games (e.g. PMD: Explorers of Sky) without emulation.
 *
 * A .2sflib holds the game image; each .mini2sf patches one value (the song number) into
 * the game's code. Instead of running the code, we stream the library once, keep only the
 * SOUND/BGM and SOUND/ME files (a few MB out of ~128 MB), and play song N with the DSE player.
 */
#ifndef NDSMUSIC_TWOSF_H
#define NDSMUSIC_TWOSF_H

#include <stddef.h>
#include <stdint.h>

#include "dse_set.h"

typedef struct {
    char path[512]; /* library this was loaded from */
    uint8_t *blob;  /* captured music files */
    uint32_t blob_len;
    DseSet dse;
} TwoSfLib;

typedef struct {
    char lib[256];      /* _lib tag (relative to the mini2sf's folder) */
    int has_number;
    uint32_t number;    /* patched value = song number */
    char title[96], game[96];
    uint32_t length_ms, fade_ms;
} Mini2sf;

/* 0 on success; err gets a short reason otherwise. */
int twosf_lib_load(TwoSfLib *lib, const char *path, char *err, size_t errlen);
void twosf_lib_free(TwoSfLib *lib);

int mini2sf_read(const char *path, Mini2sf *out, char *err, size_t errlen);

#endif

/*
 * nds_rom.h - NDS cartridge header + NitroFS (FNT/FAT) walker.
 */
#ifndef NDSMUSIC_NDS_ROM_H
#define NDSMUSIC_NDS_ROM_H

#include "datasource.h"

#define NDS_PATH_MAX 256

typedef struct {
    char title[13];   /* 12 chars + NUL */
    char gamecode[5]; /* e.g. "B2KJ" */
    uint32_t fnt_off, fnt_size;
    uint32_t fat_off, fat_size;
} NdsHeader;

typedef struct {
    char path[NDS_PATH_MAX]; /* e.g. "/data/sound/sound_data.sdat" */
    uint16_t file_id;
    uint32_t offset; /* absolute ROM offset */
    uint32_t size;
} NdsFile;

/* Return 0 to keep walking, nonzero to stop (that value is returned by nds_walk_files). */
typedef int (*nds_visit_fn)(const NdsFile *f, void *user);

int nds_read_header(const DataSource *rom, NdsHeader *out);

/* Visits every named file in NitroFS. Returns 0 when done, <0 on malformed tables,
 * or the callback's nonzero value if it stopped early. */
int nds_walk_files(const DataSource *rom, const NdsHeader *h, nds_visit_fn fn, void *user);
/* Same, from FNT/FAT tables already in memory (rom_size bounds the file offsets). */
int nds_walk_tables(const uint8_t *fnt, uint32_t fnt_size, const uint8_t *fat, uint32_t fat_size, uint32_t rom_size,
                    nds_visit_fn fn, void *user);

/* Finds SDAT archives: first by ".sdat" extension, then (if none) by "SDAT" magic.
 * Returns number found (<= max), or <0 on error. */
int nds_find_sdats(const DataSource *rom, const NdsHeader *h, NdsFile *out, int max);

#endif

/*
 * ncsf.h - NCSF / miniNCSF loader (PSF version 0x25).
 *
 * Layout: "PSF\x25", u32 reserved size, u32 program size, u32 crc32, reserved, program, "[TAG]" text.
 * The program section is a zlib-compressed SDAT; the reserved section holds the sequence number.
 * miniNCSFs usually carry no SDAT and pull it from an .ncsflib named in a _lib tag.
 */
#ifndef NDSMUSIC_NCSF_H
#define NDSMUSIC_NCSF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *sdat; /* decompressed SDAT (malloc'd) */
    uint32_t sdat_size;
    uint32_t seq;  /* sequence to play */
    char title[96], game[96], artist[96];
    uint32_t length_ms, fade_ms; /* 0 = not tagged */
} Ncsf;

/* 0 on success; on failure err gets a short reason and nothing needs freeing. */
int ncsf_load(const char *path, Ncsf *out, char *err, size_t errlen);
void ncsf_free(Ncsf *n);

/* "m:ss.mmm", "ss.mmm", "h:mm:ss" -> milliseconds (0 if unparseable). */
uint32_t ncsf_parse_time(const char *s);

#endif

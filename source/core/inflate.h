/*
 * inflate.h - small self-contained zlib/DEFLATE decompressor, so the app needs no extra
 * portlibs. Works either into one buffer, or streaming through a callback with only a
 * 64 KB window in memory (used to pick a few files out of a 128 MB 2SF game image).
 */
#ifndef NDSMUSIC_INFLATE_H
#define NDSMUSIC_INFLATE_H

#include <stdint.h>

/* Receives decompressed data in order. `pos` is the offset of data[0] in the output.
 * Return nonzero to stop early. */
typedef int (*inflate_sink)(void *ctx, uint64_t pos, const uint8_t *data, uint32_t len);

/* 0 = finished, 1 = stopped by the sink, -1 = corrupt input. */
int zlib_inflate_stream(const uint8_t *in, uint32_t in_len, inflate_sink sink, void *ctx);

/* Decompresses a whole zlib stream into a malloc'd buffer (*out). max_out caps the output
 * size (guards against decompression bombs). Returns 0 on success. */
int zlib_inflate(const uint8_t *in, uint32_t in_len, uint32_t max_out, uint8_t **out, uint32_t *out_len);

#endif

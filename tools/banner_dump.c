/*
 * banner_dump - prints a ROM's banner titles and animation sequence, and writes each
 * animation frame as a PPM image (transparent pixels shown as magenta).
 *
 *   banner_dump game.nds [out_prefix]
 *
 * Build (from the repo root):
 *   cc -std=c99 -O2 -Isource/core tools/banner_dump.c source/core/[a-z]*.c -lm -o banner_dump
 */
#include <stdio.h>
#include <stdlib.h>

#include "nds_banner.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s game.nds [out_prefix]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    DataSource rom = ds_file(f);
    static NdsBanner b;
    if (nds_read_banner(&rom, &b) != 0) {
        printf("no banner\n");
        fclose(f);
        return 1;
    }
    static const char *names[BANNER_LANGS] = {"JP", "EN", "FR", "DE", "IT", "ES", "ZH", "KO"};
    for (int i = 0; i < BANNER_LANGS; i++)
        if (b.title[i][0]) {
            printf("%s: ", names[i]);
            for (const char *p = b.title[i]; *p; p++) putchar(*p == '\n' ? '|' : *p);
            putchar('\n');
        }
    printf("animated=%d frames=%d bitmaps=%d palettes=%d\n", b.animated, b.num_frames, b.num_bitmaps, b.num_palettes);
    for (int i = 0; i < b.num_frames; i++)
        printf("  frame %d: bmp=%d pal=%d flipH=%d flipV=%d dur=%d\n", i, b.frame[i].bitmap, b.frame[i].palette,
               b.frame[i].flip_h, b.frame[i].flip_v, b.frame[i].duration);
    if (argc > 2) {
        for (int i = 0; i < b.num_frames; i++) {
            char path[512];
            snprintf(path, sizeof path, "%s%d.ppm", argv[2], i);
            FILE *o = fopen(path, "wb");
            if (!o) continue;
            fprintf(o, "P6 32 32 255\n");
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 32; x++) {
                    int c = nds_banner_pixel(&b, i, x, y);
                    unsigned char px[3] = {255, 0, 255};
                    if (c >= 0) {
                        px[0] = (unsigned char)((c & 31) << 3);
                        px[1] = (unsigned char)(((c >> 5) & 31) << 3);
                        px[2] = (unsigned char)(((c >> 10) & 31) << 3);
                    }
                    fwrite(px, 1, 3, o);
                }
            fclose(o);
        }
    }
    fclose(f);
    return 0;
}

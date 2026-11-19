/*
 * sdat_list - PC test harness for the ndsmusic loader.
 *
 *   sdat_list game.nds                       list SDATs, sequences and streams
 *   sdat_list game.nds <sdat#> <seq#> <dir>  dump that sequence's SSEQ/SBNK/SWARs to <dir>
 *
 * Build (from the repo root):
 *   cc -std=c99 -O2 -Isource/core tools/sdat_list.c source/core/[a-z]*.c -lm -o sdat_list
 */
#include <stdio.h>
#include <stdlib.h>

#include "nds_music.h"

static void dump(const char *dir, const char *name, const uint8_t *data, uint32_t size) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fwrite(data, 1, size, f);
    fclose(f);
    printf("  wrote %s (%u bytes)\n", path, (unsigned)size);
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 5) {
        fprintf(stderr, "usage: %s game.nds [sdat# seq# outdir]\n", argv[0]);
        return 2;
    }

    NdsMusic m;
    NdsMusicResult r = nds_music_open(&m, argv[1]);
    if (r != NDSM_OK) {
        fprintf(stderr, "%s\n", nds_music_strerror(r));
        return 1;
    }
    printf("%s [%s]: %d SDAT(s)\n", m.hdr.title, m.hdr.gamecode, m.num_sdats);

    if (argc == 2) {
        if (m.dse.num) printf("DSE music: %d songs\n", m.dse.num);
        for (int i = 0; i < m.dse.num; i++)
            printf("  %s %04u  %s\n", m.dse.songs[i].group == DSE_GROUP_ME ? "ME " : "BGM", m.dse.songs[i].number,
                   m.dse.songs[i].label);
        for (int i = 0; i < m.num_sdats; i++) {
            const Sdat *s = &m.sdat[i];
            printf("SDAT %d: %s (%u bytes) - %u seq, %u seqarc, %u bank, %u wavearc, %u strm\n", i, m.sdat_file[i].path,
                   (unsigned)m.sdat_file[i].size, (unsigned)s->num_seq, (unsigned)s->num_seqarc, (unsigned)s->num_bank,
                   (unsigned)s->num_wavearc, (unsigned)s->num_strm);
            for (uint32_t q = 0; q < s->num_seq; q++) {
                if (!s->seq[q].present) continue;
                const char *nm = sdat_seq_name(s, q);
                printf("  SEQ  %4u  %-24s bank=%u vol=%u ply=%u\n", (unsigned)q, nm ? nm : "(unnamed)",
                       s->seq[q].bank, s->seq[q].vol, s->seq[q].ply);
            }
            for (uint32_t a = 0; a < s->num_seqarc; a++) {
                SdatSsarEntry *ents;
                uint32_t n = sdat_ssar_entries(s, a, &ents);
                const char *an = sdat_ssar_name(s, a);
                printf("  SSAR %4u  %-24s %u entries\n", (unsigned)a, an ? an : "(unnamed)", (unsigned)n);
                for (uint32_t q = 0; q < n; q++) {
                    if (!ents[q].valid) continue;
                    const char *nm = sdat_ssar_entry_name(s, a, q);
                    printf("    SE %4u.%03u %-22s bank=%u vol=%u\n", (unsigned)a, (unsigned)q, nm ? nm : "(unnamed)",
                           ents[q].bank, ents[q].vol);
                }
                free(ents);
            }
            for (uint32_t q = 0; q < s->num_strm; q++) {
                if (!s->strm[q].present) continue;
                const char *nm = sdat_strm_name(s, q);
                DataSource ds = sdat_strm_source(s, q);
                printf("  STRM %4u  %-24s %u bytes\n", (unsigned)q, nm ? nm : "(unnamed)", (unsigned)ds.size);
            }
        }
    } else {
        int si = atoi(argv[2]);
        uint32_t qi = (uint32_t)strtoul(argv[3], NULL, 10);
        SdatSeqBundle b;
        if (si < 0 || si >= m.num_sdats || sdat_load_seq(&m.sdat[si], qi, &b) != 0) {
            fprintf(stderr, "couldn't load sequence %u from SDAT %d\n", (unsigned)qi, si);
            nds_music_close(&m);
            return 1;
        }
        dump(argv[4], "seq.sseq", b.sseq, b.sseq_size);
        dump(argv[4], "bank.sbnk", b.sbnk, b.sbnk_size);
        for (int k = 0; k < SDAT_MAX_WAVEARCS; k++) {
            char nm[32];
            snprintf(nm, sizeof nm, "wave%d.swar", k);
            if (b.swar[k]) dump(argv[4], nm, b.swar[k], b.swar_size[k]);
        }
        sdat_free_seq(&b);
    }
    nds_music_close(&m);
    return 0;
}

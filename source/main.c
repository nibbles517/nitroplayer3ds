/*
 * NDS Music Player - plays Nintendo DS music straight from .nds ROMs and from NCSF rips.
 *
 * Top screen: now playing.  Bottom screen: SD card browser / song list.
 */
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "audio.h"
#include "browser.h"
#include "ncsf.h"
#include "nds_music.h"
#include "song.h"
#include "twosf.h"

#define APP_VERSION "1.0.0"
#define CONFIG_DIR "sdmc:/3ds/ndsmusicplayer"
#define LASTDIR_FILE CONFIG_DIR "/lastdir.txt"
#define VOLUME_FILE CONFIG_DIR "/volume.txt"
#define VOLUME_DEFAULT 150 /* percent; 100 = DS level, which sits a bit under other 3DS audio */
#define VOLUME_MIN 10
#define VOLUME_MAX 400
#define VOLUME_STEP 10

#define TOP_W 50
#define TEXT_W 41 /* top rows 2-9 leave room for the game icon on the right */
#define ICON_X 336 /* top-screen pixel position of the 64x64 (2x) icon */
#define ICON_Y 16
#define ICON_SCALE 2
#define BOT_W 40
#define LIST_ROWS 27 /* bottom rows 1..27 */

/* ---- state ------------------------------------------------------------------------- */

typedef enum { VIEW_FILES, VIEW_SONGS } View;
typedef enum { PL_NONE, PL_ROM, PL_NCSF } PlaySrc;

typedef enum { ITEM_SEQ, ITEM_STRM, ITEM_SFX, ITEM_DSE } ItemKind;

typedef struct {
    uint8_t sdat;
    uint8_t kind;   /* ItemKind */
    uint16_t arc;   /* ITEM_SFX: sequence archive */
    uint16_t index; /* sequence / stream / archive entry */
    char label[64];
} SongItem;

static PrintConsole top_con, bot_con;
static View view = VIEW_FILES;
static bool dirty = true;

static DirList files;
static int file_cur, file_top;

static NdsMusic rom;
static bool rom_open;
static char rom_file[256];
static SongItem *songs;
static int songs_cap;
static int nsongs, song_cur, song_top;

static PlaySrc play_src = PL_NONE;
static int play_idx;
static DirList play_dir; /* folder the current NCSF came from, for next/prev */
static char now_title[128], now_sub[128], now_sub2[128], now_pos[64];
static uint32_t now_len_ms, now_fade_ms;
static bool repeat, paused;
static int volume = VOLUME_DEFAULT;
static bool volume_changed;
static char status[128];

static bool have_audio;
static int banner_lang = BANNER_EN;

/* Game icon / banner title of the open ROM */
static char rom_line1[BANNER_TITLE_MAX], rom_line2[BANNER_TITLE_MAX];
static unsigned rom_gen, icon_gen = ~0u;
static int icon_frame, icon_timer;

/* ---- helpers ----------------------------------------------------------------------- */

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    dirty = true;
}

/* Prints text at a row, clipped/padded to the console width, in an ANSI colour. */
static void put_line(int row, int width, const char *color, const char *text) {
    char buf[64];
    int n = (int)strlen(text);
    if (n > width) n = width;
    memcpy(buf, text, (size_t)n);
    memset(buf + n, ' ', (size_t)(width - n));
    buf[width] = 0;
    printf("\x1b[%d;1H%s%s\x1b[0m", row + 1, color, buf);
}

static void put_linef(int row, int width, const char *color, const char *fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    put_line(row, width, color, text);
}

static void fmt_time(char *out, size_t n, uint32_t ms) {
    uint32_t s = ms / 1000;
    snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void save_lastdir(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(LASTDIR_FILE, "w");
    if (f) {
        fputs(files.dir, f);
        fclose(f);
    }
}

static void save_volume(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(VOLUME_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", volume);
        fclose(f);
    }
}

static void load_volume(void) {
    FILE *f = fopen(VOLUME_FILE, "r");
    if (!f) return;
    int v;
    if (fscanf(f, "%d", &v) == 1 && v >= VOLUME_MIN && v <= VOLUME_MAX) volume = v;
    fclose(f);
}

static void change_volume(int delta) {
    int v = volume + delta;
    if (v < VOLUME_MIN) v = VOLUME_MIN;
    if (v > VOLUME_MAX) v = VOLUME_MAX;
    if (v == volume) return;
    volume = v;
    volume_changed = true;
    audio_set_volume(volume);
    set_status("Volume %d%%", volume);
}

static void load_start_dir(char *out, size_t n) {
    snprintf(out, n, "sdmc:/");
    FILE *f = fopen(LASTDIR_FILE, "r");
    if (!f) return;
    char line[BROWSER_PATH_MAX];
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "sdmc:/", 6) == 0) snprintf(out, n, "%s", line);
    }
    fclose(f);
}

/* ---- browsing ---------------------------------------------------------------------- */

static void enter_dir(const char *dir, const char *select_name) {
    DirList nl;
    if (!browser_list(&nl, dir)) {
        set_status("Can't open %s", dir);
        if (files.items) return;
        if (!browser_list(&nl, "sdmc:/")) return;
    }
    browser_free(&files);
    files = nl;
    file_cur = file_top = 0;
    if (select_name && *select_name) {
        for (int i = 0; i < files.count; i++)
            if (strcmp(files.items[i].name, select_name) == 0) file_cur = i;
    }
    dirty = true;
}

static void close_rom(void) {
    if (play_src == PL_ROM) { /* songs may stream from the ROM file */
        audio_play(NULL);
        play_src = PL_NONE;
        now_title[0] = now_sub[0] = now_sub2[0] = now_pos[0] = 0;
    }
    if (rom_open) nds_music_close(&rom);
    rom_open = false;
    free(songs);
    songs = NULL;
    nsongs = songs_cap = 0;
}

static SongItem *add_item(int sdat, ItemKind kind, uint32_t arc, uint32_t index) {
    if (index > 0xFFFF || arc > 0xFFFF) return NULL;
    if (nsongs == songs_cap) {
        int cap = songs_cap ? songs_cap * 2 : 256;
        SongItem *n = (SongItem *)realloc(songs, (size_t)cap * sizeof(SongItem));
        if (!n) return NULL;
        songs = n;
        songs_cap = cap;
    }
    SongItem *it = &songs[nsongs++];
    memset(it, 0, sizeof *it);
    it->sdat = (uint8_t)sdat;
    it->kind = (uint8_t)kind;
    it->arc = (uint16_t)arc;
    it->index = (uint16_t)index;
    return it;
}

static void open_rom(const char *path, const char *name) {
    close_rom();
    NdsMusicResult r = nds_music_open(&rom, path);
    if (r != NDSM_OK) {
        set_status("%s: %s", name, nds_music_strerror(r));
        return;
    }
    rom_open = true;
    rom_gen++;
    snprintf(rom_file, sizeof rom_file, "%s", name);

    /* Display title: first two lines of the banner title, else the 12-char header title. */
    rom_line2[0] = 0;
    if (rom.banner.valid && nds_banner_title(&rom.banner, banner_lang)[0]) {
        const char *t = nds_banner_title(&rom.banner, banner_lang);
        const char *nl = strchr(t, '\n');
        snprintf(rom_line1, sizeof rom_line1, "%.*s", nl ? (int)(nl - t) : (int)strlen(t), t);
        if (nl) {
            const char *nl2 = strchr(nl + 1, '\n');
            snprintf(rom_line2, sizeof rom_line2, "%.*s", nl2 ? (int)(nl2 - nl - 1) : (int)strlen(nl + 1), nl + 1);
        }
    } else {
        snprintf(rom_line1, sizeof rom_line1, "%s", rom.hdr.title);
    }

    /* Music (sequences, then streams) first, sound effects from sequence archives after. */
    int nmusic = 0, nsfx = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < rom.num_sdats; i++) {
            const Sdat *s = &rom.sdat[i];
            char pre[8] = "";
            if (rom.num_sdats > 1) snprintf(pre, sizeof pre, "%d:", i + 1);
            if (pass == 0) {
                for (uint32_t q = 0; q < s->num_seq; q++) {
                    if (!s->seq[q].present) continue;
                    const char *nm = sdat_seq_name(s, q);
                    SongItem *it = add_item(i, ITEM_SEQ, 0, q);
                    if (!it) goto oom;
                    snprintf(it->label, sizeof it->label, "%s%03u %s", pre, (unsigned)q, nm ? nm : "(sequence)");
                    nmusic++;
                }
                for (uint32_t q = 0; q < s->num_strm; q++) {
                    if (!s->strm[q].present) continue;
                    const char *nm = sdat_strm_name(s, q);
                    SongItem *it = add_item(i, ITEM_STRM, 0, q);
                    if (!it) goto oom;
                    snprintf(it->label, sizeof it->label, "%sS%02u %s", pre, (unsigned)q, nm ? nm : "(stream)");
                    nmusic++;
                }
            } else {
                for (uint32_t a = 0; a < s->num_seqarc; a++) {  /* sound effects */
                    SdatSsarEntry *ents;
                    uint32_t n = sdat_ssar_entries(s, a, &ents);
                    const char *an = sdat_ssar_name(s, a);
                    for (uint32_t q = 0; q < n; q++) {
                        if (!ents[q].valid) continue;
                        const char *nm = sdat_ssar_entry_name(s, a, q);
                        SongItem *it = add_item(i, ITEM_SFX, a, q);
                        if (!it) {
                            free(ents);
                            goto oom;
                        }
                        if (nm) snprintf(it->label, sizeof it->label, "%sE%02u.%03u %s", pre, (unsigned)a, (unsigned)q, nm);
                        else snprintf(it->label, sizeof it->label, "%sE%02u.%03u %s", pre, (unsigned)a, (unsigned)q,
                                      an ? an : "(effect)");
                        nsfx++;
                    }
                    free(ents);
                }
            }
        }
    /* Games on the DSE engine (Mystery Dungeon: Explorers): music from SOUND/BGM and SOUND/ME. */
    for (int i = 0; i < rom.dse.num; i++) {
        const DseSong *d = &rom.dse.songs[i];
        SongItem *it = add_item(0, ITEM_DSE, 0, (uint32_t)i);
        if (!it) goto oom;
        if (d->group == DSE_GROUP_ME) snprintf(it->label, sizeof it->label, "ME%02u %s", (unsigned)d->number, d->label);
        else snprintf(it->label, sizeof it->label, "%03u %s", (unsigned)d->number, d->label);
        nmusic++;
    }
    if (nsongs == 0) {
        close_rom();
        set_status("%s: no songs found", name);
        return;
    }
    song_cur = song_top = 0;
    view = VIEW_SONGS;
    if (nsfx) set_status("%d songs, %d sound effects", nmusic, nsfx);
    else set_status("%d songs", nmusic);
    return;

oom:
    close_rom();
    set_status("Out of memory");
}

/* ---- playback ---------------------------------------------------------------------- */

static int sfx_playing; /* effects currently layered over the music */

static void apply_timing(Song *g) {
    if (repeat) song_set_loops(g, 0, 0);
    else if (now_len_ms) song_set_length(g, now_len_ms, now_fade_ms);
    else song_set_loops(g, 2, 8000);
}

/* Effects play on top of whatever music is going, and don't touch the now-playing info. */
static bool play_rom_effect(int i) {
    const SongItem *it = &songs[i];
    Song *g = song_open_ssar(&rom.sdat[it->sdat], it->arc, it->index, AUDIO_RATE);
    if (!g) {
        set_status("Couldn't load %s", it->label);
        return false;
    }
    song_set_loops(g, 1, 1500); /* looping effects: once round, then a short fade */
    audio_play_sfx(g);
    set_status("Effect: %s", it->label);
    return true;
}

static bool play_rom_song(int i) {
    if (!rom_open || i < 0 || i >= nsongs) return false;
    const SongItem *it = &songs[i];
    if (it->kind == ITEM_SFX) return play_rom_effect(i);
    audio_play(NULL); /* stop the old track before loading the new one */
    const Sdat *s = rom.num_sdats ? &rom.sdat[it->sdat] : NULL;
    Song *g = it->kind == ITEM_DSE    ? song_open_dse(&rom.dse, it->index, AUDIO_RATE)
              : it->kind == ITEM_STRM ? song_open_strm(s, it->index, AUDIO_RATE)
                                      : song_open_seq(s, it->index, AUDIO_RATE);
    play_src = PL_ROM;
    play_idx = i;
    snprintf(now_title, sizeof now_title, "%s", it->label);
    snprintf(now_sub, sizeof now_sub, "%s", rom_line1);
    snprintf(now_sub2, sizeof now_sub2, "%s", rom_line2);
    snprintf(now_pos, sizeof now_pos, "Song %d / %d", i + 1, nsongs);
    now_len_ms = now_fade_ms = 0;
    dirty = true;
    if (!g) {
        set_status("Couldn't load %s", it->label);
        return false;
    }
    apply_timing(g);
    paused = false;
    audio_play(g);
    status[0] = 0;
    return true;
}

static bool play_ncsf(int i) {
    if (i < 0 || i >= play_dir.count || play_dir.items[i].kind != ENT_NCSF) return false;
    char path[BROWSER_PATH_MAX + 256], err[96];
    snprintf(path, sizeof path, "%s%s", play_dir.dir, play_dir.items[i].name);

    play_src = PL_NCSF;
    play_idx = i;
    snprintf(now_title, sizeof now_title, "%s", play_dir.items[i].name);
    now_sub[0] = now_sub2[0] = 0;
    snprintf(now_pos, sizeof now_pos, "File %d / %d", i + 1, play_dir.count);
    now_len_ms = now_fade_ms = 0;
    dirty = true;

    Ncsf n;
    if (ncsf_load(path, &n, err, sizeof err) != 0) {
        set_status("%s: %s", play_dir.items[i].name, err);
        return false;
    }
    Sdat sd;
    DataSource src = ds_mem(n.sdat, n.sdat_size);
    Song *g = NULL;
    if (sdat_open(&sd, &src) == 0) {
        g = song_open_seq(&sd, n.seq, AUDIO_RATE); /* copies what it needs */
        sdat_close(&sd);
    }
    if (n.title[0]) snprintf(now_title, sizeof now_title, "%s", n.title);
    if (n.game[0]) snprintf(now_sub, sizeof now_sub, "%s", n.game);
    now_len_ms = n.length_ms;
    now_fade_ms = n.length_ms ? n.fade_ms : 0;
    ncsf_free(&n);
    if (!g) {
        set_status("Couldn't load sequence from %s", play_dir.items[i].name);
        return false;
    }
    apply_timing(g);
    paused = false;
    audio_play(g);
    status[0] = 0;
    return true;
}

/* 2SF sets of DSE games: the library is streamed once, then kept until another one is needed. */
static TwoSfLib twosf;
static bool twosf_loaded;

static void draw_top(void);

static bool play_2sf(int i) {
    if (i < 0 || i >= play_dir.count || play_dir.items[i].kind != ENT_2SF) return false;
    char path[BROWSER_PATH_MAX + 256], libpath[BROWSER_PATH_MAX + 256], err[96];
    snprintf(path, sizeof path, "%s%s", play_dir.dir, play_dir.items[i].name);

    play_src = PL_NCSF;
    play_idx = i;
    snprintf(now_title, sizeof now_title, "%s", play_dir.items[i].name);
    now_sub[0] = now_sub2[0] = 0;
    snprintf(now_pos, sizeof now_pos, "File %d / %d", i + 1, play_dir.count);
    now_len_ms = now_fade_ms = 0;
    dirty = true;

    Mini2sf m;
    if (mini2sf_read(path, &m, err, sizeof err) != 0) {
        set_status("%s: %s", play_dir.items[i].name, err);
        return false;
    }
    snprintf(libpath, sizeof libpath, "%s%s", play_dir.dir, m.lib);
    if (!twosf_loaded || strcmp(twosf.path, libpath) != 0) {
        audio_play(NULL); /* the playing song may use the old library's samples */
        if (twosf_loaded) twosf_lib_free(&twosf);
        twosf_loaded = false;
        set_status("Loading %s ...", m.lib);
        draw_top();
        gfxFlushBuffers();
        gfxSwapBuffers();
        if (twosf_lib_load(&twosf, libpath, err, sizeof err) != 0) {
            set_status("%s: %s", m.lib, err);
            return false;
        }
        twosf_loaded = true;
    }
    int idx = dse_set_find(&twosf.dse, DSE_GROUP_BGM, m.number);
    Song *g = idx >= 0 ? song_open_dse(&twosf.dse, idx, AUDIO_RATE) : NULL;
    if (m.title[0]) snprintf(now_title, sizeof now_title, "%s", m.title);
    if (m.game[0]) snprintf(now_sub, sizeof now_sub, "%s", m.game);
    if (idx >= 0) snprintf(now_sub2, sizeof now_sub2, "bgm%04u %s", (unsigned)m.number, twosf.dse.songs[idx].label);
    now_len_ms = m.length_ms;
    now_fade_ms = m.length_ms ? m.fade_ms : 0;
    if (!g) {
        set_status(idx < 0 ? "Song %u isn't in this library" : "Couldn't load song %u", (unsigned)m.number);
        return false;
    }
    apply_timing(g);
    paused = false;
    audio_play(g);
    status[0] = 0;
    return true;
}

static bool play_file(int i) {
    if (i < 0 || i >= play_dir.count) return false;
    return play_dir.items[i].kind == ENT_2SF ? play_2sf(i) : play_ncsf(i);
}

static void start_ncsf_from_browser(int file_index) {
    if (play_src == PL_ROM) close_rom(); /* frees the ROM; file playback doesn't need it */
    browser_free(&play_dir);
    if (!browser_list(&play_dir, files.dir)) {
        set_status("Can't read folder");
        return;
    }
    for (int i = 0; i < play_dir.count; i++)
        if (strcmp(play_dir.items[i].name, files.items[file_index].name) == 0) {
            play_file(i);
            return;
        }
}

static void stop(void) {
    audio_play(NULL);
    audio_stop_sfx();
    play_src = PL_NONE;
    now_title[0] = now_sub[0] = now_sub2[0] = now_pos[0] = 0;
    dirty = true;
}

/* Step `dir` (+1/-1) through the current playlist, skipping entries that fail to load.
 * At the end of the list, automatic advancing stops playback; a button press does nothing. */
static void advance(int dir, bool automatic) {
    int idx = play_idx;
    for (int tries = 0; tries < 64; tries++) {
        int next = idx + dir;
        if (play_src == PL_ROM) { /* music only: effects are skipped */
            while (next >= 0 && next < nsongs && songs[next].kind == ITEM_SFX) next += dir;
            if (next < 0 || next >= nsongs) break;
            if (play_rom_song(next)) return;
        } else if (play_src == PL_NCSF) {
            while (next >= 0 && next < play_dir.count && play_dir.items[next].kind != ENT_NCSF &&
                   play_dir.items[next].kind != ENT_2SF)
                next += dir;
            if (next < 0 || next >= play_dir.count) break;
            if (play_file(next)) return;
        } else {
            return;
        }
        idx = next;
    }
    if (automatic) {
        stop();
        set_status("End of list");
    }
}

/* ---- drawing ----------------------------------------------------------------------- */

static void draw_top(void) {
    consoleSelect(&top_con);
    put_line(0, TOP_W, "\x1b[47;30m", " NDS Music Player v" APP_VERSION);

    uint32_t pos = 0;
    int loops = 0;
    uint16_t voices = 0;
    bool playing = false;
    audio_lock();
    Song *g = audio_current();
    if (g) {
        playing = true;
        pos = song_pos_ms(g);
        loops = song_loops(g);
        voices = song_voices(g);
    }
    audio_unlock();

    put_line(2, TEXT_W, "\x1b[36m", playing ? "Now playing" : (now_title[0] ? "Stopped" : "Nothing playing"));
    put_line(3, TEXT_W, "\x1b[1;37m", now_title);
    put_line(4, TEXT_W, "", now_sub);
    put_line(5, TEXT_W, "", now_sub2);
    put_line(6, TEXT_W, "", now_pos);

    char t[16], len[24] = "";
    fmt_time(t, sizeof t, pos);
    if (now_len_ms && !repeat) {
        char l[16];
        fmt_time(l, sizeof l, now_len_ms + now_fade_ms);
        snprintf(len, sizeof len, " / %s", l);
    }
    char fx[24] = "";
    if (sfx_playing) snprintf(fx, sizeof fx, "[+%d effect%s] ", sfx_playing, sfx_playing > 1 ? "s" : "");
    put_linef(7, TEXT_W, "\x1b[33m", "%s%s%s", fx, repeat ? "[Repeat] " : "", paused ? "[Paused]" : "");
    put_linef(8, TEXT_W, "", "Time %s%s   Loops %d", t, len, loops);

    char vbuf[20];
    for (int i = 0; i < 16; i++) vbuf[i] = (voices >> i) & 1 ? "0123456789ABCDEF"[i] : '.';
    vbuf[16] = 0;
    put_linef(9, TEXT_W, "\x1b[32m", "Voices %s  Vol %d%%", vbuf, volume);

    put_line(11, TOP_W, "\x1b[36m", "Controls");
    put_line(12, TOP_W, "", "  A       open / play (effects play over music)");
    put_line(13, TOP_W, "", "  B       back");
    put_line(14, TOP_W, "", "  L / R   previous / next song");
    put_line(15, TOP_W, "", "  Y       pause      X  stop all");
    put_line(16, TOP_W, "", "  SELECT  repeat song on/off");
    put_line(17, TOP_W, "", "  Circle Pad up/down or ZL/ZR  volume");
    put_line(18, TOP_W, "", "  D-pad Left/Right  page up/down   START  quit");

    if (!have_audio) put_line(26, TOP_W, "\x1b[31m", "No audio: dump DSP firmware (DSP1) first.");
    put_line(28, TOP_W, "\x1b[33m", status);
}

static void draw_list(void) {
    consoleSelect(&bot_con);
    int count, *cur, *topi;
    if (view == VIEW_FILES) {
        count = files.count;
        cur = &file_cur;
        topi = &file_top;
        char hdr[64];
        size_t dl = strlen(files.dir);
        snprintf(hdr, sizeof hdr, "%s", dl > BOT_W ? files.dir + dl - BOT_W : files.dir);
        put_line(0, BOT_W, "\x1b[47;30m", hdr);
    } else {
        count = nsongs;
        cur = &song_cur;
        topi = &song_top;
        put_linef(0, BOT_W, "\x1b[47;30m", "%s", rom_line1);
    }

    if (*cur >= count) *cur = count - 1;
    if (*cur < 0) *cur = 0;
    if (*cur < *topi) *topi = *cur;
    if (*cur >= *topi + LIST_ROWS) *topi = *cur - LIST_ROWS + 1;

    for (int r = 0; r < LIST_ROWS; r++) {
        int i = *topi + r;
        if (i >= count) {
            put_line(r + 1, BOT_W, "", "");
            continue;
        }
        char text[300];
        const char *color = "";
        bool sel = i == *cur;
        if (view == VIEW_FILES) {
            const Entry *e = &files.items[i];
            bool now = play_src == PL_NCSF && !strcmp(play_dir.dir, files.dir) && play_idx < play_dir.count &&
                       !strcmp(play_dir.items[play_idx].name, e->name);
            switch (e->kind) {
            case ENT_PARENT: snprintf(text, sizeof text, "%c ..", sel ? '>' : ' '); break;
            case ENT_DIR: snprintf(text, sizeof text, "%c [%s]", sel ? '>' : ' ', e->name); color = "\x1b[36m"; break;
            case ENT_ROM: snprintf(text, sizeof text, "%c %s", sel ? '>' : ' ', e->name); color = "\x1b[33m"; break;
            default: snprintf(text, sizeof text, "%c%c%s", sel ? '>' : ' ', now ? '*' : ' ', e->name); break;
            }
        } else {
            bool now = play_src == PL_ROM && play_idx == i;
            snprintf(text, sizeof text, "%c%c%s", sel ? '>' : ' ', now ? '*' : ' ', songs[i].label);
            if (songs[i].kind == ITEM_STRM) color = "\x1b[35m";
            else if (songs[i].kind == ITEM_SFX) color = "\x1b[32m";
        }
        put_line(r + 1, BOT_W, sel ? "\x1b[47;30m" : color, text);
    }
    put_linef(29, BOT_W - 1, "\x1b[36m", "%d/%d  A:open  B:back  START:quit", count ? *cur + 1 : 0, count);
}

/* ---- input ------------------------------------------------------------------------- */

static void activate(void) {
    if (view == VIEW_SONGS) {
        play_rom_song(song_cur);
        return;
    }
    if (files.count == 0) return;
    const Entry *e = &files.items[file_cur];
    char path[BROWSER_PATH_MAX + 256];
    switch (e->kind) {
    case ENT_PARENT: {
        char dir[BROWSER_PATH_MAX], child[256];
        snprintf(dir, sizeof dir, "%s", files.dir);
        browser_parent(dir, child, sizeof child);
        enter_dir(dir, child);
        break;
    }
    case ENT_DIR:
        snprintf(path, sizeof path, "%s%s/", files.dir, e->name);
        enter_dir(path, NULL);
        break;
    case ENT_ROM:
        if (rom_open && !strcmp(rom_file, e->name)) { /* reopen list of the ROM already loaded */
            view = VIEW_SONGS;
            dirty = true;
            break;
        }
        set_status("Scanning %s ...", e->name);
        draw_top();
        gfxFlushBuffers();
        gfxSwapBuffers();
        snprintf(path, sizeof path, "%s%s", files.dir, e->name);
        open_rom(path, e->name);
        break;
    case ENT_NCSF:
    case ENT_2SF:
        start_ncsf_from_browser(file_cur);
        break;
    }
}

static void back(void) {
    if (view == VIEW_SONGS) {
        view = VIEW_FILES;
        dirty = true;
        return;
    }
    if (browser_is_root(files.dir)) return;
    char dir[BROWSER_PATH_MAX], child[256];
    snprintf(dir, sizeof dir, "%s", files.dir);
    browser_parent(dir, child, sizeof child);
    enter_dir(dir, child);
}

static void move_cursor(int delta) {
    int *cur = view == VIEW_FILES ? &file_cur : &song_cur;
    int count = view == VIEW_FILES ? files.count : nsongs;
    if (count == 0) return;
    int n = *cur + delta;
    if (delta == 1 && n >= count) n = 0; /* single steps wrap around */
    else if (delta == -1 && n < 0) n = count - 1;
    if (n < 0) n = 0;
    if (n >= count) n = count - 1;
    *cur = n;
    dirty = true;
}

/* ---- game icon --------------------------------------------------------------------- */

static uint16_t bgr555_to_rgb565(int c) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (uint16_t)((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}

/* Draws straight into the top console's framebuffer (RGB565, rotated 90 degrees),
 * in a corner the text never touches. b == NULL clears the area. */
static void icon_paint(const NdsBanner *b, int frame) {
    u16 *fb = top_con.frameBuffer;
    const int size = BANNER_ICON_W * ICON_SCALE;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int c = b ? nds_banner_pixel(b, frame, x / ICON_SCALE, y / ICON_SCALE) : -1;
            fb[(ICON_X + x) * 240 + (239 - (ICON_Y + y))] = c < 0 ? 0 : bgr555_to_rgb565(c);
        }
}

/* Call once per 60 Hz frame: shows the open ROM's icon and steps DSi animations. */
static void icon_tick(void) {
    const NdsBanner *b = rom_open && rom.banner.valid ? &rom.banner : NULL;
    unsigned gen = b ? rom_gen : 0;
    if (gen != icon_gen) {
        icon_gen = gen;
        icon_frame = 0;
        icon_timer = b ? b->frame[0].duration : 0;
        icon_paint(b, 0);
        return;
    }
    if (!b || b->num_frames < 2) return;
    if (--icon_timer > 0) return;
    icon_frame = (icon_frame + 1) % b->num_frames;
    icon_timer = b->frame[icon_frame].duration;
    icon_paint(b, icon_frame);
}

static int system_banner_lang(void) {
    u8 lang = CFG_LANGUAGE_EN;
    if (R_FAILED(cfguInit())) return BANNER_EN;
    CFGU_GetSystemLanguage(&lang);
    cfguExit();
    switch (lang) {
    case CFG_LANGUAGE_JP: return BANNER_JP;
    case CFG_LANGUAGE_FR: return BANNER_FR;
    case CFG_LANGUAGE_DE: return BANNER_DE;
    case CFG_LANGUAGE_IT: return BANNER_IT;
    case CFG_LANGUAGE_ES: return BANNER_ES;
    case CFG_LANGUAGE_ZH: case CFG_LANGUAGE_TW: return BANNER_ZH;
    case CFG_LANGUAGE_KO: return BANNER_KO;
    default: return BANNER_EN;
    }
}

/* ---- main -------------------------------------------------------------------------- */

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, &top_con);
    consoleInit(GFX_BOTTOM, &bot_con);
    hidSetRepeatParameters(20, 5);
    banner_lang = system_banner_lang();

    have_audio = audio_init();
    load_volume();
    audio_set_volume(volume);

    char start[BROWSER_PATH_MAX];
    load_start_dir(start, sizeof start);
    enter_dir(start, NULL);
    if (!files.items) set_status("Can't read the SD card");

    int frame = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 rep = hidKeysDownRepeat();

        if (down & KEY_START) break;
        /* D-pad navigates; the Circle Pad (and ZL/ZR on New 3DS) changes volume. */
        if (rep & KEY_DDOWN) move_cursor(1);
        if (rep & KEY_DUP) move_cursor(-1);
        if (rep & KEY_DRIGHT) move_cursor(LIST_ROWS);
        if (rep & KEY_DLEFT) move_cursor(-LIST_ROWS);
        if (rep & (KEY_CPAD_UP | KEY_ZR)) change_volume(VOLUME_STEP);
        if (rep & (KEY_CPAD_DOWN | KEY_ZL)) change_volume(-VOLUME_STEP);
        if (down & KEY_A) activate();
        if (down & KEY_B) back();
        if (down & KEY_X) stop();
        if (down & KEY_Y) {
            paused = !paused;
            audio_set_paused(paused);
            dirty = true;
        }
        if (down & KEY_R) advance(1, false);
        if (down & KEY_L) advance(-1, false);
        if (down & KEY_SELECT) {
            repeat = !repeat;
            audio_lock();
            Song *g = audio_current();
            if (g) apply_timing(g);
            audio_unlock();
            dirty = true;
        }

        /* Auto-advance when the current song finishes. */
        audio_lock();
        Song *g = audio_current();
        bool done = g && song_done(g);
        audio_unlock();
        if (done) advance(1, true);

        int fx = audio_reap_sfx();
        if (fx != sfx_playing) {
            sfx_playing = fx;
            dirty = true;
        }

        if (dirty) draw_list();
        if (dirty || frame % 15 == 0) draw_top();
        icon_tick();
        dirty = false;
        frame++;

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    if (files.items) save_lastdir();
    if (volume_changed) save_volume();
    audio_play(NULL);
    if (have_audio) audio_exit();
    close_rom();
    if (twosf_loaded) twosf_lib_free(&twosf);
    browser_free(&files);
    browser_free(&play_dir);
    gfxExit();
    return 0;
}

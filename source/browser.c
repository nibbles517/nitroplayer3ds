#include "browser.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), m = strlen(ext);
    if (n <= m) return 0;
    for (size_t i = 0; i < m; i++)
        if (tolower((unsigned char)name[n - m + i]) != ext[i]) return 0;
    return 1;
}

static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
    }
}

static int entry_cmp(const void *pa, const void *pb) {
    const Entry *a = (const Entry *)pa, *b = (const Entry *)pb;
    int ra = a->kind == ENT_PARENT ? 0 : a->kind == ENT_DIR ? 1 : 2;
    int rb = b->kind == ENT_PARENT ? 0 : b->kind == ENT_DIR ? 1 : 2;
    if (ra != rb) return ra - rb;
    return ci_cmp(a->name, b->name);
}

bool browser_is_root(const char *dir) {
    const char *colon = strchr(dir, ':');
    return colon && strcmp(colon, ":/") == 0;
}

bool browser_list(DirList *out, const char *dir) {
    memset(out, 0, sizeof *out);
    snprintf(out->dir, sizeof out->dir, "%s", dir);
    size_t len = strlen(out->dir);
    if (len == 0 || out->dir[len - 1] != '/') {
        if (len + 1 >= sizeof out->dir) return false;
        out->dir[len] = '/';
        out->dir[len + 1] = 0;
    }

    DIR *d = opendir(out->dir);
    if (!d) return false;
    out->items = (Entry *)malloc(BROWSER_MAX_ENTRIES * sizeof(Entry));
    if (!out->items) {
        closedir(d);
        return false;
    }

    if (!browser_is_root(out->dir)) {
        strcpy(out->items[0].name, "..");
        out->items[0].kind = ENT_PARENT;
        out->count = 1;
    }

    struct dirent *de;
    while ((de = readdir(d)) && out->count < BROWSER_MAX_ENTRIES) {
        if (de->d_name[0] == '.') continue; /* hidden files, "." and ".." */
        Entry *e = &out->items[out->count];
        bool is_dir;
#ifdef DT_DIR
        if (de->d_type != DT_UNKNOWN) {
            is_dir = de->d_type == DT_DIR;
        } else
#endif
        {
            char full[BROWSER_PATH_MAX + 256];
            struct stat st;
            snprintf(full, sizeof full, "%s%s", out->dir, de->d_name);
            is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir) e->kind = ENT_DIR;
        else if (has_ext(de->d_name, ".nds") || has_ext(de->d_name, ".srl")) e->kind = ENT_ROM;
        else if (has_ext(de->d_name, ".ncsf") || has_ext(de->d_name, ".minincsf")) e->kind = ENT_NCSF;
        else if (has_ext(de->d_name, ".mini2sf") || has_ext(de->d_name, ".2sf")) e->kind = ENT_2SF;
        else continue;
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        out->count++;
    }
    closedir(d);
    qsort(out->items, (size_t)out->count, sizeof(Entry), entry_cmp);
    return true;
}

void browser_free(DirList *d) {
    free(d->items);
    d->items = NULL;
    d->count = 0;
}

void browser_parent(char *dir, char *child, int child_len) {
    if (child && child_len) child[0] = 0;
    if (browser_is_root(dir)) return;
    size_t len = strlen(dir);
    if (len && dir[len - 1] == '/') dir[--len] = 0;
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    if (child && child_len) snprintf(child, (size_t)child_len, "%s", slash + 1);
    slash[1] = 0;
}

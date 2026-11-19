/*
 * browser.h - SD card directory listing (folders + .nds/.ncsf/.minincsf/.2sf/.mini2sf files).
 */
#ifndef NDSMUSIC_BROWSER_H
#define NDSMUSIC_BROWSER_H

#include <stdbool.h>

#define BROWSER_MAX_ENTRIES 4096
#define BROWSER_PATH_MAX 768

typedef enum { ENT_PARENT, ENT_DIR, ENT_ROM, ENT_NCSF, ENT_2SF } EntryKind;

typedef struct {
    char name[256];
    EntryKind kind;
} Entry;

typedef struct {
    char dir[BROWSER_PATH_MAX]; /* always ends with '/' */
    Entry *items;
    int count;
} DirList;

/* Lists `dir` (sorted: "..", folders, then files). false if the folder can't be opened. */
bool browser_list(DirList *out, const char *dir);
void browser_free(DirList *d);

/* "sdmc:/a/b/" -> "sdmc:/a/" (no-op at the root). Returns the removed folder name in `child` if non-NULL. */
void browser_parent(char *dir, char *child, int child_len);
bool browser_is_root(const char *dir);

#endif

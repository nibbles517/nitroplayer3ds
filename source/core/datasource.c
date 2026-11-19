#include "datasource.h"

#include <stdlib.h>
#include <string.h>

static void (*io_lock)(void);
static void (*io_unlock)(void);

void ds_set_io_lock(void (*lock)(void), void (*unlock)(void)) {
    io_lock = lock;
    io_unlock = unlock;
}

DataSource ds_mem(const void *data, uint32_t size) {
    DataSource ds;
    memset(&ds, 0, sizeof ds);
    ds.kind = DS_MEM;
    ds.mem = (const uint8_t *)data;
    ds.size = data ? size : 0;
    return ds;
}

DataSource ds_file(FILE *fp) {
    DataSource ds;
    memset(&ds, 0, sizeof ds);
    ds.kind = DS_FILE;
    ds.fp = fp;
    if (!fp || fseek(fp, 0, SEEK_END) != 0) return ds;
    long end = ftell(fp);
    if (end < 0 || (unsigned long)end > 0xFFFFFFFFul) return ds;
    ds.size = (uint32_t)end;
    return ds;
}

DataSource ds_sub(const DataSource *parent, uint32_t off, uint32_t size) {
    DataSource ds = *parent;
    if (off > parent->size) {
        ds.size = 0;
        return ds;
    }
    if (size > parent->size - off) size = parent->size - off;
    ds.base = parent->base + off;
    ds.size = size;
    return ds;
}

int ds_read(const DataSource *ds, uint32_t off, void *dst, uint32_t len) {
    if (off > ds->size || len > ds->size - off) return -1;
    if (len == 0) return 0;
    if (ds->kind == DS_MEM) {
        memcpy(dst, ds->mem + ds->base + off, len);
        return 0;
    }
    if (io_lock) io_lock();
    int ok = fseek(ds->fp, (long)(ds->base + off), SEEK_SET) == 0 && fread(dst, 1, len, ds->fp) == len;
    if (io_unlock) io_unlock();
    return ok ? 0 : -1;
}

uint8_t *ds_load(const DataSource *ds, uint32_t off, uint32_t len) {
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) return NULL;
    if (ds_read(ds, off, buf, len) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

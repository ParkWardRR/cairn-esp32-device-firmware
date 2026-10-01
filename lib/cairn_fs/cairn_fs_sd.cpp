/*
 * Arduino SD backend. Compiled only for the device; on the host this file is
 * empty and cairn_fs_posix.c provides the implementation instead.
 */

#ifdef ARDUINO

#include "cairn_fs.h"

#include <Arduino.h>
#include <SD.h>

#include <string.h>

struct cairn_file {
    File f;
};

struct cairn_dir {
    File d;
};

/* The card is mounted by the caller (SD.begin in setup), because the CS pin is
 * board configuration rather than filesystem policy. */
bool cairn_fs_begin(const char *root)
{
    (void)root;
    return true;
}

void cairn_fs_end(void) { }

bool cairn_fs_mkdir(const char *path)
{
    if (SD.exists(path)) return true;
    return SD.mkdir(path);
}

bool cairn_fs_exists(const char *path)   { return SD.exists(path); }
bool cairn_fs_remove(const char *path)   { return SD.remove(path); }
bool cairn_fs_rmdir(const char *path)    { return SD.rmdir(path); }

bool cairn_fs_rename(const char *from, const char *to)
{
    /* Refuse to clobber: a sealed bundle is never mutated, and FatFs rename
     * semantics over an existing name are not something to rely on. */
    if (SD.exists(to)) return false;
    return SD.rename(from, to);
}

cairn_file_t *cairn_fs_open(const char *path, cairn_fs_mode_t mode)
{
    const char *flags = FILE_READ;
    switch (mode) {
    case CAIRN_FS_WRITE:  flags = FILE_WRITE;  break;
    case CAIRN_FS_APPEND: flags = FILE_APPEND; break;
    case CAIRN_FS_READ:
    default:              flags = FILE_READ;   break;
    }

    File f = SD.open(path, flags);
    if (!f) return nullptr;

    cairn_file_t *h = new cairn_file_t;
    h->f = f;
    return h;
}

size_t cairn_fs_read(cairn_file_t *f, void *buf, size_t len)
{
    int got = f->f.read((uint8_t *)buf, len);
    return (got > 0) ? (size_t)got : 0;
}

size_t cairn_fs_write(cairn_file_t *f, const void *buf, size_t len)
{
    return f->f.write((const uint8_t *)buf, len);
}

bool cairn_fs_seek(cairn_file_t *f, uint64_t offset)
{
    return f->f.seek((uint32_t)offset);
}

uint64_t cairn_fs_size(cairn_file_t *f) { return (uint64_t)f->f.size(); }

bool cairn_fs_flush(cairn_file_t *f)
{
    f->f.flush();
    return true;
}

void cairn_fs_close(cairn_file_t *f)
{
    if (f == nullptr) return;
    f->f.close();
    delete f;
}

bool cairn_fs_file_size(const char *path, uint64_t *size)
{
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    *size = (uint64_t)f.size();
    f.close();
    return true;
}

cairn_dir_t *cairn_fs_opendir(const char *path)
{
    File d = SD.open(path);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return nullptr;
    }

    cairn_dir_t *h = new cairn_dir_t;
    h->d = d;
    return h;
}

bool cairn_fs_readdir(cairn_dir_t *d, char *name, size_t cap, bool *is_dir,
                      uint64_t *size)
{
    File entry = d->d.openNextFile();
    if (!entry) return false;

    /*
     * Whether name() is absolute has changed between Arduino core versions, so
     * the basename is taken here once rather than guessed at every call site.
     */
    const char *nm = entry.name();
    const char *base = strrchr(nm, '/');
    base = (base != nullptr) ? base + 1 : nm;

    snprintf(name, cap, "%s", base);
    if (is_dir != nullptr) *is_dir = entry.isDirectory();
    if (size != nullptr)   *size = (uint64_t)entry.size();

    entry.close();
    return true;
}

void cairn_fs_closedir(cairn_dir_t *d)
{
    if (d == nullptr) return;
    d->d.close();
    delete d;
}

bool cairn_fs_space(uint64_t *total, uint64_t *used)
{
    *total = SD.totalBytes();
    *used  = SD.usedBytes();
    return *total > 0;
}

#endif /* ARDUINO */

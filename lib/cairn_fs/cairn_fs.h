/*
 * A minimal filesystem abstraction, with an Arduino SD backend for the device
 * and a POSIX backend for host tests.
 *
 * This exists for one reason: the storage layer makes claims that are only
 * worth anything if they are tested. "A torn tail is truncated to the last
 * valid frame with an exact discarded byte count" and "an interrupted seal is
 * completed idempotently at the next boot" are properties about crash
 * behaviour, and they cannot be exercised by compiling for ESP32 and hoping.
 * Behind this interface the same cairn_store.c runs on the device and under a
 * host test that can tear a file mid-frame and re-open it.
 *
 * The surface is deliberately small and synchronous — exactly what the store
 * needs and nothing more. It is not a VFS.
 */

#ifndef CAIRN_FS_H
#define CAIRN_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cairn_file cairn_file_t;
typedef struct cairn_dir  cairn_dir_t;

typedef enum {
    CAIRN_FS_READ,   /* fails if absent */
    CAIRN_FS_WRITE,  /* truncates or creates */
    CAIRN_FS_APPEND, /* creates if absent, writes at the end */
} cairn_fs_mode_t;

/* Mount point prefix, so the POSIX backend can root the tree in a temp dir
 * while the device uses absolute card paths. Pass NULL on the device. */
bool cairn_fs_begin(const char *root);
void cairn_fs_end(void);

bool cairn_fs_mkdir(const char *path);
bool cairn_fs_exists(const char *path);
bool cairn_fs_remove(const char *path);
bool cairn_fs_rmdir(const char *path);

/*
 * Rename, used to move a sealed bundle directory. Must fail rather than
 * overwrite if the destination exists: a sealed bundle is never mutated.
 */
bool cairn_fs_rename(const char *from, const char *to);

cairn_file_t *cairn_fs_open(const char *path, cairn_fs_mode_t mode);

/* Returns bytes transferred, which may be short; callers treat a short count
 * as a failure because the length is always known in advance. */
size_t   cairn_fs_read(cairn_file_t *f, void *buf, size_t len);
size_t   cairn_fs_write(cairn_file_t *f, const void *buf, size_t len);
bool     cairn_fs_seek(cairn_file_t *f, uint64_t offset);
uint64_t cairn_fs_size(cairn_file_t *f);
bool     cairn_fs_flush(cairn_file_t *f);
void     cairn_fs_close(cairn_file_t *f);

/* Size of a path without opening it for I/O. */
bool cairn_fs_file_size(const char *path, uint64_t *size);

cairn_dir_t *cairn_fs_opendir(const char *path);

/*
 * Next entry. `name` receives the basename only — the Arduino core has changed
 * whether name() is absolute between versions, and a store that guessed would
 * build paths like /cairn/bundles//cairn/bundles/X.
 */
bool cairn_fs_readdir(cairn_dir_t *d, char *name, size_t cap, bool *is_dir,
                      uint64_t *size);
void cairn_fs_closedir(cairn_dir_t *d);

/* Total and used bytes, for the free-space floor that protects capture data. */
bool cairn_fs_space(uint64_t *total, uint64_t *used);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_FS_H */

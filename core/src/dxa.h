/*
 * Reader for DX Library archives, version 8 (the .wolf files of Wolf RPG Editor 3.x).
 * The format is described in docs/behaviour/archive.md; this file is written from that
 * description, not from any official binary.
 *
 * The archive key string is an input. It is derived from the game when the archive is opened
 * and is never stored (#858). Every size and offset read from the archive is checked before use.
 */
#ifndef ENGINEHOST_WOLF_DXA_H
#define ENGINEHOST_WOLF_DXA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DXA_OK = 0,
    DXA_E_IO = -1,          /* the source could not be read */
    DXA_E_FORMAT = -2,      /* not a DX archive, or a size or offset is out of range */
    DXA_E_VERSION = -3,     /* a DX archive, but not version 8 */
    DXA_E_UNSUPPORTED = -4, /* a version 8 feature this reader does not handle (wide code pages) */
    DXA_E_NOMEM = -5,
    DXA_E_NOT_FOUND = -6,
    DXA_E_TOO_BIG = -7,     /* over the index or file size cap */
    DXA_E_NAME = -8,        /* a refused name: "..", absolute, or a too-long key string */
    DXA_E_IS_DIR = -9,
};

/* Reads len bytes at offset into buf. Returns 0 when all of them were read. */
typedef int (*dxa_read_fn)(void *ctx, uint64_t offset, void *buf, size_t len);

typedef struct {
    dxa_read_fn read;
    void *ctx;
    uint64_t size;
} dxa_source;

typedef struct dxa dxa;

/* A file found by dxa_find: where its entry and its directory are in the index. */
typedef struct {
    uint64_t entry;     /* offset of the file entry in the file table */
    uint64_t directory; /* offset of the enclosing directory in the directory table */
    uint64_t size;      /* the file's decoded size */
    int is_dir;
} dxa_file;

/*
 * Opens an archive. key is the archive's key string (key_len bytes; at most 63 are used, as the
 * library does). A NULL key means the library's default key string.
 */
int dxa_open(const dxa_source *src, const char *key, size_t key_len, dxa **out);
void dxa_close(dxa *a);

/* The code page the archive's names are in, after the whitelist (anything else is 932). */
int dxa_code_page(const dxa *a);

/* Finds a path inside the archive, separators / or \, matched as the library does (ASCII
 * letters without regard to case). */
int dxa_find(const dxa *a, const char *path, dxa_file *out);

/* Reads and decodes a file into a new buffer the caller frees with free(). */
int dxa_read(const dxa *a, const dxa_file *f, uint8_t **data, size_t *len);

/* Calls visit for every file and directory, with the path as written, separated by /.
 * A non-zero return from visit stops the walk and is returned. */
typedef int (*dxa_visit_fn)(void *ctx, const char *path, const dxa_file *f);
int dxa_walk(const dxa *a, dxa_visit_fn visit, void *ctx);

const char *dxa_error(int code);

/* Exposed for tests. */
uint32_t dxa_crc32(const uint8_t *p, size_t n);
void dxa_make_key(const uint8_t *s, size_t n, uint8_t key[7]);

#ifdef __cplusplus
}
#endif

#endif

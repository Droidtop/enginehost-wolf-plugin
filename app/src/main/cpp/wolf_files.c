/* See wolf_files.h. */
#include "wolf_files.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dxa.h"
#include "wolf_open.h"

#define ARCHIVES_MAX 64
#define NAME_MAX_LEN 1024

typedef struct {
    char top[256];  /* the folder the archive stands for ("" for Data.wolf) */
    int fd;
    dxa *a;         /* NULL: there is none, or it cannot be opened */
} archive;

struct wolf_files {
    char folder[4096];
    char data[4096]; /* the Data folder, or "" */
    archive archives[ARCHIVES_MAX];
    int narchives;
};

static int pread_all(void *ctx, uint64_t offset, void *buf, size_t len) {
    int fd = *(int *)ctx;
    size_t at = 0;
    while (at < len) {
        ssize_t r = pread(fd, (uint8_t *)buf + at, len - at, (off_t)(offset + at));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        at += (size_t)r;
    }
    return 0;
}

wolf_files *wolf_files_open(const char *folder) {
    wolf_files *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    snprintf(f->folder, sizeof f->folder, "%s", folder);
    if (wolf_find_ci(folder, "Data", S_IFDIR, f->data, sizeof f->data) != 0) f->data[0] = 0;
    return f;
}

void wolf_files_close(wolf_files *f) {
    if (!f) return;
    for (int i = 0; i < f->narchives; i++) {
        if (f->archives[i].a) dxa_close(f->archives[i].a);
        if (f->archives[i].fd >= 0) close(f->archives[i].fd);
    }
    free(f);
}

/* The archive standing for folder top ("" for Data.wolf), opened once; NULL when there is none. */
static dxa *archive_for(wolf_files *f, const char *top) {
    for (int i = 0; i < f->narchives; i++)
        if (strcasecmp(f->archives[i].top, top) == 0) return f->archives[i].a;
    if (f->narchives == ARCHIVES_MAX) return NULL;
    archive *ar = &f->archives[f->narchives++];
    snprintf(ar->top, sizeof ar->top, "%s", top);
    ar->fd = -1;
    ar->a = NULL;
    char name[300], path[4096];
    int found;
    if (!*top) {
        found = wolf_find_ci(f->folder, "Data.wolf", S_IFREG, path, sizeof path) == 0;
    } else {
        snprintf(name, sizeof name, "%s.wolf", top);
        found = f->data[0] && wolf_find_ci(f->data, name, S_IFREG, path, sizeof path) == 0;
    }
    if (!found) return NULL;
    ar->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (ar->fd < 0) return NULL;
    struct stat st;
    if (fstat(ar->fd, &st) != 0) return NULL;
    dxa_source src = {pread_all, &ar->fd, (uint64_t)st.st_size};
    /* the library's default key string, as wolf_open.c opens BasicData (#858) */
    if (dxa_open(&src, NULL, 0, &ar->a) != DXA_OK) ar->a = NULL;
    return ar->a;
}

/* A loose file: each part of the name found in any case. */
static int loose(wolf_files *f, const char *name, uint8_t **data, size_t *len) {
    if (!f->data[0]) return 1;
    char dir[4096], next[4096], part[256];
    snprintf(dir, sizeof dir, "%s", f->data);
    const char *p = name;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n == 0 || n >= sizeof part) return 1;
        memcpy(part, p, n);
        part[n] = 0;
        if (wolf_find_ci(dir, part, slash ? S_IFDIR : S_IFREG, next, sizeof next) != 0) return 1;
        memcpy(dir, next, sizeof dir);
        if (!slash) break;
        p = slash + 1;
    }
    return wolf_slurp(dir, data, len) == 0 ? 0 : -1;
}

static int from_archive(dxa *a, const char *path, uint8_t **data, size_t *len) {
    dxa_file file;
    int r = dxa_find(a, path, &file);
    if (r == DXA_E_NOT_FOUND || r == DXA_E_IS_DIR || r == DXA_E_NAME) return 1;
    if (r == DXA_OK) r = dxa_read(a, &file, data, len);
    return r == DXA_OK ? 0 : -1;
}

int wolf_files_read(wolf_files *f, const char *name_in, uint8_t **data, size_t *len) {
    *data = NULL;
    *len = 0;
    size_t n = strlen(name_in);
    if (n == 0 || n >= NAME_MAX_LEN) return -1;
    char name[NAME_MAX_LEN];
    for (size_t i = 0; i <= n; i++) name[i] = name_in[i] == '\\' ? '/' : name_in[i];
    /* the manual's refused sequences, absolute names and drive letters (risks.md) */
    if (name[0] == '/' || strstr(name, "//") || strstr(name, "..") || strstr(name, "./") || strchr(name, ':') ||
        strchr(name, '%') || strchr(name, '*') || strchr(name, '?') || strchr(name, '"') || strchr(name, '<') ||
        strchr(name, '>') || strchr(name, '|'))
        return -1;
    int r = loose(f, name, data, len);
    if (r != 1) return r;
    const char *slash = strchr(name, '/');
    if (slash) {
        char top[256];
        size_t tn = (size_t)(slash - name);
        if (tn < sizeof top) {
            memcpy(top, name, tn);
            top[tn] = 0;
            dxa *a = archive_for(f, top);
            /* inside a folder's archive the folder's own name may or may not lead the path */
            if (a && (r = from_archive(a, slash + 1, data, len)) != 1) return r;
            if (a && (r = from_archive(a, name, data, len)) != 1) return r;
        }
    }
    dxa *all = archive_for(f, "");
    if (all) return from_archive(all, name, data, len);
    return 1;
}

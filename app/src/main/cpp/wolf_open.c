/*
 * See wolf_open.h. Where a game keeps its data and what is refused: Enginehost
 * docs/new-engines/wolf.md, and docs/behaviour/formats.md and archive.md on the engine branch.
 */
#include "wolf_open.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dxa.h"
#include "wolf_read.h"

typedef struct {
    const char *folder;
    wolf_open_log_fn log;
    void *log_ctx;
    char *why;
    size_t why_size;
    /* The project data: an unpacked folder (dir) or an archive (archive, with prefix inside it). */
    char dir[4096];
    int archive_fd;
    dxa *archive;
    const char *prefix;
} opener;

static void say(opener *o, const char *fmt, ...) {
    if (!o->log) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    o->log(o->log_ctx, line);
}

static int refuse(opener *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o->why, o->why_size, fmt, ap);
    va_end(ap);
    say(o, "not starting: %s", o->why);
    return -1;
}

/*
 * The entry of dir named name in any letter case (the games come from Windows, whose names are
 * not case-sensitive), as a full path in out. 0 when found; kind is S_IFDIR or S_IFREG.
 */
int wolf_find_ci(const char *dir, const char *name, unsigned kind, char *out, size_t out_size) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int found = -1;
    struct dirent *e;
    while (found && (e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, name) != 0) continue;
        if ((size_t)snprintf(out, out_size, "%s/%s", dir, e->d_name) >= out_size) continue;
        struct stat st;
        if (stat(out, &st) == 0 && (st.st_mode & S_IFMT) == kind) found = 0;
    }
    closedir(d);
    return found;
}

/* A whole file into a new buffer. 0, or an errno. */
int wolf_slurp(const char *path, uint8_t **data, size_t *len) {
    *data = NULL;
    *len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    struct stat st;
    int err = 0;
    if (fstat(fd, &st) != 0) {
        err = errno;
    } else if (st.st_size < 0 || (uint64_t)st.st_size > WOLF_READ_UNPACKED_MAX) {
        err = EFBIG;
    } else {
        size_t n = (size_t)st.st_size;
        uint8_t *p = malloc(n ? n : 1);
        size_t at = 0;
        if (!p) err = ENOMEM;
        while (!err && at < n) {
            ssize_t r = read(fd, p + at, n - at);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) err = r < 0 ? errno : EIO;
            else at += (size_t)r;
        }
        if (err) free(p);
        else { *data = p; *len = n; }
    }
    close(fd);
    return err;
}

static int archive_read(void *ctx, uint64_t offset, void *buf, size_t len) {
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

/*
 * One project file by name (BasicData's own name for it). 0 with the bytes; 1 when the game has
 * no such file; -1 with why set when it is there but cannot be read.
 */
static int fetch(opener *o, const char *name, uint8_t **data, size_t *len) {
    if (!o->archive) {
        char path[4096];
        if (wolf_find_ci(o->dir, name, S_IFREG, path, sizeof path) != 0) return 1;
        int err = wolf_slurp(path, data, len);
        if (err) return refuse(o, "%s could not be read (%s).", name, strerror(err));
        return 0;
    }
    char path[256];
    snprintf(path, sizeof path, "%s%s", o->prefix, name);
    dxa_file f;
    int r = dxa_find(o->archive, path, &f);
    if (r == DXA_E_NOT_FOUND) return 1;
    if (r == DXA_OK) r = dxa_read(o->archive, &f, data, len);
    if (r != DXA_OK) return refuse(o, "%s in the game's archive could not be read (%s).", name, dxa_error(r));
    return 0;
}

static const char *read_error(int r) {
    switch (r) {
    case WOLF_READ_E_FORMAT: return "it is not laid out the way this core reads it yet";
    case WOLF_READ_E_NOMEM: return "out of memory";
    case WOLF_READ_E_TOO_BIG: return "it is larger than this core accepts";
    default: return "unknown error";
    }
}

/* What a loader said, as the sentence for the person. */
static int load_failed(opener *o, const char *name, int r) {
    if (r == WOLF_READ_E_PROTECTED)
        return refuse(o, "%s is protected (WOLF RPG Editor Pro, or the 2.x editor's scrambler). "
                         "Enginehost does not run protected Wolf games.", name);
    return refuse(o, "%s could not be loaded: %s.", name, read_error(r));
}

static int open_archive(opener *o, const char *path, const char *label) {
    o->archive_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (o->archive_fd < 0) return refuse(o, "%s could not be opened (%s).", label, strerror(errno));
    struct stat st;
    if (fstat(o->archive_fd, &st) != 0) return refuse(o, "%s could not be read (%s).", label, strerror(errno));
    dxa_source src = {archive_read, &o->archive_fd, (uint64_t)st.st_size};
    /* The library's own default key string, the one an archive without a game key opens with.
     * A game's key is derived when the archive is opened and never stored (#858); how is still
     * open, so an archive built on any other key is refused below rather than guessed at. */
    int r = dxa_open(&src, NULL, 0, &o->archive);
    if (r == DXA_E_VERSION)
        return refuse(o, "%s is a WOLF RPG Editor 2.x archive. This core reads unpacked data and 3.x "
                         "archives so far.", label);
    if (r == DXA_E_FORMAT)
        return refuse(o, "%s could not be opened without a game key. It may be built with a protect key, "
                         "which Enginehost does not run, or with a key this core cannot derive yet.", label);
    if (r != DXA_OK) return refuse(o, "%s could not be opened (%s).", label, dxa_error(r));
    say(o, "project data: %s (code page %d)", label, dxa_code_page(o->archive));
    return 0;
}

/* Finds where the project data is: unpacked first, as the runtime prefers loose files. */
static int locate(opener *o, wolf_open_game *g) {
    char path[4096], data[4096];
    if (wolf_find_ci(o->folder, "GamePro.exe", S_IFREG, path, sizeof path) == 0)
        return refuse(o, "This is a WOLF RPG Editor Pro game, which uses Wolf's protection. Enginehost "
                         "does not run it.");
    int has_data = wolf_find_ci(o->folder, "Data", S_IFDIR, data, sizeof data) == 0;
    if (has_data && wolf_find_ci(data, "BasicData", S_IFDIR, o->dir, sizeof o->dir) == 0) {
        snprintf(g->source, sizeof g->source, "Data/BasicData");
        say(o, "project data: Data/BasicData (unpacked)");
        return 0;
    }
    if (wolf_find_ci(o->folder, "Data.wolf", S_IFREG, path, sizeof path) == 0) {
        snprintf(g->source, sizeof g->source, "Data.wolf");
        o->prefix = "BasicData/";
        return open_archive(o, path, "Data.wolf");
    }
    if (has_data && wolf_find_ci(data, "BasicData.wolf", S_IFREG, path, sizeof path) == 0) {
        snprintf(g->source, sizeof g->source, "Data/BasicData.wolf");
        o->prefix = "";
        return open_archive(o, path, "Data/BasicData.wolf");
    }
    return refuse(o, "No WOLF RPG Editor data was found: the folder has no Data/BasicData and no Data.wolf.");
}

/* A file the game cannot start without. */
static int need(opener *o, const char *name, uint8_t **data, size_t *len) {
    int r = fetch(o, name, data, len);
    if (r == 1) {
        /* Data.wolf may keep BasicData's files at its root. */
        if (o->archive && o->prefix[0]) {
            const char *prefix = o->prefix;
            o->prefix = "";
            r = fetch(o, name, data, len);
            if (r == 0) say(o, "%s: found at the archive's root", name);
            else o->prefix = prefix;
        }
        if (r == 1) return refuse(o, "The game's data has no %s.", name);
    }
    return r;
}

static int load_db(opener *o, wolf_open_game *g, int which, const char *base) {
    char project_name[64], dat_name[64];
    snprintf(project_name, sizeof project_name, "%s.project", base);
    snprintf(dat_name, sizeof dat_name, "%s.dat", base);
    uint8_t *project = NULL, *dat = NULL;
    size_t project_len = 0, dat_len = 0;
    int r = need(o, project_name, &project, &project_len);
    if (r == 0) r = need(o, dat_name, &dat, &dat_len);
    if (r == 0) {
        r = wolf_db_load(project, project_len, dat, dat_len, &g->db[which]);
        if (r != WOLF_READ_OK) r = load_failed(o, dat_name, r);
    }
    free(project);
    free(dat);
    if (r) return r;
    const wolf_db *db = &g->db[which];
    uint32_t rows = 0;
    for (uint32_t i = 0; i < db->ntypes; i++) rows += db->types[i].nrows;
    say(o, "%s: %u types, %u rows (%s)", base, db->ntypes, rows, db->utf8 ? "UTF-8" : "Shift-JIS");
    return 0;
}

/* A text for the log: UTF-8 as is, anything else with its non-ASCII bytes escaped. */
static void printable(const char *s, int utf8, char *out, size_t size) {
    size_t at = 0;
    for (; *s && at + 5 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if (utf8 || (c >= 0x20 && c < 0x7F)) out[at++] = (char)c;
        else at += (size_t)snprintf(out + at, size - at, "\\x%02X", c);
    }
    out[at] = 0;
}

int wolf_open(const char *folder, wolf_open_game *g, char *why, size_t why_size, wolf_open_log_fn log,
              void *log_ctx) {
    memset(g, 0, sizeof *g);
    opener o = {.folder = folder, .log = log, .log_ctx = log_ctx, .why = why, .why_size = why_size,
                .archive_fd = -1};
    if (why_size) why[0] = 0;
    uint8_t *p = NULL;
    size_t n = 0;
    int r = locate(&o, g);

    if (r == 0) r = need(&o, "Game.dat", &p, &n);
    if (r == 0) {
        r = wolf_game_load(p, n, &g->game);
        free(p);
        p = NULL;
        if (r != WOLF_READ_OK) r = load_failed(&o, "Game.dat", r);
    }
    if (r == 0) {
        char title[256];
        printable(wolf_game_title(&g->game), g->game.utf8, title, sizeof title);
        wolf_game_screen(&g->game, &g->width, &g->height);
        say(&o, "Game.dat: \"%s\", version 0x%X, %s, screen %ux%u", title, g->game.version,
            g->game.utf8 ? "UTF-8" : "Shift-JIS", g->width, g->height);
        r = need(&o, "CommonEvent.dat", &p, &n);
    }
    if (r == 0) {
        r = wolf_commons_load(p, n, &g->commons);
        free(p);
        p = NULL;
        if (r != WOLF_READ_OK) r = load_failed(&o, "CommonEvent.dat", r);
    }
    if (r == 0) {
        uint32_t automatic = 0, parallel = 0;
        for (uint32_t i = 0; i < g->commons.count; i++) {
            uint8_t run = g->commons.events[i].run;
            if (run == 1) automatic++;
            else if (run == 2 || run == 3) parallel++;
        }
        say(&o, "CommonEvent.dat: %u common events (%u automatic, %u parallel), version 0x%X%s",
            g->commons.count, automatic, parallel, g->commons.version, g->commons.packed ? ", packed" : "");
        r = load_db(&o, g, WOLF_OPEN_DB_USER, "DataBase");
    }
    if (r == 0) r = load_db(&o, g, WOLF_OPEN_DB_CHANGEABLE, "CDataBase");
    if (r == 0) r = load_db(&o, g, WOLF_OPEN_DB_SYSTEM, "SysDatabase");

    if (o.archive) dxa_close(o.archive);
    if (o.archive_fd >= 0) close(o.archive_fd);
    if (r) wolf_open_free(g);
    return r ? -1 : 0;
}

void wolf_open_free(wolf_open_game *g) {
    wolf_game_free(&g->game);
    wolf_commons_free(&g->commons);
    for (int i = 0; i < WOLF_OPEN_DB_COUNT; i++) wolf_db_free(&g->db[i]);
    memset(g, 0, sizeof *g);
}

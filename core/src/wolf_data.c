/* The project-file loaders. Layouts: docs/behaviour/formats.md. */
#include "wolf_data.h"

#include <stdlib.h>
#include <string.h>

#define COMMON_EVENTS_MAX 100000u
#define DB_TYPES_MAX 10000u
#define DB_FIELDS_MAX 10000u
#define DB_ROWS_MAX 1000000u
#define SELF_NAMES 100u
#define TILESETS_MAX 10000u
#define AUTOTILES_MAX 256u
#define MAP_TREE_MAX 100000u

static int fail_code(const wolf_reader *r, int fallback) { return r->failed ? WOLF_READ_E_FORMAT : fallback; }

/* ---- CommonEvent.dat --------------------------------------------------------------------- */

static const uint8_t COMMON_MAGIC[9] = {0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x43, 0x00};

static void free_event(wolf_common_event *e) {
    free(e->name);
    wolf_list_free(&e->list);
    free(e->defaults);
}

static int read_event(wolf_reader *r, int packed, wolf_common_event *e) {
    memset(e, 0, sizeof *e);
    e->return_self = -1;
    wolf_expect_u8(r, 0x8E);
    e->number = wolf_read_u32(r);
    uint32_t cond = wolf_read_u8(r);
    e->run = (uint8_t)(cond & 0x0F);
    e->condition_op = (uint8_t)(cond >> 4);
    e->condition_var = wolf_read_s32(r);
    e->condition_value = wolf_read_s32(r);
    e->number_args = (uint8_t)wolf_read_u8(r);
    e->text_args = (uint8_t)wolf_read_u8(r);
    e->name = wolf_read_text(r, NULL);
    if (r->failed) return WOLF_READ_E_FORMAT;
    size_t used = 0;
    int lr = wolf_list_parse_ex(r->p + r->at, r->n - r->at, packed, &used, &e->list);
    if (lr == WOLF_LIST_E_NOMEM) return WOLF_READ_E_NOMEM;
    if (lr) return WOLF_READ_E_FORMAT;
    r->at += used;
    free(wolf_read_text(r, NULL)); /* meaning Unknown */
    free(wolf_read_text(r, NULL)); /* memo */
    wolf_expect_u8(r, 0x8F);
    wolf_skip_texts(r); /* argument names */
    wolf_skip_bytes(r); /* argument kinds */
    uint32_t n = wolf_read_count(r, WOLF_READ_LIST_MAX, 4);
    for (uint32_t i = 0; i < n && !r->failed; i++) wolf_skip_texts(r); /* choices */
    n = wolf_read_count(r, WOLF_READ_LIST_MAX, 4);
    for (uint32_t i = 0; i < n && !r->failed; i++) wolf_skip_numbers(r); /* choice values */
    e->ndefaults = wolf_read_count(r, WOLF_READ_LIST_MAX, 4);
    if (r->failed) return WOLF_READ_E_FORMAT;
    e->defaults = calloc(e->ndefaults + 1u, sizeof(int32_t));
    if (!e->defaults) return WOLF_READ_E_NOMEM;
    for (uint32_t i = 0; i < e->ndefaults; i++) e->defaults[i] = wolf_read_s32(r);
    wolf_expect_u8(r, 0x90);
    wolf_read_s32(r); /* colour */
    for (uint32_t i = 0; i < SELF_NAMES && !r->failed; i++) free(wolf_read_text(r, NULL));
    wolf_expect_u8(r, 0x91);
    free(wolf_read_text(r, NULL)); /* meaning Unknown */
    uint32_t sep = wolf_read_u8(r);
    if (sep == 0x92) {
        free(wolf_read_text(r, NULL)); /* the return value's name */
        int32_t self = wolf_read_s32(r);
        wolf_expect_u8(r, 0x92);
        if (self >= 0 && self < (int32_t)SELF_NAMES) e->return_self = self;
    } else if (sep != 0x91) {
        r->failed = 1;
    }
    return fail_code(r, WOLF_READ_OK);
}

static int load_commons(wolf_reader *r, int packed, wolf_commons *out) {
    out->count = wolf_read_count(r, COMMON_EVENTS_MAX, 16);
    if (r->failed) return WOLF_READ_E_FORMAT;
    out->events = calloc(out->count + 1u, sizeof *out->events);
    out->table = calloc(out->count + 1u, sizeof *out->table);
    if (!out->events || !out->table) return WOLF_READ_E_NOMEM;
    for (uint32_t i = 0; i < out->count; i++) {
        int er = read_event(r, packed, &out->events[i]);
        if (er) {
            out->count = i + 1; /* free what was read, the failed one included */
            return er;
        }
    }
    uint32_t footer = wolf_read_u8(r);
    if (r->failed || footer < 0x89 || r->at != r->n) return WOLF_READ_E_FORMAT;
    for (uint32_t i = 0; i < out->count; i++) {
        out->table[i].list = &out->events[i].list;
        out->table[i].name = out->events[i].name;
        out->table[i].return_self = out->events[i].return_self;
    }
    return WOLF_READ_OK;
}

int wolf_commons_load(const uint8_t *p, size_t n, wolf_commons *out) {
    memset(out, 0, sizeof *out);
    wolf_reader r;
    wolf_reader_init(&r, p, n);
    int hr = wolf_read_head(&r, COMMON_MAGIC, sizeof COMMON_MAGIC, 5, &out->utf8);
    if (hr) return hr;
    out->version = wolf_read_u8(&r);
    if (r.failed) return WOLF_READ_E_FORMAT;
    out->packed = out->version == 0x93 || out->version == 0xCC;
    int res;
    if (out->packed) {
        uint8_t *body = NULL;
        size_t body_len = 0;
        res = wolf_read_unpack(&r, &body, &body_len);
        if (res) return res;
        wolf_reader b;
        wolf_reader_init(&b, body, body_len);
        res = load_commons(&b, 1, out);
        free(body);
    } else {
        res = load_commons(&r, 0, out);
    }
    if (res) wolf_commons_free(out);
    return res;
}

void wolf_commons_free(wolf_commons *c) {
    if (c->events)
        for (uint32_t i = 0; i < c->count; i++) free_event(&c->events[i]);
    free(c->events);
    free(c->table);
    memset(c, 0, sizeof *c);
}

/* ---- Databases --------------------------------------------------------------------------- */

static const uint8_t DB_MAGIC[9] = {0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x4D, 0x00};

static void free_type(wolf_db_type *t) {
    free(t->name);
    if (t->fields)
        for (uint32_t i = 0; i < t->nfields; i++) free(t->fields[i].name);
    free(t->fields);
    if (t->rows)
        for (uint32_t i = 0; i < t->nrows; i++) {
            free(t->rows[i].name);
            free(t->rows[i].numbers);
            if (t->rows[i].texts)
                for (uint32_t k = 0; k < t->ntexts; k++) free(t->rows[i].texts[k]);
            free(t->rows[i].texts);
        }
    free(t->rows);
}

void wolf_db_free(wolf_db *db) {
    if (db->types)
        for (uint32_t i = 0; i < db->ntypes; i++) free_type(&db->types[i]);
    free(db->types);
    memset(db, 0, sizeof *db);
}

/* The schema half, from the .project. */
static int read_schema(wolf_reader *r, wolf_db_type *t) {
    t->name = wolf_read_text(r, NULL);
    t->nfields = wolf_read_count(r, DB_FIELDS_MAX, 5);
    if (r->failed) return WOLF_READ_E_FORMAT;
    t->fields = calloc(t->nfields + 1u, sizeof *t->fields);
    if (!t->fields) return WOLF_READ_E_NOMEM;
    for (uint32_t i = 0; i < t->nfields && !r->failed; i++) t->fields[i].name = wolf_read_text(r, NULL);
    t->nrows = wolf_read_count(r, DB_ROWS_MAX, 5);
    if (r->failed) return WOLF_READ_E_FORMAT;
    t->rows = calloc(t->nrows + 1u, sizeof *t->rows);
    if (!t->rows) return WOLF_READ_E_NOMEM;
    for (uint32_t i = 0; i < t->nrows && !r->failed; i++) t->rows[i].name = wolf_read_text(r, NULL);
    free(wolf_read_text(r, NULL)); /* memo */
    uint32_t kinds = wolf_read_count(r, WOLF_READ_LIST_MAX, 1);
    const uint8_t *k = wolf_read_bytes(r, kinds);
    for (uint32_t i = 0; k && i < kinds && i < t->nfields; i++) t->fields[i].kind = k[i];
    uint32_t n = wolf_read_count(r, WOLF_READ_LIST_MAX, 5);
    for (uint32_t i = 0; i < n && !r->failed; i++) free(wolf_read_text(r, NULL));
    n = wolf_read_count(r, WOLF_READ_LIST_MAX, 4);
    for (uint32_t i = 0; i < n && !r->failed; i++) wolf_skip_texts(r);
    n = wolf_read_count(r, WOLF_READ_LIST_MAX, 4);
    for (uint32_t i = 0; i < n && !r->failed; i++) wolf_skip_numbers(r);
    n = wolf_read_count(r, WOLF_READ_LIST_MAX, 4);
    for (uint32_t i = 0; i < n && !r->failed; i++) {
        int32_t v = wolf_read_s32(r);
        if (i < t->nfields) t->fields[i].def = v;
    }
    return fail_code(r, WOLF_READ_OK);
}

/* Resizes a type's fields or rows to what the .dat holds (names past the project's are ""). */
static int resize_fields(wolf_db_type *t, uint32_t nf) {
    if (nf == t->nfields) return WOLF_READ_OK;
    for (uint32_t i = nf; i < t->nfields; i++) free(t->fields[i].name);
    wolf_db_field *f = realloc(t->fields, (nf + 1u) * sizeof *f);
    if (!f) return WOLF_READ_E_NOMEM;
    for (uint32_t i = t->nfields; i < nf; i++) memset(&f[i], 0, sizeof f[i]);
    t->fields = f;
    t->nfields = nf;
    return WOLF_READ_OK;
}

static int resize_rows(wolf_db_type *t, uint32_t nr) {
    if (nr == t->nrows) return WOLF_READ_OK;
    for (uint32_t i = nr; i < t->nrows; i++) free(t->rows[i].name);
    wolf_db_row *rows = realloc(t->rows, (nr + 1u) * sizeof *rows);
    if (!rows) return WOLF_READ_E_NOMEM;
    for (uint32_t i = t->nrows; i < nr; i++) memset(&rows[i], 0, sizeof rows[i]);
    t->rows = rows;
    t->nrows = nr;
    return WOLF_READ_OK;
}

/* The value half, from the .dat. */
static int read_values(wolf_reader *r, wolf_db_type *t) {
    const uint8_t *sep = wolf_read_bytes(r, 4);
    if (!sep || sep[0] != 0xFE || sep[1] != 0xFF || sep[2] != 0xFF || sep[3] != 0xFF) return WOLF_READ_E_FORMAT;
    t->id_method = wolf_read_s32(r);
    uint32_t nf = wolf_read_count(r, DB_FIELDS_MAX, 4);
    if (r->failed) return WOLF_READ_E_FORMAT;
    int rr = resize_fields(t, nf);
    if (rr) return rr;
    t->nnumbers = t->ntexts = 0;
    for (uint32_t i = 0; i < nf; i++) {
        uint32_t pos = wolf_read_u32(r);
        uint32_t block = pos / 1000u, slot = pos % 1000u;
        if (block != 1 && block != 2) return WOLF_READ_E_FORMAT;
        t->fields[i].is_text = block == 2;
        t->fields[i].slot = slot;
        if (block == 2) t->ntexts++;
        else t->nnumbers++;
    }
    /* every slot must fall inside its kind's values */
    for (uint32_t i = 0; i < nf; i++)
        if (t->fields[i].slot >= (t->fields[i].is_text ? t->ntexts : t->nnumbers)) return WOLF_READ_E_FORMAT;
    size_t per_row = (size_t)t->nnumbers * 4 + (size_t)t->ntexts * 5;
    uint32_t nr = wolf_read_count(r, DB_ROWS_MAX, per_row);
    if (r->failed) return WOLF_READ_E_FORMAT;
    rr = resize_rows(t, nr);
    if (rr) return rr;
    for (uint32_t i = 0; i < nr; i++) {
        wolf_db_row *row = &t->rows[i];
        /* a type with no fields keeps no per-row storage, so a short file cannot ask for
         * a million allocations */
        if (t->nnumbers && !(row->numbers = calloc(t->nnumbers, sizeof(int32_t)))) return WOLF_READ_E_NOMEM;
        if (t->ntexts && !(row->texts = calloc(t->ntexts, sizeof(char *)))) return WOLF_READ_E_NOMEM;
        for (uint32_t k = 0; k < t->nnumbers; k++) row->numbers[k] = wolf_read_s32(r);
        for (uint32_t k = 0; k < t->ntexts && !r->failed; k++) row->texts[k] = wolf_read_text(r, NULL);
        if (r->failed) return WOLF_READ_E_FORMAT;
    }
    return WOLF_READ_OK;
}

static int read_dat(wolf_reader *r, uint32_t version, wolf_db *db) {
    uint32_t n = wolf_read_u32(r);
    if (r->failed || n != db->ntypes) return WOLF_READ_E_FORMAT;
    for (uint32_t i = 0; i < n; i++) {
        int vr = read_values(r, &db->types[i]);
        if (vr) return vr;
    }
    if (wolf_read_u8(r) != version || r->failed || r->at != r->n) return WOLF_READ_E_FORMAT;
    return WOLF_READ_OK;
}

int wolf_db_load(const uint8_t *project, size_t project_len, const uint8_t *dat, size_t dat_len, wolf_db *out) {
    memset(out, 0, sizeof *out);
    wolf_reader d;
    wolf_reader_init(&d, dat, dat_len);
    int res = wolf_read_head(&d, DB_MAGIC, sizeof DB_MAGIC, 5, &out->utf8);
    if (res) return res;
    out->version = wolf_read_u8(&d);
    if (d.failed) return WOLF_READ_E_FORMAT;

    wolf_reader p;
    wolf_reader_init(&p, project, project_len);
    out->ntypes = wolf_read_count(&p, DB_TYPES_MAX, 16);
    if (p.failed) return WOLF_READ_E_FORMAT;
    out->types = calloc(out->ntypes + 1u, sizeof *out->types);
    if (!out->types) return WOLF_READ_E_NOMEM;
    for (uint32_t i = 0; i < out->ntypes && !res; i++) res = read_schema(&p, &out->types[i]);
    if (!res && p.at != p.n) res = WOLF_READ_E_FORMAT; /* nothing trails the schema */
    if (!res) {
        if (out->version == 0xC4) {
            uint8_t *body = NULL;
            size_t body_len = 0;
            res = wolf_read_unpack(&d, &body, &body_len);
            if (!res) {
                wolf_reader b;
                wolf_reader_init(&b, body, body_len);
                /* the footer stays the version byte inside the unpacked body */
                res = read_dat(&b, out->version, out);
                free(body);
            }
        } else {
            res = read_dat(&d, out->version, out);
        }
    }
    if (res) wolf_db_free(out);
    return res;
}

static const wolf_db_field *db_field(const wolf_db *db, uint32_t type, uint32_t row, uint32_t field,
                                     const wolf_db_row **r) {
    if (type >= db->ntypes) return NULL;
    const wolf_db_type *t = &db->types[type];
    if (row >= t->nrows || field >= t->nfields) return NULL;
    *r = &t->rows[row];
    return &t->fields[field];
}

int wolf_db_number(const wolf_db *db, uint32_t type, uint32_t row, uint32_t field, int32_t *out) {
    const wolf_db_row *r;
    const wolf_db_field *f = db_field(db, type, row, field, &r);
    *out = 0;
    if (!f || f->is_text || !r->numbers) return 0;
    *out = r->numbers[f->slot];
    return 1;
}

const char *wolf_db_text(const wolf_db *db, uint32_t type, uint32_t row, uint32_t field) {
    const wolf_db_row *r;
    const wolf_db_field *f = db_field(db, type, row, field, &r);
    if (!f || !f->is_text || !r->texts) return NULL;
    return r->texts[f->slot];
}

int wolf_db_set_number(wolf_db *db, uint32_t type, uint32_t row, uint32_t field, int32_t value) {
    const wolf_db_row *r;
    const wolf_db_field *f = db_field(db, type, row, field, &r);
    if (!f || f->is_text || !r->numbers) return 0;
    r->numbers[f->slot] = value;
    return 1;
}

int wolf_db_set_text(wolf_db *db, uint32_t type, uint32_t row, uint32_t field, const char *value) {
    const wolf_db_row *r;
    const wolf_db_field *f = db_field(db, type, row, field, &r);
    if (!f || !f->is_text || !r->texts) return 0;
    size_t n = strlen(value);
    char *copy = malloc(n + 1);
    if (!copy) return 0;
    memcpy(copy, value, n + 1);
    free(r->texts[f->slot]);
    r->texts[f->slot] = copy;
    return 1;
}

static int same(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

int32_t wolf_db_type_by_name(const wolf_db *db, const char *name) {
    for (uint32_t i = 0; i < db->ntypes && i < INT32_MAX; i++)
        if (same(db->types[i].name, name)) return (int32_t)i;
    return -1;
}

int32_t wolf_db_row_by_name(const wolf_db *db, uint32_t type, const char *name) {
    if (type >= db->ntypes) return -1;
    const wolf_db_type *t = &db->types[type];
    for (uint32_t i = 0; i < t->nrows && i < INT32_MAX; i++)
        if (same(t->rows[i].name, name)) return (int32_t)i;
    return -1;
}

int32_t wolf_db_field_by_name(const wolf_db *db, uint32_t type, const char *name) {
    if (type >= db->ntypes) return -1;
    const wolf_db_type *t = &db->types[type];
    for (uint32_t i = 0; i < t->nfields && i < INT32_MAX; i++)
        if (same(t->fields[i].name, name)) return (int32_t)i;
    return -1;
}

/* ---- Game.dat ---------------------------------------------------------------------------- */

static const uint8_t GAME_MAGIC[8] = {0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x4D};

void wolf_game_free(wolf_game *g) {
    free(g->settings);
    if (g->texts)
        for (uint32_t i = 0; i < g->ntexts; i++) free(g->texts[i]);
    free(g->texts);
    free(g->words);
    memset(g, 0, sizeof *g);
}

int wolf_game_load(const uint8_t *p, size_t n, wolf_game *out) {
    memset(out, 0, sizeof *out);
    wolf_reader r;
    wolf_reader_init(&r, p, n);
    int ignored;
    int res = wolf_read_head(&r, GAME_MAGIC, sizeof GAME_MAGIC, -1, &ignored);
    if (res) return res;
    out->version = wolf_read_u8(&r);
    out->utf8 = out->version == 0x55;
    out->nsettings = wolf_read_count(&r, WOLF_READ_LIST_MAX, 1);
    const uint8_t *s = wolf_read_bytes(&r, out->nsettings);
    if (!s) return WOLF_READ_E_FORMAT;
    out->settings = malloc(out->nsettings + 1u);
    if (!out->settings) return WOLF_READ_E_NOMEM;
    memcpy(out->settings, s, out->nsettings);
    out->ntexts = wolf_read_count(&r, WOLF_READ_LIST_MAX, 5);
    if (r.failed) {
        wolf_game_free(out);
        return WOLF_READ_E_FORMAT;
    }
    out->texts = calloc(out->ntexts + 1u, sizeof(char *));
    if (!out->texts) {
        wolf_game_free(out);
        return WOLF_READ_E_NOMEM;
    }
    for (uint32_t i = 0; i < out->ntexts && !r.failed; i++) out->texts[i] = wolf_read_text(&r, NULL);
    if (r.failed || out->ntexts < 2 || strcmp(out->texts[1], "0000-0000") != 0) {
        wolf_game_free(out);
        return WOLF_READ_E_FORMAT;
    }
    out->file_size = wolf_read_s32(&r);
    wolf_read_s32(&r); /* meaning Unknown */
    out->nwords = wolf_read_count(&r, WOLF_READ_LIST_MAX, 2);
    if (r.failed) {
        wolf_game_free(out);
        return WOLF_READ_E_FORMAT;
    }
    out->words = calloc(out->nwords + 1u, sizeof(uint16_t));
    if (!out->words) {
        wolf_game_free(out);
        return WOLF_READ_E_NOMEM;
    }
    for (uint32_t i = 0; i < out->nwords; i++) out->words[i] = (uint16_t)wolf_read_u16(&r);
    if (r.failed) {
        wolf_game_free(out);
        return WOLF_READ_E_FORMAT;
    }
    return WOLF_READ_OK; /* any remaining bytes: meaning Unknown */
}

const char *wolf_game_title(const wolf_game *g) { return g->ntexts > 0 && g->texts[0] ? g->texts[0] : ""; }

void wolf_game_screen(const wolf_game *g, uint32_t *width, uint32_t *height) {
    uint32_t w = g->nwords > 16 ? g->words[16] : 0, h = g->nwords > 17 ? g->words[17] : 0;
    *width = w ? w : 320;
    *height = h ? h : 240;
}

void wolf_game_config(const wolf_game *g, wolf_config *cfg) {
    (void)g;
    memset(cfg, 0, sizeof *cfg);
    /* Unknown in the data (formats.md, "Game.dat"): fixed defaults until it is read. */
    cfg->normal = 100000u;
    cfg->spare = 10000u;
    cfg->strings = 10000u;
    cfg->system = 10000u;
    cfg->system_strings = 10000u;
    cfg->map_events = 10000u;
    cfg->seed = 1u;
}

/* ---- TileSetData.dat --------------------------------------------------------------------- */

#define TILESET_SEPARATOR 0xFF
#define TILESETS_END 0xCF

static void free_tileset(wolf_tileset *s) {
    free(s->name);
    free(s->base);
    if (s->autotiles)
        for (uint32_t i = 0; i < s->nautotiles; i++) free(s->autotiles[i]);
    free(s->autotiles);
    free(s->tags);
    free(s->flags);
}

void wolf_tilesets_free(wolf_tilesets *t) {
    if (t->sets)
        for (uint32_t i = 0; i < t->count; i++) free_tileset(&t->sets[i]);
    free(t->sets);
    memset(t, 0, sizeof *t);
}

static int read_tileset(wolf_reader *r, wolf_tileset *s) {
    s->name = wolf_read_text(r, NULL);
    s->base = wolf_read_text(r, NULL);
    if (r->failed) return WOLF_READ_E_FORMAT;
    /* The autotile sheets have no count: they run up to the separator (a text's length word
     * never starts with 0xFF, since no text is that long). */
    s->autotiles = calloc(AUTOTILES_MAX, sizeof(char *));
    if (!s->autotiles) return WOLF_READ_E_NOMEM;
    while (r->at < r->n && r->p[r->at] != TILESET_SEPARATOR) {
        if (s->nautotiles == AUTOTILES_MAX) return WOLF_READ_E_FORMAT;
        char *name = wolf_read_text(r, NULL);
        if (!name) return WOLF_READ_E_FORMAT;
        s->autotiles[s->nautotiles++] = name;
    }
    wolf_expect_u8(r, TILESET_SEPARATOR);
    s->nchips = wolf_read_count(r, WOLF_READ_LIST_MAX, 1);
    const uint8_t *tags = wolf_read_bytes(r, s->nchips);
    wolf_expect_u8(r, TILESET_SEPARATOR);
    uint32_t nflags = wolf_read_count(r, WOLF_READ_LIST_MAX, 4);
    if (r->failed || !tags || nflags != s->nchips) return WOLF_READ_E_FORMAT;
    s->tags = malloc(s->nchips + 1u);
    s->flags = calloc(s->nchips + 1u, sizeof(uint32_t));
    if (!s->tags || !s->flags) return WOLF_READ_E_NOMEM;
    memcpy(s->tags, tags, s->nchips);
    for (uint32_t i = 0; i < s->nchips; i++) s->flags[i] = wolf_read_u32(r);
    return fail_code(r, WOLF_READ_OK);
}

int wolf_tilesets_load(const uint8_t *p, size_t n, wolf_tilesets *out) {
    memset(out, 0, sizeof *out);
    wolf_reader r;
    wolf_reader_init(&r, p, n);
    int res = wolf_read_head(&r, DB_MAGIC, sizeof DB_MAGIC, 5, &out->utf8);
    if (res) return res;
    out->version = wolf_read_u8(&r);
    /* each tileset is at least two texts (5 bytes each), two separators and two counts */
    uint32_t count = wolf_read_count(&r, TILESETS_MAX, 20);
    if (r.failed) return WOLF_READ_E_FORMAT;
    out->sets = calloc(count + 1u, sizeof *out->sets);
    if (!out->sets) return WOLF_READ_E_NOMEM;
    out->count = count;
    for (uint32_t i = 0; i < count; i++) {
        res = read_tileset(&r, &out->sets[i]);
        if (res) break;
    }
    if (!res) {
        wolf_expect_u8(&r, TILESETS_END);
        res = r.failed || r.at != r.n ? WOLF_READ_E_FORMAT : WOLF_READ_OK;
    }
    if (res) wolf_tilesets_free(out);
    return res;
}

/* ---- MapTree.dat ------------------------------------------------------------------------- */

#define MAP_TREE_HEAD 10
#define MAP_TREE_END 0xBC

void wolf_map_tree_free(wolf_map_tree *t) {
    free(t->entries);
    memset(t, 0, sizeof *t);
}

int wolf_map_tree_load(const uint8_t *p, size_t n, wolf_map_tree *out) {
    memset(out, 0, sizeof *out);
    if (n >= 2 && p[1] == 0x50) return WOLF_READ_E_PROTECTED; /* as every other file: formats.md */
    wolf_reader r;
    wolf_reader_init(&r, p, n);
    wolf_read_bytes(&r, MAP_TREE_HEAD); /* meaning Unknown */
    out->version = wolf_read_u8(&r);
    uint32_t count = wolf_read_count(&r, MAP_TREE_MAX, 8);
    if (r.failed) return WOLF_READ_E_FORMAT;
    out->entries = calloc(count + 1u, sizeof *out->entries);
    if (!out->entries) return WOLF_READ_E_NOMEM;
    out->count = count;
    for (uint32_t i = 0; i < count; i++) {
        out->entries[i].parent = wolf_read_s32(&r);
        out->entries[i].map = wolf_read_s32(&r);
    }
    wolf_expect_u8(&r, MAP_TREE_END);
    if (r.failed || r.at != r.n) {
        wolf_map_tree_free(out);
        return WOLF_READ_E_FORMAT;
    }
    return WOLF_READ_OK;
}

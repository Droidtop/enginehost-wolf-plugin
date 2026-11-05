/* Tests for the project-file loaders. Every file is built here from the layouts in
 * docs/behaviour/formats.md; no game data is used. */
#include "../src/vm.h"
#include "../src/wolf_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { uint8_t *p; size_t n, cap; } buf;
static void put(buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void u8(buf *b, uint32_t v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void u16(buf *b, uint32_t v) { uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; put(b, x, 2); }
static void u32(buf *b, uint32_t v) { uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; put(b, x, 4); }
static void text(buf *b, const char *s) { uint32_t n = (uint32_t)strlen(s) + 1; u32(b, n); put(b, s, n); }

/* The simplest LZ4 block that holds src: one sequence of literals only. */
static void lz4_literals(buf *b, const uint8_t *src, size_t n) {
    if (n < 15) {
        u8(b, (uint32_t)n << 4);
    } else {
        u8(b, 0xF0);
        size_t rest = n - 15;
        while (rest >= 255) { u8(b, 255); rest -= 255; }
        u8(b, (uint32_t)rest);
    }
    put(b, src, n);
}

/* head (indicator, magic, version) then a packed body. */
static buf pack(const uint8_t *head, size_t head_len, const uint8_t *body, size_t body_len) {
    buf blk = {0}, out = {0};
    lz4_literals(&blk, body, body_len);
    put(&out, head, head_len);
    u32(&out, (uint32_t)body_len);
    u32(&out, (uint32_t)blk.n);
    put(&out, blk.p, blk.n);
    free(blk.p);
    return out;
}

static uint32_t rng = 12345;
static uint32_t next_rand(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* ---- LZ4 --------------------------------------------------------------------------------- */

static void test_lz4(void) {
    uint8_t out[32];
    /* 4 literals, a match of 8 at offset 4, then one literal */
    const uint8_t a[] = {0x44, 'a', 'b', 'c', 'd', 4, 0, 0x10, 'x'};
    CHECK(wolf_lz4_block(a, sizeof a, out, 13) == 0 && memcmp(out, "abcdabcdabcdx", 13) == 0);
    /* an overlapping match: offset 1 repeats the last byte */
    const uint8_t b[] = {0x10, 'z', 1, 0};
    CHECK(wolf_lz4_block(b, sizeof b, out, 5) == 0 && memcmp(out, "zzzzz", 5) == 0);
    /* a long match length, extended */
    const uint8_t c[] = {0x1F, 'q', 1, 0, 3};
    CHECK(wolf_lz4_block(c, sizeof c, out, 1 + 4 + 15 + 3) == 0 && out[22] == 'q');
    CHECK(wolf_lz4_block(a, sizeof a, out, 12) != 0);           /* output size must be exact */
    CHECK(wolf_lz4_block(a, sizeof a, out, 14) != 0);
    const uint8_t zero_off[] = {0x10, 'z', 0, 0};
    CHECK(wolf_lz4_block(zero_off, sizeof zero_off, out, 5) != 0);
    const uint8_t far[] = {0x10, 'z', 2, 0};                       /* before the output's start */
    CHECK(wolf_lz4_block(far, sizeof far, out, 5) != 0);
    const uint8_t short_lit[] = {0x50, 'a', 'b'};
    CHECK(wolf_lz4_block(short_lit, sizeof short_lit, out, 5) != 0);
    for (size_t k = 0; k < sizeof a; k++) CHECK(wolf_lz4_block(a, k, out, 13) != 0);
    /* literal-only, long */
    uint8_t big[600];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)i;
    buf blk = {0};
    lz4_literals(&blk, big, sizeof big);
    uint8_t back[600];
    CHECK(wolf_lz4_block(blk.p, blk.n, back, sizeof back) == 0 && memcmp(big, back, sizeof big) == 0);
    free(blk.p);
}

/* ---- CommonEvent.dat --------------------------------------------------------------------- */

/* One command (ints, one optional string); extras adds the 3.5+ trailing byte list. */
static void cmd(buf *b, int32_t code, const int32_t *ints, int nint, const char *str, int extras) {
    u8(b, (uint32_t)nint + 1);
    u32(b, (uint32_t)code);
    for (int i = 0; i < nint; i++) u32(b, (uint32_t)ints[i]);
    u8(b, 0); /* indent */
    u8(b, str ? 1 : 0);
    if (str) text(b, str);
    u8(b, 0);
    if (extras) { u8(b, 2); u8(b, 0xAA); u8(b, 0xBB); }
}

static void event(buf *b, uint32_t number, const char *name, int return_self, int extras,
                  void (*list)(buf *, int)) {
    u8(b, 0x8E);
    u32(b, number);
    u8(b, 0x21); /* run 1, operator 2 */
    u32(b, 2000003);
    u32(b, 9);
    u8(b, 1);
    u8(b, 0);
    text(b, name);
    list(b, extras);
    text(b, "");
    text(b, "memo");
    u8(b, 0x8F);
    u32(b, 1); text(b, "arg");      /* argument names */
    u32(b, 1); u8(b, 0);            /* argument kinds */
    u32(b, 1); u32(b, 1); text(b, "choice");
    u32(b, 1); u32(b, 1); u32(b, 5);
    u32(b, 2); u32(b, 11); u32(b, (uint32_t)-4); /* defaults */
    u8(b, 0x90);
    u32(b, 0xFFFFFF);
    for (int i = 0; i < 100; i++) text(b, i == 1 ? "result" : "");
    u8(b, 0x91);
    text(b, "");
    if (return_self >= 0) {
        u8(b, 0x92);
        text(b, "result");
        u32(b, (uint32_t)return_self);
        u8(b, 0x92);
    } else {
        u8(b, 0x91);
    }
}

/* self 1 = self 0 + 1, then stop */
static void adder_list(buf *b, int extras) {
    u32(b, 2);
    const int32_t set[] = {1600001, 1600000, 1, 0};
    cmd(b, 121, set, 4, NULL, extras);
    cmd(b, 0, NULL, 0, NULL, extras);
}
static void empty_list(buf *b, int extras) {
    u32(b, 1);
    cmd(b, 0, NULL, 0, NULL, extras);
}

static const uint8_t COMMON_HEAD[] = {0x00, 0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x43, 0x00};

static buf commons_body(int extras) {
    buf b = {0};
    u32(&b, 2);
    event(&b, 0, "Adder", 1, extras, adder_list);
    event(&b, 1, "Nothing", -1, extras, empty_list);
    u8(&b, 0x8F);
    return b;
}

static buf commons_plain(void) {
    buf f = {0};
    put(&f, COMMON_HEAD, sizeof COMMON_HEAD);
    u8(&f, 0x90);
    buf body = commons_body(0);
    put(&f, body.p, body.n);
    free(body.p);
    return f;
}

static buf commons_packed(const uint8_t *body, size_t n) {
    uint8_t head[sizeof COMMON_HEAD + 1];
    memcpy(head, COMMON_HEAD, sizeof COMMON_HEAD);
    head[6] = 0x55; /* UTF-8 */
    head[sizeof COMMON_HEAD] = 0xCC;
    return pack(head, sizeof head, body, n);
}

#define V(x) (2000000 + (x))

static void check_calls(const wolf_commons *c) {
    typedef struct { int32_t code; int nint; int32_t ints[4]; const char *str; } st;
    const st s[] = {
        {210, 4, {500000, 0x01000001, 7, V(5)}, NULL},
        {300, 4, {0, 0x01000001, 40, V(7)}, "Adder"},
        {210, 2, {500001, 0}, NULL},
    };
    buf b = {0};
    u32(&b, 3);
    for (int i = 0; i < 3; i++) cmd(&b, s[i].code, s[i].ints, s[i].nint, s[i].str, 0);
    wolf_list l;
    size_t used;
    CHECK(wolf_list_parse(b.p, b.n, &used, &l) == WOLF_LIST_OK);
    free(b.p);
    wolf_config cfg;
    wolf_game_config(NULL, &cfg);
    wolf_host host = {0};
    wolf_vm *vm = wolf_vm_create(&host, &cfg);
    CHECK(vm != NULL);
    wolf_vm_set_common_events(vm, c->table, c->count);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, V(5)) == 8);
    CHECK(wolf_get(vm, NULL, V(7)) == 41);
    free(t);
    wolf_vm_destroy(vm);
    wolf_list_free(&l);
}

static void check_commons(const wolf_commons *c) {
    CHECK(c->count == 2);
    if (c->count != 2) return;
    const wolf_common_event *e = &c->events[0];
    CHECK(e->number == 0 && e->run == 1 && e->condition_op == 2);
    CHECK(e->condition_var == 2000003 && e->condition_value == 9);
    CHECK(e->number_args == 1 && e->text_args == 0);
    CHECK(strcmp(e->name, "Adder") == 0);
    CHECK(e->list.count == 2 && e->list.cmds[0].code == 121);
    CHECK(e->ndefaults == 2 && e->defaults[0] == 11 && e->defaults[1] == -4);
    CHECK(e->return_self == 1);
    CHECK(c->events[1].return_self == -1 && strcmp(c->events[1].name, "Nothing") == 0);
    CHECK(c->table[0].list == &e->list && c->table[1].return_self == -1);
    check_calls(c);
}

static int load_commons(const uint8_t *p, size_t n) {
    wolf_commons c;
    int r = wolf_commons_load(p, n, &c);
    if (r == WOLF_READ_OK) wolf_commons_free(&c);
    return r;
}

static void test_commons(void) {
    buf f = commons_plain();
    wolf_commons c;
    CHECK(wolf_commons_load(f.p, f.n, &c) == WOLF_READ_OK);
    CHECK(!c.utf8 && !c.packed && c.version == 0x90);
    check_commons(&c);
    wolf_commons_free(&c);

    /* loading copies: the file can go */
    uint8_t *copy = malloc(f.n);
    memcpy(copy, f.p, f.n);
    CHECK(wolf_commons_load(copy, f.n, &c) == WOLF_READ_OK);
    memset(copy, 0, f.n);
    free(copy);
    check_commons(&c);
    wolf_commons_free(&c);

    for (size_t k = 0; k < f.n; k++) CHECK(load_commons(f.p, k) != WOLF_READ_OK);
    buf longer = {0};
    put(&longer, f.p, f.n);
    u8(&longer, 0);
    CHECK(load_commons(longer.p, longer.n) == WOLF_READ_E_FORMAT); /* nothing after the footer */
    free(longer.p);

    /* Pro and the scrambler are refused */
    uint8_t *m = malloc(f.n);
    memcpy(m, f.p, f.n);
    m[1] = 0x50;
    CHECK(load_commons(m, f.n) == WOLF_READ_E_PROTECTED);
    memcpy(m, f.p, f.n);
    m[0] = 0x0A;
    CHECK(load_commons(m, f.n) == WOLF_READ_E_PROTECTED);

    for (int i = 0; i < 20000; i++) {
        memcpy(m, f.p, f.n);
        int edits = 1 + (int)(next_rand() % 4);
        for (int e = 0; e < edits; e++) m[next_rand() % f.n] = (uint8_t)next_rand();
        load_commons(m, f.n);
    }
    free(m);
    free(f.p);

    /* packed, with the trailing byte lists */
    buf body = commons_body(1);
    buf pf = commons_packed(body.p, body.n);
    CHECK(wolf_commons_load(pf.p, pf.n, &c) == WOLF_READ_OK);
    CHECK(c.utf8 && c.packed && c.version == 0xCC);
    check_commons(&c);
    wolf_commons_free(&c);
    for (size_t k = 0; k < pf.n; k++) CHECK(load_commons(pf.p, k) != WOLF_READ_OK);
    for (size_t k = 0; k < body.n; k++) { /* a short body, packed correctly */
        buf t = commons_packed(body.p, k);
        CHECK(load_commons(t.p, t.n) != WOLF_READ_OK);
        free(t.p);
    }
    uint8_t *mb = malloc(body.n);
    for (int i = 0; i < 5000; i++) {
        memcpy(mb, body.p, body.n);
        mb[next_rand() % body.n] = (uint8_t)next_rand();
        buf t = commons_packed(mb, body.n);
        load_commons(t.p, t.n);
        free(t.p);
    }
    free(mb);
    /* an unpacked size beyond the cap is refused before allocating */
    uint8_t big[sizeof COMMON_HEAD + 1 + 8 + 1];
    memcpy(big, pf.p, sizeof COMMON_HEAD + 1);
    big[sizeof COMMON_HEAD + 1 + 0] = 0xFF; big[sizeof COMMON_HEAD + 1 + 1] = 0xFF;
    big[sizeof COMMON_HEAD + 1 + 2] = 0xFF; big[sizeof COMMON_HEAD + 1 + 3] = 0xFF;
    big[sizeof COMMON_HEAD + 1 + 4] = 1; big[sizeof COMMON_HEAD + 1 + 5] = 0;
    big[sizeof COMMON_HEAD + 1 + 6] = 0; big[sizeof COMMON_HEAD + 1 + 7] = 0;
    big[sizeof big - 1] = 0;
    CHECK(load_commons(big, sizeof big) == WOLF_READ_E_TOO_BIG);
    free(pf.p);
    free(body.p);
}

/* ---- Databases --------------------------------------------------------------------------- */

static buf db_project(uint32_t extra_rows) {
    buf b = {0};
    u32(&b, 2);
    /* type 0: three fields, two rows */
    text(&b, "Monsters");
    u32(&b, 3); text(&b, "HP"); text(&b, "Name"); text(&b, "MP");
    u32(&b, 2 + extra_rows); text(&b, "Slime"); text(&b, "Bat");
    for (uint32_t i = 0; i < extra_rows; i++) text(&b, "Extra");
    text(&b, "memo");
    u32(&b, 3); u8(&b, 0); u8(&b, 1); u8(&b, 3);
    u32(&b, 3); text(&b, ""); text(&b, ""); text(&b, "");
    u32(&b, 3); u32(&b, 0); u32(&b, 0); u32(&b, 1); text(&b, "a");
    u32(&b, 3); u32(&b, 0); u32(&b, 0); u32(&b, 1); u32(&b, 7);
    u32(&b, 3); u32(&b, 10); u32(&b, 0); u32(&b, 5);
    /* type 1: empty */
    text(&b, "Empty");
    u32(&b, 0); u32(&b, 0); text(&b, "");
    u32(&b, 0); u32(&b, 0); u32(&b, 0); u32(&b, 0); u32(&b, 0);
    return b;
}

static buf db_body(uint32_t version, uint32_t mp_pos) {
    buf b = {0};
    u32(&b, 2);
    u32(&b, 0xFFFFFFFEu);
    u32(&b, 1);
    u32(&b, 3); u32(&b, 1000); u32(&b, 2000); u32(&b, mp_pos);
    u32(&b, 2);
    u32(&b, 30); u32(&b, (uint32_t)-2); text(&b, "Slime");
    u32(&b, 50); u32(&b, 9); text(&b, "Bat");
    u32(&b, 0xFFFFFFFEu);
    u32(&b, 0);
    u32(&b, 0);
    u32(&b, 0);
    u8(&b, version);
    return b;
}

static const uint8_t DB_HEAD[] = {0x00, 0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x4D, 0x00};

static buf db_dat(uint32_t mp_pos) {
    buf f = {0};
    put(&f, DB_HEAD, sizeof DB_HEAD);
    u8(&f, 0xC1);
    buf body = db_body(0xC1, mp_pos);
    put(&f, body.p, body.n);
    free(body.p);
    return f;
}

static int load_db(const buf *p, const uint8_t *d, size_t dn) {
    wolf_db db;
    int r = wolf_db_load(p->p, p->n, d, dn, &db);
    if (r == WOLF_READ_OK) wolf_db_free(&db);
    return r;
}

static void check_db(const wolf_db *db) {
    CHECK(db->ntypes == 2);
    if (db->ntypes != 2) return;
    const wolf_db_type *t = &db->types[0];
    CHECK(strcmp(t->name, "Monsters") == 0 && t->nfields == 3 && t->nrows == 2);
    CHECK(t->nnumbers == 2 && t->ntexts == 1 && t->id_method == 1);
    CHECK(t->fields[0].kind == 0 && t->fields[1].kind == 1 && t->fields[2].kind == 3);
    CHECK(t->fields[0].def == 10 && t->fields[2].def == 5);
    CHECK(strcmp(t->rows[1].name, "Bat") == 0);
    int32_t v;
    CHECK(wolf_db_number(db, 0, 0, 0, &v) && v == 30);
    CHECK(wolf_db_number(db, 0, 0, 2, &v) && v == -2);
    CHECK(wolf_db_number(db, 0, 1, 2, &v) && v == 9);
    CHECK(strcmp(wolf_db_text(db, 0, 1, 1), "Bat") == 0);
    CHECK(!wolf_db_number(db, 0, 0, 1, &v) && v == 0); /* a text field */
    CHECK(wolf_db_text(db, 0, 0, 0) == NULL);
    CHECK(!wolf_db_number(db, 0, 2, 0, &v) && !wolf_db_number(db, 0, 0, 3, &v) && !wolf_db_number(db, 2, 0, 0, &v));
    CHECK(db->types[1].nfields == 0 && db->types[1].nrows == 0);
}

static void test_db(void) {
    buf p = db_project(0), d = db_dat(1001);
    wolf_db db;
    CHECK(wolf_db_load(p.p, p.n, d.p, d.n, &db) == WOLF_READ_OK);
    CHECK(db.version == 0xC1 && !db.utf8);
    check_db(&db);
    wolf_db_free(&db);

    /* the .dat is authoritative on counts */
    buf p3 = db_project(3);
    CHECK(wolf_db_load(p3.p, p3.n, d.p, d.n, &db) == WOLF_READ_OK);
    check_db(&db);
    wolf_db_free(&db);
    free(p3.p);

    /* a slot outside its kind, and an unknown block */
    buf bad = db_dat(1002);
    CHECK(load_db(&p, bad.p, bad.n) == WOLF_READ_E_FORMAT);
    free(bad.p);
    bad = db_dat(3000);
    CHECK(load_db(&p, bad.p, bad.n) == WOLF_READ_E_FORMAT);
    free(bad.p);

    for (size_t k = 0; k < d.n; k++) CHECK(load_db(&p, d.p, k) != WOLF_READ_OK);
    for (size_t k = 0; k < p.n; k++) {
        buf t = {p.p, k, k};
        CHECK(load_db(&t, d.p, d.n) != WOLF_READ_OK);
    }
    uint8_t *m = malloc(d.n);
    memcpy(m, d.p, d.n);
    m[1] = 0x50;
    CHECK(load_db(&p, m, d.n) == WOLF_READ_E_PROTECTED);
    memcpy(m, d.p, d.n);
    m[0] = 0x02;
    CHECK(load_db(&p, m, d.n) == WOLF_READ_E_PROTECTED);
    for (int i = 0; i < 20000; i++) {
        memcpy(m, d.p, d.n);
        m[next_rand() % d.n] = (uint8_t)next_rand();
        load_db(&p, m, d.n);
    }
    free(m);
    uint8_t *mp = malloc(p.n);
    for (int i = 0; i < 20000; i++) {
        memcpy(mp, p.p, p.n);
        mp[next_rand() % p.n] = (uint8_t)next_rand();
        buf t = {mp, p.n, p.n};
        load_db(&t, d.p, d.n);
    }
    free(mp);

    /* packed, UTF-8 */
    uint8_t head[sizeof DB_HEAD + 1];
    memcpy(head, DB_HEAD, sizeof DB_HEAD);
    head[6] = 0x55;
    head[sizeof DB_HEAD] = 0xC4;
    buf body = db_body(0xC4, 1001);
    buf pd = pack(head, sizeof head, body.p, body.n);
    CHECK(wolf_db_load(p.p, p.n, pd.p, pd.n, &db) == WOLF_READ_OK);
    CHECK(db.utf8 && db.version == 0xC4);
    check_db(&db);
    wolf_db_free(&db);
    for (size_t k = 0; k < body.n; k++) {
        buf t = pack(head, sizeof head, body.p, k);
        CHECK(load_db(&p, t.p, t.n) != WOLF_READ_OK);
        free(t.p);
    }
    free(pd.p);
    free(body.p);
    free(p.p);
    free(d.p);
}

/* ---- Game.dat ---------------------------------------------------------------------------- */

static buf game_file(const char *code, uint32_t width, uint32_t height) {
    static const uint8_t head[] = {0x00, 0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x4D};
    buf b = {0};
    put(&b, head, sizeof head);
    u8(&b, 0x55);
    u32(&b, 4); u8(&b, 16); u8(&b, 1); u8(&b, 1); u8(&b, 0);
    u32(&b, 3); text(&b, "A Test"); text(&b, code); text(&b, "");
    u32(&b, 1234);
    u32(&b, 0);
    u32(&b, 19);
    for (uint32_t i = 0; i < 19; i++) u16(&b, i == 16 ? width : i == 17 ? height : i);
    u8(&b, 0x77); /* the tail, not read */
    return b;
}

static int load_game(const uint8_t *p, size_t n) {
    wolf_game g;
    int r = wolf_game_load(p, n, &g);
    if (r == WOLF_READ_OK) wolf_game_free(&g);
    return r;
}

static void test_game(void) {
    buf f = game_file("0000-0000", 640, 480);
    wolf_game g;
    CHECK(wolf_game_load(f.p, f.n, &g) == WOLF_READ_OK);
    CHECK(g.utf8 && g.nsettings == 4 && g.settings[0] == 16 && g.file_size == 1234);
    CHECK(strcmp(wolf_game_title(&g), "A Test") == 0);
    uint32_t w, h;
    wolf_game_screen(&g, &w, &h);
    CHECK(w == 640 && h == 480);
    wolf_config cfg;
    wolf_game_config(&g, &cfg);
    CHECK(cfg.normal > 0 && cfg.strings > 0 && cfg.seed == 1);
    wolf_game_free(&g);

    buf z = game_file("0000-0000", 0, 0);
    CHECK(wolf_game_load(z.p, z.n, &g) == WOLF_READ_OK);
    wolf_game_screen(&g, &w, &h);
    CHECK(w == 320 && h == 240);
    wolf_game_free(&g);
    free(z.p);

    buf bad = game_file("1111-0000", 640, 480);
    CHECK(load_game(bad.p, bad.n) == WOLF_READ_E_FORMAT);
    free(bad.p);

    size_t ends = f.n - 1; /* without the unread tail */
    for (size_t k = 0; k < ends; k++) CHECK(load_game(f.p, k) != WOLF_READ_OK);
    CHECK(load_game(f.p, ends) == WOLF_READ_OK);
    uint8_t *m = malloc(f.n);
    memcpy(m, f.p, f.n);
    m[1] = 0x50;
    CHECK(load_game(m, f.n) == WOLF_READ_E_PROTECTED);
    memcpy(m, f.p, f.n);
    m[0] = 0x01;
    CHECK(load_game(m, f.n) == WOLF_READ_E_PROTECTED);
    for (int i = 0; i < 20000; i++) {
        memcpy(m, f.p, f.n);
        m[next_rand() % f.n] = (uint8_t)next_rand();
        load_game(m, f.n);
    }
    free(m);
    free(f.p);
}

int main(void) {
    test_lz4();
    test_commons();
    test_db();
    test_game();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    fprintf(stderr, "all data checks passed\n");
    return 0;
}

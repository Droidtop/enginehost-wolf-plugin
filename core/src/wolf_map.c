/* Wolf RPG Editor maps (.mps). Layout: docs/behaviour/formats.md, "Maps (.mps)". */
#include "wolf_map.h"

#include <stdlib.h>
#include <string.h>

#define MAGIC_LEN 20u
#define MARKER_AT 16u
#define TERMINATOR 0x66u
#define EVENT_START 0x6Fu
#define EVENT_END 0x70u
#define PAGE_START 0x79u
#define PAGE_END 0x7Au
#define LAYERED_VERSION 0x67u /* from here: a layer count, and 3.5+ command tails */
#define PACKABLE_VERSION 0x65u

/* The smallest an event or a page can be, for bounding their counts against the input. */
#define EVENT_MIN 26u
#define PAGE_MIN 56u

static const uint8_t EVENT_MAGIC[4] = {0x39, 0x30, 0x00, 0x00};

static int fail_code(const wolf_reader *r, int fallback) { return r->failed ? WOLF_READ_E_FORMAT : fallback; }

static void free_page(wolf_map_page *pg) {
    free(pg->graphic);
    free(pg->route);
    wolf_list_free(&pg->list);
}

static void free_event(wolf_map_event *e) {
    free(e->name);
    if (e->pages)
        for (uint32_t i = 0; i < e->npages; i++) free_page(&e->pages[i]);
    free(e->pages);
}

void wolf_map_free(wolf_map *m) {
    if (m->events)
        for (uint32_t i = 0; i < m->nevents; i++) free_event(&m->events[i]);
    free(m->events);
    free(m->tiles);
    memset(m, 0, sizeof *m);
}

/* A move route: u32 step count, then each step u8 id, u8 argc, argc x s32, 01 00. */
static int read_route(wolf_reader *r, wolf_map_page *pg) {
    pg->route_steps = wolf_read_count(r, WOLF_MAP_ROUTE_MAX, 4);
    size_t start = r->at;
    for (uint32_t s = 0; s < pg->route_steps && !r->failed; s++) {
        wolf_read_u8(r);
        uint32_t argc = wolf_read_u8(r);
        wolf_read_bytes(r, (size_t)argc * 4);
        wolf_expect_u8(r, 0x01);
        wolf_expect_u8(r, 0x00);
    }
    if (r->failed) return WOLF_READ_E_FORMAT;
    size_t len = r->at - start;
    pg->route_len = (uint32_t)len;
    pg->route = malloc(len ? len : 1);
    if (!pg->route) return WOLF_READ_E_NOMEM;
    if (len) memcpy(pg->route, r->p + start, len);
    return WOLF_READ_OK;
}

static int read_page(wolf_reader *r, int extras, wolf_map_page *pg) {
    pg->transfer = -1;
    wolf_read_s32(r); /* meaning Unknown */
    pg->graphic = wolf_read_text(r, NULL);
    pg->direction = (uint8_t)wolf_read_u8(r);
    pg->frame = (uint8_t)wolf_read_u8(r);
    pg->opacity = (uint8_t)wolf_read_u8(r);
    pg->blend = (uint8_t)wolf_read_u8(r);
    pg->trigger = (uint8_t)wolf_read_u8(r);
    for (int i = 0; i < 4; i++) {
        uint32_t b = wolf_read_u8(r);
        pg->cond[i].flags = (uint8_t)(b & 0x0F);
        pg->cond[i].op = (uint8_t)(b >> 4);
    }
    for (int i = 0; i < 4; i++) pg->cond[i].variable = wolf_read_s32(r);
    for (int i = 0; i < 4; i++) pg->cond[i].value = wolf_read_s32(r);
    pg->anim_speed = (uint8_t)wolf_read_u8(r);
    pg->move_speed = (uint8_t)wolf_read_u8(r);
    pg->move_freq = (uint8_t)wolf_read_u8(r);
    pg->move_type = (uint8_t)wolf_read_u8(r);
    pg->options = (uint8_t)wolf_read_u8(r);
    pg->route_options = (uint8_t)wolf_read_u8(r);
    if (r->failed) return WOLF_READ_E_FORMAT;
    int res = read_route(r, pg);
    if (res) return res;
    size_t used = 0;
    int lr = wolf_list_parse_ex(r->p + r->at, r->n - r->at, extras, &used, &pg->list);
    if (lr == WOLF_LIST_E_NOMEM) return WOLF_READ_E_NOMEM;
    if (lr) return WOLF_READ_E_FORMAT;
    r->at += used;
    pg->features = wolf_read_s32(r);
    pg->shadow = (uint8_t)wolf_read_u8(r);
    pg->range_x = (uint8_t)wolf_read_u8(r);
    pg->range_y = (uint8_t)wolf_read_u8(r);
    if (pg->features > 3) pg->transfer = (int32_t)wolf_read_u8(r);
    wolf_expect_u8(r, PAGE_END);
    return fail_code(r, WOLF_READ_OK);
}

static int read_event(wolf_reader *r, int extras, wolf_map_event *e) {
    const uint8_t *magic = wolf_read_bytes(r, sizeof EVENT_MAGIC);
    if (!magic || memcmp(magic, EVENT_MAGIC, sizeof EVENT_MAGIC)) return WOLF_READ_E_FORMAT;
    e->id = wolf_read_s32(r);
    e->name = wolf_read_text(r, NULL);
    e->x = wolf_read_s32(r);
    e->y = wolf_read_s32(r);
    uint32_t declared = wolf_read_count(r, WOLF_MAP_PAGES_MAX, PAGE_MIN);
    for (int i = 0; i < 4; i++) wolf_expect_u8(r, 0);
    if (r->failed) return WOLF_READ_E_FORMAT;
    e->pages = calloc(declared + 1u, sizeof *e->pages);
    if (!e->pages) return WOLF_READ_E_NOMEM;
    for (;;) {
        uint32_t b = wolf_read_u8(r);
        if (r->failed) return WOLF_READ_E_FORMAT;
        if (b == EVENT_END) break;
        if (b != PAGE_START || e->npages == declared) return WOLF_READ_E_FORMAT;
        e->npages++; /* counted first, so a failed page is freed too */
        int res = read_page(r, extras, &e->pages[e->npages - 1]);
        if (res) return res;
    }
    return e->npages == declared ? WOLF_READ_OK : WOLF_READ_E_FORMAT;
}

static int read_body(wolf_reader *r, wolf_map *m) {
    free(wolf_read_text(r, NULL)); /* meaning Unknown */
    m->tileset = wolf_read_s32(r);
    int32_t w = wolf_read_s32(r), h = wolf_read_s32(r);
    uint32_t declared = wolf_read_count(r, WOLF_MAP_EVENTS_MAX, EVENT_MIN);
    int32_t layers = 3;
    if (m->version >= LAYERED_VERSION) {
        wolf_read_s32(r); /* meaning Unknown */
        layers = wolf_read_s32(r);
    }
    if (r->failed) return WOLF_READ_E_FORMAT;
    if (w < 0 || h < 0 || layers < 0) return WOLF_READ_E_FORMAT;
    if ((uint32_t)w > WOLF_MAP_SIDE_MAX || (uint32_t)h > WOLF_MAP_SIDE_MAX || (uint32_t)layers > WOLF_MAP_LAYERS_MAX)
        return WOLF_READ_E_TOO_BIG;
    m->width = (uint32_t)w;
    m->height = (uint32_t)h;
    m->layers = (uint32_t)layers;

    int has_tiles = 1;
    if (m->utf8 && r->n - r->at >= 4) { /* a UTF-8 map may store -1 in place of its tiles */
        size_t save = r->at;
        if (wolf_read_s32(r) == -1) has_tiles = 0;
        else r->at = save;
    }
    if (has_tiles) {
        uint64_t cells = (uint64_t)m->width * m->height * m->layers; /* at most 6.4e11 */
        if (cells * 4 > r->n - r->at) return WOLF_READ_E_FORMAT;
        m->tiles = malloc(cells ? (size_t)cells * sizeof(int32_t) : 1);
        if (!m->tiles) return WOLF_READ_E_NOMEM;
        for (uint64_t i = 0; i < cells; i++) m->tiles[i] = wolf_read_s32(r);
    }

    m->events = calloc(declared + 1u, sizeof *m->events);
    if (!m->events) return WOLF_READ_E_NOMEM;
    int extras = m->version >= LAYERED_VERSION;
    for (;;) {
        uint32_t b = wolf_read_u8(r);
        if (r->failed) return WOLF_READ_E_FORMAT;
        if (b == TERMINATOR) break;
        if (b != EVENT_START || m->nevents == declared) return WOLF_READ_E_FORMAT;
        m->nevents++;
        int res = read_event(r, extras, &m->events[m->nevents - 1]);
        if (res) return res;
    }
    if (m->nevents != declared || r->at != r->n) return WOLF_READ_E_FORMAT;
    return WOLF_READ_OK;
}

int wolf_map_load(const uint8_t *p, size_t n, wolf_map *out) {
    memset(out, 0, sizeof *out);
    if (n >= 2 && p[1] == 0x50) return WOLF_READ_E_PROTECTED;
    if (n < MAGIC_LEN + 5 || p[n - 1] != TERMINATOR) return WOLF_READ_E_FORMAT;
    for (size_t i = 0; i < MAGIC_LEN; i++) {
        static const uint8_t name[6] = {'W', 'O', 'L', 'F', 'M', 0};
        uint8_t want = i >= 10 && i < 16 ? name[i - 10] : 0;
        if (i == MARKER_AT && p[i] == 0x55) {
            out->utf8 = 1;
            continue;
        }
        if (p[i] != want) return WOLF_READ_E_FORMAT;
    }
    wolf_reader r;
    wolf_reader_init(&r, p, n);
    r.at = MAGIC_LEN;
    out->version = wolf_read_u32(&r);
    wolf_read_u8(&r); /* meaning Unknown */
    if (r.failed) return WOLF_READ_E_FORMAT;

    /* Packed when the version allows it and the next two words describe an LZ4 block that ends
     * the file exactly; otherwise plain (formats.md says why). */
    if (out->version >= PACKABLE_VERSION && r.n - r.at >= 8) {
        const uint8_t *q = r.p + r.at + 4;
        uint32_t packed = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
        out->packed = packed == r.n - r.at - 8;
    }
    int res;
    if (out->packed) {
        uint8_t *body = NULL;
        size_t body_len = 0;
        res = wolf_read_unpack(&r, &body, &body_len);
        if (res) return res;
        wolf_reader b;
        wolf_reader_init(&b, body, body_len);
        res = read_body(&b, out);
        free(body);
    } else {
        res = read_body(&r, out);
    }
    if (res) wolf_map_free(out);
    return res;
}

int32_t wolf_map_tile(const wolf_map *m, uint32_t layer, int32_t x, int32_t y) {
    if (!m->tiles || layer >= m->layers || x < 0 || y < 0 || (uint32_t)x >= m->width || (uint32_t)y >= m->height)
        return 0;
    return m->tiles[((size_t)layer * m->height + (size_t)y) * m->width + (size_t)x];
}

const wolf_map_event *wolf_map_find(const wolf_map *m, int32_t id) {
    for (uint32_t i = 0; i < m->nevents; i++)
        if (m->events[i].id == id) return &m->events[i];
    return NULL;
}

void wolf_map_config(const wolf_map *m, wolf_config *cfg) {
    for (uint32_t i = 0; i < m->nevents; i++) {
        int32_t id = m->events[i].id;
        if (id >= 0 && (uint32_t)id < WOLF_MAP_EVENTS_MAX && (uint32_t)id >= cfg->map_events)
            cfg->map_events = (uint32_t)id + 1u;
    }
}

int wolf_map_start_page(wolf_thread *t, const wolf_map *m, const wolf_config *cfg, uint32_t event_index,
                        uint32_t page_index) {
    if (event_index >= m->nevents) return -1;
    const wolf_map_event *e = &m->events[event_index];
    if (page_index >= e->npages || e->id < 0 || (uint32_t)e->id >= cfg->map_events ||
        (uint32_t)e->id >= WOLF_MAP_EVENTS_MAX)
        return -1;
    wolf_thread_start(t, &e->pages[page_index].list, e->id, -1);
    return 0;
}

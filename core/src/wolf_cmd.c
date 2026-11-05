#include "wolf_cmd.h"

#include <stdlib.h>
#include <string.h>

#define COMMANDS_MAX 1000000u
#define STRING_MAX (16u << 20)
#define ROUTE_STEPS_MAX 100000u
#define LIST_BYTES_MAX (256u << 20)

typedef struct {
    const uint8_t *p;
    size_t n, at;
} cursor;

static int take(cursor *c, size_t k, const uint8_t **out) {
    if (k > c->n - c->at) return -1;
    *out = c->p + c->at;
    c->at += k;
    return 0;
}
static int u8(cursor *c, uint32_t *v) {
    const uint8_t *b;
    if (take(c, 1, &b)) return -1;
    *v = b[0];
    return 0;
}
static int u32(cursor *c, uint32_t *v) {
    const uint8_t *b;
    if (take(c, 4, &b)) return -1;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}

typedef struct {
    size_t ints, strs, text, routes, route_bytes;
} totals;

/* One pass over the list. With out == NULL it only checks and counts; otherwise it fills. */
static int walk(cursor *c, uint32_t count, int extras, totals *t, wolf_list *out, int32_t *ints, wolf_str *strs,
                char *text, wolf_route *routes, uint8_t *route_bytes) {
    size_t ni = 0, ns = 0, nt = 0, nr = 0, nb = 0;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t argc1, code, indent, nstr, tail;
        if (u8(c, &argc1) || argc1 == 0 || u32(c, &code)) return WOLF_LIST_E_FORMAT;
        uint32_t nint = argc1 - 1;
        const uint8_t *raw_ints;
        if (take(c, (size_t)nint * 4, &raw_ints)) return WOLF_LIST_E_FORMAT;
        if (u8(c, &indent) || u8(c, &nstr)) return WOLF_LIST_E_FORMAT;
        wolf_cmd *cmd = out ? &out->cmds[k] : NULL;
        if (cmd) {
            cmd->code = (int32_t)code;
            cmd->indent = (uint8_t)indent;
            cmd->nint = (uint8_t)nint;
            cmd->nstr = (uint8_t)nstr;
            cmd->ints = ints + ni;
            cmd->strs = strs + ns;
            cmd->route = NULL;
            for (uint32_t i = 0; i < nint; i++) {
                const uint8_t *b = raw_ints + 4 * i;
                ints[ni + i] = (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
            }
        }
        ni += nint;
        for (uint32_t s = 0; s < nstr; s++) {
            uint32_t len;
            const uint8_t *b;
            if (u32(c, &len) || len > STRING_MAX || take(c, len, &b)) return WOLF_LIST_E_FORMAT;
            /* the stored length counts the NUL; tolerate a missing one */
            uint32_t textlen = len && b[len - 1] == 0 ? len - 1 : len;
            if (cmd) {
                memcpy(text + nt, b, textlen);
                text[nt + textlen] = 0;
                strs[ns + s].text = text + nt;
                strs[ns + s].len = textlen;
            }
            nt += (size_t)textlen + 1;
        }
        ns += nstr;
        if (u8(c, &tail)) return WOLF_LIST_E_FORMAT;
        if (tail == 1) {
            size_t start = c->at;
            const uint8_t *b;
            uint32_t flags, steps;
            if (take(c, 5, &b) || u8(c, &flags) || u32(c, &steps) || steps > ROUTE_STEPS_MAX)
                return WOLF_LIST_E_FORMAT;
            for (uint32_t s = 0; s < steps; s++) {
                uint32_t id, argc;
                if (u8(c, &id) || u8(c, &argc) || take(c, (size_t)argc * 4, &b) || take(c, 2, &b))
                    return WOLF_LIST_E_FORMAT;
            }
            size_t len = c->at - start;
            if (cmd) {
                memcpy(route_bytes + nb, c->p + start, len);
                routes[nr].step_count = steps;
                routes[nr].raw_len = (uint32_t)len;
                routes[nr].raw = route_bytes + nb;
                cmd->route = &routes[nr];
            }
            nr++;
            nb += len;
        } else if (tail != 0) {
            return WOLF_LIST_E_FORMAT;
        }
        if (extras) { /* 3.5+: a byte count and that many bytes, meaning unknown (formats.md) */
            uint32_t en;
            const uint8_t *b;
            if (u8(c, &en) || take(c, en, &b)) return WOLF_LIST_E_FORMAT;
        }
    }
    if (t) {
        t->ints = ni; t->strs = ns; t->text = nt; t->routes = nr; t->route_bytes = nb;
    }
    return WOLF_LIST_OK;
}

int wolf_list_parse_ex(const uint8_t *p, size_t n, int extras, size_t *used, wolf_list *out) {
    memset(out, 0, sizeof *out);
    *used = 0;
    if (n > LIST_BYTES_MAX) n = LIST_BYTES_MAX;
    cursor c = {p, n, 0};
    uint32_t count;
    if (u32(&c, &count)) return WOLF_LIST_E_FORMAT;
    if (count > COMMANDS_MAX) return WOLF_LIST_E_TOO_BIG;
    /* every command takes at least 8 bytes, so a count the input cannot hold is refused early */
    if ((size_t)count > (n - c.at) / 8) return WOLF_LIST_E_FORMAT;
    totals t;
    cursor first = c;
    int r = walk(&first, count, extras, &t, NULL, NULL, NULL, NULL, NULL, NULL);
    if (r) return r;
    size_t bytes = (size_t)count * sizeof(wolf_cmd) + t.ints * sizeof(int32_t) + t.strs * sizeof(wolf_str) +
                   t.routes * sizeof(wolf_route) + t.text + t.route_bytes + 16;
    uint8_t *mem = malloc(bytes);
    if (!mem) return WOLF_LIST_E_NOMEM;
    /* largest alignment first */
    wolf_cmd *cmds = (wolf_cmd *)mem;
    wolf_str *strs = (wolf_str *)(cmds + count);
    wolf_route *routes = (wolf_route *)(strs + t.strs);
    int32_t *ints = (int32_t *)(routes + t.routes);
    char *text = (char *)(ints + t.ints);
    uint8_t *route_bytes = (uint8_t *)text + t.text;
    out->cmds = cmds;
    out->count = count;
    out->storage = mem;
    r = walk(&c, count, extras, NULL, out, ints, strs, text, routes, route_bytes);
    if (r) { wolf_list_free(out); return r; }
    *used = c.at;
    return WOLF_LIST_OK;
}

int wolf_list_parse(const uint8_t *p, size_t n, size_t *used, wolf_list *out) {
    return wolf_list_parse_ex(p, n, 0, used, out);
}

void wolf_list_free(wolf_list *l) {
    free(l->storage);
    memset(l, 0, sizeof *l);
}

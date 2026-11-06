/* Tests for sound (140). Lists and the system database are built here from the layouts in
 * docs/behaviour/formats.md; no game data is used. */
#include "../src/vm.h"
#include "../src/wolf_cmd.h"
#include "../src/wolf_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { uint8_t *p; size_t n, cap; uint32_t count; } buf;
static void put(buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void u8(buf *b, uint32_t v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void u32(buf *b, uint32_t v) { uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; put(b, x, 4); }
static void text(buf *b, const char *s) { uint32_t n = (uint32_t)strlen(s) + 1; u32(b, n); put(b, s, n); }

static void cmd(buf *b, int32_t code, const int32_t *ints, int nint, const char *const *strs) {
    u8(b, (uint32_t)nint + 1);
    u32(b, (uint32_t)code);
    for (int i = 0; i < nint; i++) u32(b, (uint32_t)ints[i]);
    u8(b, 0);
    int ns = 0;
    while (strs && strs[ns]) ns++;
    u8(b, (uint32_t)ns);
    for (int i = 0; i < ns; i++) text(b, strs[i]);
    u8(b, 0);
    b->count++;
}

typedef struct {
    int n;
    wolf_sound last[16];
    char files[16][64];
} rec;
static void on_sound(void *ctx, const wolf_sound *s) {
    rec *r = ctx;
    if (r->n >= 16) return;
    r->last[r->n] = *s;
    snprintf(r->files[r->n], sizeof r->files[r->n], "%s", s->file);
    r->n++;
}
static int log_count;
static void on_log(void *ctx, int level, const char *m) { (void)ctx; (void)level; (void)m; log_count++; }

/* A system database whose types 1-3 (BGM, BGS, SE) each have two rows: a file text field and
 * volume and frequency number fields. Type 0 is empty. */
static int make_sysdb(wolf_db *db) {
    static const char *const files[3][2] = {{"BGM/a.ogg", "BGM/b.ogg"}, {"BGS/rain.ogg", "BGS/wind.ogg"},
                                            {"SE/click.wav", "SE/bell.wav"}};
    buf p = {0}, d = {0};
    u32(&p, 4);
    text(&p, "Empty");
    u32(&p, 0); u32(&p, 0); text(&p, "");
    u32(&p, 0); u32(&p, 0); u32(&p, 0); u32(&p, 0); u32(&p, 0);
    for (int t = 0; t < 3; t++) {
        text(&p, "Sound");
        u32(&p, 3); text(&p, "File"); text(&p, "Volume"); text(&p, "Frequency");
        u32(&p, 2); text(&p, "one"); text(&p, "two");
        text(&p, "");
        u32(&p, 3); u8(&p, 1); u8(&p, 0); u8(&p, 0);
        u32(&p, 3); text(&p, ""); text(&p, ""); text(&p, "");
        u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
        u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
        u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
    }
    static const uint8_t head[] = {0x00, 0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x4D, 0x00};
    put(&d, head, sizeof head);
    u8(&d, 0xC1);
    u32(&d, 4);
    u32(&d, 0xFFFFFFFEu); u32(&d, 0); u32(&d, 0); u32(&d, 0);
    for (int t = 0; t < 3; t++) {
        u32(&d, 0xFFFFFFFEu); u32(&d, 0);
        u32(&d, 3); u32(&d, 2000); u32(&d, 1000); u32(&d, 1001);
        u32(&d, 2);
        for (int r = 0; r < 2; r++) {
            u32(&d, (uint32_t)(80 + r)); u32(&d, (uint32_t)(90 + t));
            text(&d, files[t][r]);
        }
    }
    u8(&d, 0xC1);
    int res = wolf_db_load(p.p, p.n, d.p, d.n, db);
    free(p.p);
    free(d.p);
    return res;
}

#define S(x) (3000000 + (x))
static int32_t word(int proc, int kind, int entry, int source) {
    return (int32_t)((uint32_t)proc | (uint32_t)kind << 4 | ((uint32_t)entry & 0xFFFFu) << 8 | (uint32_t)source << 24);
}

int main(void) {
    wolf_db sys;
    CHECK(make_sysdb(&sys) == WOLF_READ_OK);
    wolf_host host = {NULL, on_log};
    wolf_config cfg = {100, 0, 10, 10, 10, 0, 1, 0};
    wolf_vm *vm = wolf_vm_create(&host, &cfg);
    wolf_vm_set_databases(vm, NULL, NULL, &sys);
    rec r = {0};
    wolf_audio audio = {&r, on_sound};
    wolf_vm_set_audio(vm, &audio);
    wolf_set_str(vm, NULL, S(4), "jump");

    buf b = {0};
    cmd(&b, 140, (int32_t[]){word(0, 0, 1, 0), 30, 1500, 0}, 4, NULL);                     /* BGM entry 1, fade 30, from 1.5 s */
    cmd(&b, 140, (int32_t[]){word(0, 2, 0, 2), 0, 0, 0, 70, 120, 1}, 7, (const char *[]){"SE/\\s[4].wav", NULL}); /* SE by file */
    cmd(&b, 140, (int32_t[]){-256, 0, 0, 0}, 4, NULL);                                      /* BGM stop */
    cmd(&b, 140, (int32_t[]){-240, 30, 0, 0}, 4, NULL);                                     /* BGS stop, fade 30 */
    cmd(&b, 140, (int32_t[]){word(1, 1, 0, 0), 0, 0, 0}, 4, NULL);                         /* preload BGS 0 */
    cmd(&b, 140, (int32_t[]){word(3, 0, 0, 0)}, 1, NULL);                                  /* free unused */
    cmd(&b, 140, (int32_t[]){word(0, 2, 0, 2), 0, 0, 0, 100, 0, 1, 0}, 8, (const char *[]){"SE/x.ogg", NULL}); /* frequency 0 */
    /* skipped: no such entry, unknown kind, unknown process, unknown source */
    cmd(&b, 140, (int32_t[]){word(0, 0, 9, 0), 0, 0, 0}, 4, NULL);
    cmd(&b, 140, (int32_t[]){word(0, 5, 0, 0), 0, 0, 0}, 4, NULL);
    cmd(&b, 140, (int32_t[]){word(2, 0, 0, 0), 0, 0, 0}, 4, NULL);
    cmd(&b, 140, (int32_t[]){word(0, 0, 0, 7), 0, 0, 0}, 4, NULL);
    cmd(&b, 140, NULL, 0, NULL);
    cmd(&b, 0, NULL, 0, NULL);
    buf whole = {0};
    u32(&whole, b.count);
    put(&whole, b.p, b.n);
    wolf_list l;
    size_t used;
    CHECK(wolf_list_parse(whole.p, whole.n, &used, &l) == WOLF_LIST_OK);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    log_count = 0;
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);

    CHECK(r.n == 7);
    CHECK(r.last[0].kind == WOLF_SOUND_BGM && r.last[0].action == WOLF_SOUND_PLAY && strcmp(r.files[0], "BGM/b.ogg") == 0);
    CHECK(r.last[0].fade_frames == 30 && r.last[0].start_ms == 1500 && r.last[0].volume == 81 && r.last[0].frequency == 90);
    CHECK(r.last[1].kind == WOLF_SOUND_SE && strcmp(r.files[1], "SE/jump.wav") == 0 && r.last[1].volume == 70 &&
          r.last[1].frequency == 120 && r.last[1].start_ms == 0);
    CHECK(r.last[2].kind == WOLF_SOUND_BGM && r.last[2].action == WOLF_SOUND_STOP);
    CHECK(r.last[3].kind == WOLF_SOUND_BGS && r.last[3].action == WOLF_SOUND_STOP && r.last[3].fade_frames == 30);
    CHECK(r.last[4].action == WOLF_SOUND_PRELOAD && strcmp(r.files[4], "BGS/rain.ogg") == 0 && r.last[4].frequency == 91);
    CHECK(r.last[5].action == WOLF_SOUND_FREE_UNUSED);
    CHECK(r.last[6].frequency == 100);
    CHECK(log_count == 3); /* each kind of refusal once: no entry, unknown kind (process and source share it), too short */

    /* without audio: logged only */
    wolf_vm_set_audio(vm, NULL);
    wolf_thread_start(t, &l, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE && r.n == 7);

    free(t);
    wolf_list_free(&l);
    free(whole.p);
    free(b.p);
    wolf_vm_destroy(vm);
    wolf_db_free(&sys);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("sound: all checks passed");
    return 0;
}

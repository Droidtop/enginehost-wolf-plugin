/* Tests for the substitution codes (text.c) and command 122 (strings.c). Lists are built here
 * from the layout in docs/behaviour/formats.md; no game data is used. */
#include "../src/vm.h"
#include "../src/vm_internal.h"
#include "../src/wolf_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { uint8_t *p; size_t n, cap; } buf;
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
}

static int log_count;
static void on_log(void *ctx, int level, const char *m) { (void)ctx; (void)level; (void)m; log_count++; }

#define V(x) (2000000 + (x))
#define S(x) (3000000 + (x))
#define SPARE(y, x) (2000000 + 100000 * (y) + (x))

static wolf_vm *make_vm(int sjis) {
    wolf_host host = {NULL, on_log};
    wolf_config cfg = {100, 10, 20, 20, 20, 4, 1, (uint32_t)sjis};
    return wolf_vm_create(&host, &cfg);
}

static void expect(wolf_vm *vm, const wolf_thread *t, const char *src, const char *want) {
    char *got = wolf_expand(vm, t, src);
    CHECK(got && strcmp(got, want) == 0);
    if (got && strcmp(got, want) != 0) fprintf(stderr, "  expand(\"%s\") = \"%s\", want \"%s\"\n", src, got, want);
    free(got);
}

static void test_expand(void) {
    wolf_vm *vm = make_vm(0);
    wolf_set(vm, NULL, V(1), 5);
    wolf_set(vm, NULL, V(5), -42);
    wolf_set(vm, NULL, SPARE(3, 7), 99);
    wolf_set(vm, NULL, 9000000 + 2, 1234);
    wolf_set_str(vm, NULL, S(0), "Alice");
    wolf_set_str(vm, NULL, S(5), "Bob");
    wolf_set_str(vm, NULL, 9900000 + 1, "sys");
    expect(vm, NULL, "HP \\v[1] / \\v[5]", "HP 5 / -42");
    expect(vm, NULL, "\\v3[7]\\sys[2]\\sysS[1]", "991234sys");
    expect(vm, NULL, "Hi \\s[0]!", "Hi Alice!");
    /* an earlier code inside a later one's brackets works; the other way round does not */
    expect(vm, NULL, "\\s[\\v[1]]", "Bob");
    expect(vm, NULL, "\\v[\\s[0]]", "\\v[Alice]");
    /* "\\" is a backslash, not the start of a code; display codes stay */
    expect(vm, NULL, "a\\\\v[1]b", "a\\\\v[1]b");
    expect(vm, NULL, "\\c[2]\\v[1]\\!\\f[20]", "\\c[2]5\\!\\f[20]");
    /* malformed or unknown codes are left as written */
    expect(vm, NULL, "\\v[x] \\v[] \\v[1 \\q[1] \\", "\\v[x] \\v[] \\v[1 \\q[1] \\");
    expect(vm, NULL, "\\v[99999999999]", "\\v[99999999999]");
    expect(vm, NULL, "", "");
    /* a variable outside the game's reads 0 */
    expect(vm, NULL, "\\v[100000]", "0");
    /* self variables need their event: a map event's thread, a common event's thread */
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_list empty = {0};
    wolf_thread_start(t, &empty, 2, -1);
    wolf_set(vm, t, 1100000 + 3, 77);
    expect(vm, t, "\\self[3]", "77");
    static wolf_list lists[2];
    static wolf_common commons[2] = {{&lists[0], "a", -1}, {&lists[1], "b", -1}};
    wolf_vm_set_common_events(vm, commons, 2);
    wolf_thread_start(t, &lists[1], -1, 1);
    wolf_set(vm, t, 1600000 + 12, 8);
    wolf_set_str(vm, t, 1600000 + 6, "six");
    expect(vm, t, "\\cself[12]:\\cself[6]", "8:six");
    expect(vm, t, "\\cself[\\v[1]]", ""); /* cself 5 is a string slot, empty */
    /* \xy is not built: left, logged once */
    log_count = 0;
    expect(vm, NULL, "\\xy[1:2:3]", "\\xy[1:2:3]");
    expect(vm, NULL, "\\xy[1]", "\\xy[1]");
    CHECK(log_count == 1);
    /* a text that grows past the cap is cut */
    char *big = malloc(WOLF_TEXT_MAX / 2 + 1);
    memset(big, 'x', WOLF_TEXT_MAX / 2);
    big[WOLF_TEXT_MAX / 2] = 0;
    wolf_set_str(vm, NULL, S(1), big);
    char *grown = wolf_expand(vm, NULL, "\\s[1]\\s[1]\\s[1]");
    CHECK(grown && strlen(grown) == WOLF_TEXT_MAX);
    free(grown);
    free(big);
    free(t);
    wolf_vm_destroy(vm);
}

static void test_char_len(void) {
    wolf_vm *u = make_vm(0), *j = make_vm(1);
    CHECK(wolf_char_len(u, "abc") == 1);
    CHECK(wolf_char_len(u, "\xE3\x81\x82x") == 3); /* あ in UTF-8 */
    CHECK(wolf_char_len(u, "\xF0\x9F\x98\x80") == 4);
    CHECK(wolf_char_len(u, "\xE3\x81") == 2); /* cut short */
    CHECK(wolf_char_len(j, "\x82\xA0x") == 2);  /* あ in Shift-JIS */
    CHECK(wolf_char_len(j, "\xB1x") == 1);      /* half-width katakana */
    CHECK(wolf_char_len(u, "\\c[12]x") == 6);
    CHECK(wolf_char_len(u, "\\A+x") == 3);
    CHECK(wolf_char_len(u, "\\!x") == 2);
    CHECK(wolf_char_len(u, "\\\\x") == 2);
    CHECK(wolf_char_len(u, "\\r[x,y]") == 3); /* only "\r[" */
    CHECK(wolf_char_len(u, "\\-[3]x") == 5);
    CHECK(wolf_char_len(u, "\\c[12") == 2);   /* no closing bracket */
    CHECK(wolf_char_len(u, "") == 0);
    wolf_vm_destroy(u);
    wolf_vm_destroy(j);
}

/* 122's packed word */
static int32_t word(int src, int target_var, int op, int ext) {
    return (int32_t)((uint32_t)src | (target_var ? 0x10u : 0u) | (uint32_t)op << 8 | (uint32_t)ext << 16);
}

static void run(wolf_vm *vm, const buf *body, uint32_t count) {
    buf b = {0};
    u32(&b, count + 1);
    put(&b, body->p, body->n);
    cmd(&b, 0, NULL, 0, NULL);
    wolf_list l;
    size_t used;
    CHECK(wolf_list_parse(b.p, b.n, &used, &l) == WOLF_LIST_OK);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    free(t);
    wolf_list_free(&l);
    free(b.p);
}

static const char *str(wolf_vm *vm, int32_t ref) { return wolf_get_str(vm, NULL, ref); }

static void test_set_string(void) {
    wolf_vm *vm = make_vm(0);
    wolf_set(vm, NULL, V(1), 3);
    buf b = {0};
    uint32_t n = 0;
#define ADD(ints, k, strs) do { cmd(&b, 122, ints, k, strs); n++; } while (0)
    /* =, with codes replaced; += */
    ADD(((int32_t[]){S(0), word(0, 0, 0, 0)}), 2, ((const char *[]){"n=\\v[1]", NULL}));
    ADD(((int32_t[]){S(0), word(0, 0, 1, 0)}), 2, ((const char *[]){"!", NULL}));
    /* from a string variable, and from one named by a variable */
    ADD(((int32_t[]){S(1), word(1, 0, 0, 0), S(0)}), 3, NULL);
    cmd(&b, 121, (int32_t[]){V(2), S(0), 0, 0x04}, 4, NULL); n++; /* V2 = 3000000 (a raw number) */
    ADD(((int32_t[]){S(2), word(2, 0, 0, 0), V(2)}), 3, NULL);
    /* the target given by a variable */
    cmd(&b, 121, (int32_t[]){V(3), S(3), 0, 0x04}, 4, NULL); n++;
    ADD(((int32_t[]){V(3), word(0, 1, 0, 0)}), 2, ((const char *[]){"via", NULL}));
    /* lines: copy the top line, then cut it from the source variable twice */
    ADD(((int32_t[]){S(4), word(0, 0, 0, 0)}), 2, ((const char *[]){"one\r\ntwo\nthree", NULL}));
    ADD(((int32_t[]){S(5), word(1, 0, 2, 0), S(4)}), 3, NULL);
    ADD(((int32_t[]){S(6), word(1, 0, 3, 0), S(4)}), 3, NULL);
    ADD(((int32_t[]){S(7), word(1, 0, 3, 0), S(4)}), 3, NULL);
    /* one character at a time, a display code counting as one */
    ADD(((int32_t[]){S(8), word(0, 0, 0, 0)}), 2, ((const char *[]){"\xE3\x81\x82\\c[1]z", NULL}));
    ADD(((int32_t[]){S(9), word(1, 0, 4, 0), S(8)}), 3, NULL);
    ADD(((int32_t[]){S(10), word(1, 0, 4, 0), S(8)}), 3, NULL);
    /* remove every, replace every, up to, after, and not found */
    ADD(((int32_t[]){S(11), word(0, 0, 0, 0)}), 2, ((const char *[]){"a-b-c", NULL}));
    ADD(((int32_t[]){S(11), word(0, 0, 8, 0)}), 2, ((const char *[]){"-", NULL}));
    ADD(((int32_t[]){S(12), word(0, 0, 0, 0)}), 2, ((const char *[]){"x\\ny\\nz", NULL}));
    ADD(((int32_t[]){S(12), word(0, 0, 9, 0)}), 2, ((const char *[]){"\\n", "+", NULL}));
    ADD(((int32_t[]){S(13), word(0, 0, 0, 0)}), 2, ((const char *[]){"key:value", NULL}));
    ADD(((int32_t[]){S(14), word(1, 0, 0, 0), S(13)}), 3, NULL);
    ADD(((int32_t[]){S(13), word(0, 0, 10, 0)}), 2, ((const char *[]){":", NULL}));
    ADD(((int32_t[]){S(14), word(0, 0, 11, 0)}), 2, ((const char *[]){":", NULL}));
    ADD(((int32_t[]){S(15), word(0, 0, 0, 0)}), 2, ((const char *[]){"abc", NULL}));
    ADD(((int32_t[]){S(15), word(0, 0, 10, 0)}), 2, ((const char *[]){"#", NULL}));
    /* a literal right side is only read, never cut */
    ADD(((int32_t[]){S(16), word(0, 0, 3, 0)}), 2, ((const char *[]){"top\nrest", NULL}));
    run(vm, &b, n);

    CHECK(strcmp(str(vm, S(0)), "n=3!") == 0);
    CHECK(strcmp(str(vm, S(1)), "n=3!") == 0);
    CHECK(strcmp(str(vm, S(2)), "n=3!") == 0);
    CHECK(strcmp(str(vm, S(3)), "via") == 0);
    CHECK(strcmp(str(vm, S(5)), "one") == 0);
    CHECK(strcmp(str(vm, S(6)), "one") == 0);
    CHECK(strcmp(str(vm, S(7)), "two") == 0);
    CHECK(strcmp(str(vm, S(4)), "three") == 0);
    CHECK(strcmp(str(vm, S(9)), "\xE3\x81\x82") == 0);
    CHECK(strcmp(str(vm, S(10)), "\\c[1]") == 0);
    CHECK(strcmp(str(vm, S(8)), "z") == 0);
    CHECK(strcmp(str(vm, S(11)), "abc") == 0);
    CHECK(strcmp(str(vm, S(12)), "x+y+z") == 0);
    CHECK(strcmp(str(vm, S(13)), "key") == 0);
    CHECK(strcmp(str(vm, S(14)), "value") == 0);
    CHECK(strcmp(str(vm, S(15)), "<<NotFound>>") == 0);
    CHECK(strcmp(str(vm, S(16)), "top") == 0);
    free(b.p);

    /* not built or not known: logged, skipped, and the list runs on */
    buf c = {0};
    uint32_t m = 0;
    int32_t skipped[][3] = {
        {S(0), word(0, 0, 5, 0), 0},  /* file read */
        {S(0), word(0, 0, 6, 0), 0},  /* file write */
        {S(0), word(0, 0, 7, 0), 0},  /* file list */
        {S(0), word(0, 0, 2, 4), 0},  /* the extended forms */
        {S(0), word(3, 0, 0, 0), 30}, /* keyboard input */
        {S(0), word(5, 0, 0, 0), 0},  /* an unknown source */
        {S(0), word(0, 0, 12, 0), 0}, /* an unknown assignment */
    };
    for (unsigned i = 0; i < sizeof skipped / sizeof skipped[0]; i++) {
        cmd(&c, 122, skipped[i], 3, (const char *[]){"zzz", NULL});
        m++;
    }
    cmd(&c, 122, (int32_t[]){S(0)}, 1, NULL); m++;                       /* too short */
    cmd(&c, 122, (int32_t[]){S(0), word(1, 0, 0, 0)}, 2, NULL); m++;     /* no source variable */
    cmd(&c, 122, (int32_t[]){V(9), word(0, 0, 0, 0)}, 2, (const char *[]){"x", NULL}); m++; /* not a string variable */
    run(vm, &c, m);
    CHECK(strcmp(str(vm, S(0)), "n=3!") == 0);
    CHECK(wolf_get(vm, NULL, V(9)) == 0);
    free(c.p);
    wolf_vm_destroy(vm);
}

int main(void) {
    test_expand();
    test_char_len();
    test_set_string();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("strings: all checks passed");
    return 0;
}

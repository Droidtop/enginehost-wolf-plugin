/* Shared between the interpreter's source files; not part of the core's interface. */
#ifndef ENGINEHOST_WOLF_VM_INTERNAL_H
#define ENGINEHOST_WOLF_VM_INTERNAL_H

#include "vm.h"

#define CODE_MAX 1000
#define CSELF_COUNT 100
#define CSELF_STR_FIRST 5
#define CSELF_STR_LAST 9
#define MAP_SELF_COUNT 10

/* Things logged once each, so a game that hits them every frame does not flood the log. */
enum {
    ONCE_DB, ONCE_CHARA, ONCE_RANGE_1500000, ONCE_MAP_EVENT_CALL, ONCE_NAME_ESCAPE,
    ONCE_STRING_OP, ONCE_BAD_ARGS, ONCE_LABEL, ONCE_DB_MISSING, ONCE_DB_KIND, ONCE_DB_OP,
    ONCE_DB_NAME, ONCE_TEXT_XY, ONCE_TEXT_LONG, ONCE_STR_SOURCE, ONCE_STR_FILE, ONCE_STR_EXT,
    ONCE_UI_HEADLESS, ONCE_UI_EXTRA, ONCE_PIC_TRANSFORM, ONCE_PIC_LIMIT, ONCE_PIC_OP, ONCE_COUNT
};

struct wolf_vm {
    wolf_host host;
    wolf_config cfg;
    int flag_stop_choices;
    int flag_stop_message;
    uint8_t unhandled[CODE_MAX + 1];
    int unhandled_other;
    uint8_t once[ONCE_COUNT];

    int32_t *normal;
    int32_t *spare; /* 9 blocks of cfg.spare */
    int32_t *system;
    char **strings;
    char **system_strings;
    int32_t *map_self; /* cfg.map_events x MAP_SELF_COUNT */

    const wolf_common *common;
    uint32_t ncommon;
    int32_t *cself;     /* ncommon x CSELF_COUNT */
    char **cself_str;   /* ncommon x 5 (self variables 5-9) */

    uint32_t rng;

    struct wolf_db *db[3]; /* WOLF_DB_USER, WOLF_DB_CHANGEABLE, WOLF_DB_SYSTEM; not owned */

    /* the window (ui.c): one at a time */
    wolf_ui ui;
    int has_ui;
    int ui_kind;                 /* UI_NONE, UI_MESSAGE, UI_CHOICES */
    const wolf_thread *ui_owner; /* the thread waiting on it, and the command it waits in */
    const wolf_cmd *ui_cmd;
    int ui_answered;
    int32_t ui_result;           /* the choice, -1 cancelled, -2 force-stopped */

    struct wolf_pictures *pics;  /* picture.c; NULL until the first 150 */
};

enum { UI_NONE, UI_MESSAGE, UI_CHOICES };

struct wolf_pictures; /* picture.c */

/* The three databases, in the order of their call values (06valueget): 10 user, 11 changeable,
 * 13 system. */
enum { WOLF_DB_USER = 0, WOLF_DB_CHANGEABLE = 1, WOLF_DB_SYSTEM = 2 };

/* 121's assignments, shared with 250 (calc.c): the result of "left how= right", or 0 when how is
 * not an assignment this build knows. */
int wolf_assign(int how, int64_t left, int64_t right, int64_t *out);

void wolf_once(wolf_vm *vm, int which, const char *fmt, ...);
uint32_t wolf_random(wolf_vm *vm, uint32_t bound); /* 0..bound inclusive */

/* An operand: a literal from -999,999 to 999,999 (and, by our reading, anything below), else the
 * variable that call value names. */
int32_t wolf_value(wolf_vm *vm, const wolf_thread *t, int32_t v);
/* Whether a call value is one the "fast" common-event arithmetic covers: a literal, a self
 * variable or a normal variable (manual, 計算式の説明). */
int wolf_is_plain(int32_t v);

/* A number comparison, numbered as command 111 and map page conditions number them
 * (commands.md); an unknown one is false and logged once. */
int wolf_compare(wolf_vm *vm, int32_t left, int32_t right, uint32_t op);

/* Handlers, by file. */
int wolf_h_set_variable(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);   /* 121 */
int wolf_h_if_number(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);      /* 111 */
int wolf_h_if_string(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);      /* 112 */
int wolf_h_case(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 401, 402, 421 */
int wolf_h_else(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 420 */
int wolf_h_branch_end(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);     /* 499 */
int wolf_h_loop(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 170 */
int wolf_h_loop_times(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);     /* 179 */
int wolf_h_loop_end(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);       /* 498 */
int wolf_h_loop_break(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);     /* 171 */
int wolf_h_loop_continue(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);  /* 176 */
int wolf_h_stop_event(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);     /* 172 */
int wolf_h_wait(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 180 */
int wolf_h_label(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);          /* 212 */
int wolf_h_jump(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 213 */
int wolf_h_call(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 210 */
int wolf_h_call_by_name(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);   /* 300 */
int wolf_h_database(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);       /* 250 */
int wolf_h_set_string(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);     /* 122 */
int wolf_h_message(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);        /* 101 */
int wolf_h_choices(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);        /* 102 */
int wolf_h_stop_choices(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);   /* 104 */
int wolf_h_stop_message(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);   /* 105 */
int wolf_h_picture(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);        /* 150 */
void wolf_pictures_free(wolf_vm *vm);

/* After a construct's opener c (flow.c): on to just past the nth (0-based) marker with this code
 * at c's indent, or past the branch end when there is none. 1 when the marker was found. */
int wolf_flow_to_marker(wolf_thread *t, const wolf_cmd *c, int32_t code, uint32_t nth);

/* Text (text.c). The longest text the core builds; longer results are cut there, logged once. */
#define WOLF_TEXT_MAX (4u << 20)
/* A copy of src with the substitution codes (\v[], \s[], \cself[], \cdb[]...) replaced, in the
 * manual's order (06specialword.html); other codes are left for whoever shows the text. NULL
 * when out of memory. The caller frees it. */
char *wolf_expand(wolf_vm *vm, const wolf_thread *t, const char *src);
/* The length in bytes of the character at s: one game character in the game's encoding, or a
 * whole display code such as "\c[1]" (122's one-character cuts, 04ev_string.html). */
size_t wolf_char_len(const wolf_vm *vm, const char *s);

/* Called by the loop when a list ends, to hand back a common event's result. */
void wolf_return(wolf_vm *vm, wolf_thread *t);

#endif

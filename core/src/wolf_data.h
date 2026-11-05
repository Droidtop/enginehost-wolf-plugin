/*
 * The project files a game starts from: CommonEvent.dat, the three databases and Game.dat.
 * Layouts: docs/behaviour/formats.md. Every loader checks every count against its input and
 * a cap, owns copies of everything it keeps, and refuses Pro-protected or scrambled files.
 */
#ifndef ENGINEHOST_WOLF_DATA_H
#define ENGINEHOST_WOLF_DATA_H

#include <stddef.h>
#include <stdint.h>

#include "vm.h"
#include "wolf_cmd.h"
#include "wolf_read.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CommonEvent.dat --------------------------------------------------------------------- */

typedef struct {
    uint32_t number;        /* as stored */
    uint8_t run;            /* 0 called only, 1 automatic, 2 parallel, 3 parallel always */
    uint8_t condition_op;
    int32_t condition_var, condition_value;
    uint8_t number_args, text_args;
    char *name;
    wolf_list list;
    uint32_t ndefaults;
    int32_t *defaults;      /* the arguments' defaults */
    int32_t return_self;    /* the self variable (0-99) handed back, or -1 */
} wolf_common_event;

typedef struct {
    int utf8;
    uint32_t version;
    int packed;             /* 3.5+ */
    uint32_t count;
    wolf_common_event *events;
    wolf_common *table;     /* for wolf_vm_set_common_events */
} wolf_commons;

int wolf_commons_load(const uint8_t *p, size_t n, wolf_commons *out);
void wolf_commons_free(wolf_commons *c);

/* ---- Databases --------------------------------------------------------------------------- */

typedef struct {
    char *name;
    uint8_t kind;    /* 0 number, 1 file name, 2 database reference, 3 choices */
    int32_t def;     /* default */
    uint8_t is_text; /* from the .dat: block 2 */
    uint32_t slot;   /* the value's place among the row's numbers or texts */
} wolf_db_field;

typedef struct {
    char *name;
    int32_t *numbers;
    char **texts;
} wolf_db_row;

typedef struct {
    char *name;
    int32_t id_method;
    uint32_t nfields, nrows, nnumbers, ntexts;
    wolf_db_field *fields;
    wolf_db_row *rows;
} wolf_db_type;

typedef struct {
    int utf8;
    uint32_t version;
    uint32_t ntypes;
    wolf_db_type *types;
} wolf_db;

/* project: the .project file; dat: the .dat. */
int wolf_db_load(const uint8_t *project, size_t project_len, const uint8_t *dat, size_t dat_len, wolf_db *out);
void wolf_db_free(wolf_db *db);
/* A value, or 0 / NULL when the type, row or field does not exist or holds the other kind. */
int wolf_db_number(const wolf_db *db, uint32_t type, uint32_t row, uint32_t field, int32_t *out);
const char *wolf_db_text(const wolf_db *db, uint32_t type, uint32_t row, uint32_t field);

/* ---- Game.dat ---------------------------------------------------------------------------- */

typedef struct {
    int utf8;
    uint32_t version;
    uint32_t nsettings;
    uint8_t *settings;
    uint32_t ntexts;
    char **texts;
    int32_t file_size;
    uint32_t nwords;
    uint16_t *words;
} wolf_game;

int wolf_game_load(const uint8_t *p, size_t n, wolf_game *out);
void wolf_game_free(wolf_game *g);
const char *wolf_game_title(const wolf_game *g);
void wolf_game_screen(const wolf_game *g, uint32_t *width, uint32_t *height);
/* A wolf_config for the interpreter. The variable counts are not in Game.dat and where the
 * runtime takes them from is Unknown (formats.md), so they are fixed defaults for now. */
void wolf_game_config(const wolf_game *g, wolf_config *cfg);

#ifdef __cplusplus
}
#endif

#endif

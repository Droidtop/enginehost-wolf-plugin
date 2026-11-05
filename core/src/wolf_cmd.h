/*
 * Event command lists, as stored in Wolf RPG Editor's CommonEvent.dat and map (.mps) files.
 * Layout: docs/behaviour/formats.md, "Event commands". Every count and length is checked.
 */
#ifndef ENGINEHOST_WOLF_CMD_H
#define ENGINEHOST_WOLF_CMD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *text; /* NUL-terminated, in the file's encoding */
    uint32_t len;     /* bytes, without the NUL */
} wolf_str;

typedef struct {
    uint32_t step_count;
    uint32_t raw_len;       /* the route as stored, after the tail byte */
    const uint8_t *raw;
} wolf_route;

typedef struct {
    int32_t code;
    uint8_t indent;
    uint8_t nint;
    uint8_t nstr;
    const int32_t *ints;
    const wolf_str *strs;
    const wolf_route *route; /* NULL unless a move route follows */
} wolf_cmd;

typedef struct {
    wolf_cmd *cmds;
    uint32_t count;
    void *storage; /* owned; freed by wolf_list_free */
} wolf_list;

enum {
    WOLF_LIST_OK = 0,
    WOLF_LIST_E_FORMAT = -1,
    WOLF_LIST_E_TOO_BIG = -2,
    WOLF_LIST_E_NOMEM = -3,
};

/* Parses one command list (u32 count, then the commands) at the start of p[0..n).
 * *used is set to the bytes it took. The list owns copies of everything it points to. */
int wolf_list_parse(const uint8_t *p, size_t n, size_t *used, wolf_list *out);
/* The same, for 3.5+ files when extras is non-zero: every command then ends with a byte count
 * and that many bytes (formats.md, "CommonEvent.dat"), checked and skipped. */
int wolf_list_parse_ex(const uint8_t *p, size_t n, int extras, size_t *used, wolf_list *out);
void wolf_list_free(wolf_list *l);

#ifdef __cplusplus
}
#endif

#endif

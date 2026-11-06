/*
 * 250, Database (DB操作): read a database field into a variable, or write one. Behaviour from the
 * editor's manual, help/04ev_db.html; the argument layout from the owner's games' data, checked
 * against take-cheeze/rpg-maker-clone's mruby-wolf (read only). docs/behaviour/commands.md has the
 * table and the Unknowns.
 */
#include "vm_internal.h"
#include "wolf_data.h"

#include <stdlib.h>
#include <string.h>

#define DB_DATA_PER_TYPE 10000 /* data numbers run 0-9999; past that, the next type (04ev_db) */
#define DB_FIELDS_PER_DATA 100 /* field numbers run 0-99; past that, the next data */
#define DB_NAMES_MAX 3

enum { OP_WRITE = 0, OP_READ = 1 };
/* byte 1's low four bits: which database */
static const int WHICH[3] = {WOLF_DB_CHANGEABLE, WOLF_DB_SYSTEM, WOLF_DB_USER};

static int32_t hold(int64_t v) { /* as 121 outside the common-event wrap: ±2,000,000,000 */
    return (int32_t)(v > 2000000000 ? 2000000000 : v < -2000000000 ? -2000000000 : v);
}

/* A name box: when its bit is set and the command carries that name, the name decides the
 * number. A name the database does not have leaves the stored number. */
static int32_t by_name(wolf_vm *vm, const wolf_cmd *c, uint32_t bits, int box, int32_t number,
                       const struct wolf_db *db, int32_t type) {
    if (!(bits & (1u << box)) || c->nstr <= (uint8_t)(box + 1)) return number;
    const char *name = c->strs[box + 1].text;
    if (!name || !*name) return number;
    int32_t found = box == 0 ? wolf_db_type_by_name(db, name)
                  : type < 0 ? -1
                  : box == 1 ? wolf_db_row_by_name(db, (uint32_t)type, name)
                             : wolf_db_field_by_name(db, (uint32_t)type, name);
    if (found < 0) {
        wolf_once(vm, ONCE_DB_NAME, "a command 250 names \"%s\", which the database does not have; "
                  "the stored number %d is used", name, (int)number);
        return number;
    }
    return found;
}

int wolf_h_database(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    if (c->nint < 4) {
        wolf_once(vm, ONCE_BAD_ARGS, "a command 250 has %d arguments; skipped", c->nint);
        return WOLF_NEXT;
    }
    uint32_t packed = (uint32_t)c->ints[3];
    uint32_t b0 = packed & 0xFF, which = (packed >> 8) & 0xF, op = (packed >> 12) & 0xF;
    uint32_t names = (packed >> 16) & 0x7;
    int how = (int)(b0 >> 4);
    int literal = (b0 & 0x02) != 0; /* the value written is the command's first string */

    if (which > 2) {
        wolf_once(vm, ONCE_DB_OP, "a command 250 picks database %u, which is not known; skipped", which);
        return WOLF_NEXT;
    }
    if (op != OP_WRITE && op != OP_READ) {
        wolf_once(vm, ONCE_DB_OP, "a command 250 asks for operation %u, which is not built; skipped", op);
        return WOLF_NEXT;
    }
    if (b0 & 0x01) {
        wolf_once(vm, ONCE_DB_OP, "a command 250 sets option bit 0, whose meaning is Unknown; ignored");
    }
    if (!literal && c->nint < 5) {
        wolf_once(vm, ONCE_BAD_ARGS, "a command 250 has no value or target; skipped");
        return WOLF_NEXT;
    }
    struct wolf_db *db = vm->db[WHICH[which]];
    if (!db) {
        wolf_once(vm, ONCE_DB_MISSING, "a command 250 uses a database the game did not load; skipped");
        return WOLF_NEXT;
    }

    int64_t type = wolf_value(vm, t, c->ints[0]);
    type = by_name(vm, c, names, 0, (int32_t)type, db, -1);
    int64_t data = by_name(vm, c, names, 1, wolf_value(vm, t, c->ints[1]), db, (int32_t)type);
    int64_t field = by_name(vm, c, names, 2, wolf_value(vm, t, c->ints[2]), db, (int32_t)type);
    if (type < 0 || data < 0 || field < 0) {
        wolf_once(vm, ONCE_DB_MISSING, "a command 250 names a negative type, data or field; skipped");
        return WOLF_NEXT;
    }
    /* Past the last field, the next data; past the last data, the next type (04ev_db). */
    data += field / DB_FIELDS_PER_DATA;
    field %= DB_FIELDS_PER_DATA;
    type += data / DB_DATA_PER_TYPE;
    data %= DB_DATA_PER_TYPE;

    uint32_t ty = (uint32_t)type, row = (uint32_t)data, fi = (uint32_t)field;
    int32_t number;
    const char *text = wolf_db_text(db, ty, row, fi);
    int is_number = !text && wolf_db_number(db, ty, row, fi, &number);
    if (!text && !is_number) {
        wolf_once(vm, ONCE_DB_MISSING, "a command 250 names type %u data %u field %u, which the database "
                  "does not have; skipped", ty, row, fi);
        return WOLF_NEXT;
    }

    if (text) {
        /* Text fields: = and += (joined) are the ones the manual and the games use. */
        if (how > 1) {
            wolf_once(vm, ONCE_DB_KIND, "a command 250 uses assignment %d on a text field; taken as =", how);
            how = 0;
        }
        const char *from;
        const char *current;
        if (op == OP_READ) {
            if (literal || c->nint < 5) return WOLF_NEXT;
            from = text;
            current = wolf_get_str(vm, t, c->ints[4]);
        } else {
            from = literal ? (c->nstr ? c->strs[0].text : "") : wolf_get_str(vm, t, c->ints[4]);
            current = text;
        }
        if (!from) from = "";
        char *joined = NULL;
        if (how == 1) {
            size_t a = strlen(current), b = strlen(from);
            joined = malloc(a + b + 1);
            if (!joined) return WOLF_NEXT;
            memcpy(joined, current, a);
            memcpy(joined + a, from, b + 1);
            from = joined;
        }
        if (op == OP_READ) wolf_set_str(vm, t, c->ints[4], from);
        else wolf_db_set_text(db, ty, row, fi, from);
        free(joined);
        return WOLF_NEXT;
    }

    if (literal) {
        wolf_once(vm, ONCE_DB_KIND, "a command 250 writes a text into a number field; skipped");
        return WOLF_NEXT;
    }
    int64_t result;
    if (op == OP_READ) {
        if (!wolf_assign(how, wolf_get(vm, t, c->ints[4]), number, &result)) goto unknown;
        wolf_set(vm, t, c->ints[4], hold(result));
    } else {
        if (!wolf_assign(how, number, wolf_value(vm, t, c->ints[4]), &result)) goto unknown;
        wolf_db_set_number(db, ty, row, fi, hold(result));
    }
    return WOLF_NEXT;
unknown:
    wolf_once(vm, ONCE_DB_OP, "a command 250 uses assignment %d, which is not known; skipped", how);
    return WOLF_NEXT;
}

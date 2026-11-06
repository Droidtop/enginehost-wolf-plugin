/*
 * 140, Sound (サウンド): play, stop, preload and free BGM, BGS and SE. What each part does is from
 * the editor's manual, help/04ev_sound.html; the packed first number from mruby-wolf (read only),
 * checked against all 5,306 real commands in the owner's games. docs/behaviour/commands.md,
 * "Sound", has the layout and the Unknowns.
 */
#include "vm_internal.h"
#include "wolf_data.h"

#include <stdlib.h>
#include <string.h>

enum { PROC_PLAY = 0, PROC_PRELOAD = 1, PROC_FREE = 3 };
enum { SRC_DB = 0, SRC_DB_VARIABLE = 1, SRC_FILE = 2 };
#define DB_STOP (-1)           /* the system database's "(停止)" entry */
#define SYSTEM_DB_SOUND_TYPE 1 /* system database type 1 BGM, 2 BGS, 3 SE (manual) */

void wolf_vm_set_audio(wolf_vm *vm, const wolf_audio *audio) {
    vm->has_audio = audio != NULL;
    if (audio) vm->audio = *audio;
    else memset(&vm->audio, 0, sizeof vm->audio);
}

static void send(wolf_vm *vm, const wolf_sound *s) {
    if (vm->has_audio && vm->audio.sound) {
        vm->audio.sound(vm->audio.ctx, s);
        return;
    }
    static const char *const kinds[] = {"BGM", "BGS", "SE"};
    static const char *const actions[] = {"play", "stop", "preload", "free unused"};
    wolf_vm_log(vm, WOLF_LOG_DEBUG, "sound: %s %s \"%s\" (no audio set)", actions[s->action], kinds[s->kind], s->file);
}

/* A system database sound entry: its file is the row's first text field; volume and frequency
 * are the first two number fields [M]. 0 when the entry does not exist. */
static int db_entry(wolf_vm *vm, int kind, int32_t entry, wolf_sound *s, const char **file) {
    struct wolf_db *db = vm->db[WOLF_DB_SYSTEM];
    uint32_t type = SYSTEM_DB_SOUND_TYPE + (uint32_t)kind;
    if (!db || entry < 0 || type >= db->ntypes || (uint32_t)entry >= db->types[type].nrows) return 0;
    const wolf_db_type *ty = &db->types[type];
    int numbers = 0;
    *file = NULL;
    for (uint32_t f = 0; f < ty->nfields; f++) {
        int32_t n;
        const char *txt = wolf_db_text(db, type, (uint32_t)entry, f);
        if (txt && !*file) *file = txt;
        else if (!txt && wolf_db_number(db, type, (uint32_t)entry, f, &n)) {
            if (numbers == 0) s->volume = n;
            else if (numbers == 1) s->frequency = n;
            numbers++;
        }
    }
    return *file != NULL;
}

int wolf_h_sound(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    if (c->nint < 1) {
        wolf_once(vm, ONCE_BAD_ARGS, "a command 140 has no arguments; skipped");
        return WOLF_NEXT;
    }
    uint32_t w = (uint32_t)c->ints[0];
    int proc = (int)(w & 0xF), kind = (int)((w >> 4) & 0xF), source = (int)((w >> 24) & 0xFF);
    int32_t entry = (int16_t)(uint16_t)((w >> 8) & 0xFFFF);
    /* the stop entry (-1) is stored sign-extended over the source byte: -256, -240, -224 [D] */
    if (source == 0xFF && entry == DB_STOP) source = SRC_DB;
    if (kind > WOLF_SOUND_SE) {
        wolf_once(vm, ONCE_SOUND, "a command 140 names sound kind %d, which is not known; skipped", kind);
        return WOLF_NEXT;
    }
    wolf_sound s;
    memset(&s, 0, sizeof s);
    s.kind = kind;
    s.file = "";
    s.volume = 100;
    s.frequency = 100;
    if (proc == PROC_FREE) {
        s.action = WOLF_SOUND_FREE_UNUSED;
        send(vm, &s);
        return WOLF_NEXT;
    }
    if (proc != PROC_PLAY && proc != PROC_PRELOAD) {
        wolf_once(vm, ONCE_SOUND, "a command 140 asks for process %d, which is not known; skipped", proc);
        return WOLF_NEXT;
    }
    s.action = proc == PROC_PRELOAD ? WOLF_SOUND_PRELOAD : WOLF_SOUND_PLAY;
    s.fade_frames = c->nint > 1 ? wolf_value(vm, t, c->ints[1]) : 0;
    if (s.fade_frames < 0) s.fade_frames = 0;
    s.start_ms = kind != WOLF_SOUND_SE && c->nint > 2 ? wolf_value(vm, t, c->ints[2]) : 0;
    if (s.start_ms < 0) s.start_ms = 0;

    char *expanded = NULL;
    if (source == SRC_FILE) {
        expanded = wolf_expand(vm, t, c->nstr ? c->strs[0].text : "");
        if (!expanded) return WOLF_NEXT;
        s.file = expanded;
        if (c->nint > 4) s.volume = wolf_value(vm, t, c->ints[4]);
        if (c->nint > 5) s.frequency = wolf_value(vm, t, c->ints[5]);
        if (!*s.file) s.action = WOLF_SOUND_STOP; /* an empty name plays nothing [M] */
    } else if (source == SRC_DB || source == SRC_DB_VARIABLE) {
        if (source == SRC_DB_VARIABLE) entry = c->nint > 3 ? wolf_value(vm, t, c->ints[3]) : DB_STOP;
        if (entry == DB_STOP) {
            s.action = WOLF_SOUND_STOP;
        } else {
            const char *file = NULL;
            if (!db_entry(vm, kind, entry, &s, &file)) {
                wolf_once(vm, ONCE_SOUND_DB, "a command 140 plays system database sound %d, which the game does "
                          "not have; skipped", (int)entry);
                return WOLF_NEXT;
            }
            s.file = file;
        }
    } else {
        wolf_once(vm, ONCE_SOUND, "a command 140 takes its sound from source %d, which is not known; skipped", source);
        return WOLF_NEXT;
    }
    if (s.volume < 0) s.volume = 0;
    if (s.frequency <= 0) s.frequency = 100; /* 0 in real SE commands: taken as normal [M] */
    send(vm, &s);
    free(expanded);
    return WOLF_NEXT;
}

/*
 * WolfPlugin's natives: the Wolf core in Enginehost's isolated runtime.
 *
 * First milestone (Droidtop/tracker#858): nativeStart opens the game folder (wolf_open.c), logs
 * what it loaded, and nativeStep hands the host a black frame of the game's screen size. Nothing
 * runs the interpreter yet. A game the core refuses or cannot read comes back as nativeError's
 * plain sentence, which WolfPlugin hands to host.fail.
 */
#include <android/log.h>
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "wolf_frame.h"
#include "wolf_open.h"

#define TAG "wolf"
/* A screen larger than this is a damaged Game.dat, not a game. */
#define SCREEN_SIDE_MAX 4096u

typedef struct {
    wolf_open_game game;
    wolf_vm *vm; /* holds pointers into game: freed first */
    wolf_frame *frame; /* runs the game; holds pointers into vm and game: freed before both */
    uint32_t *pixels;
} engine;

static char gError[512];

static void LogLine(void *ctx, const char *line)
{
    (void)ctx;
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", line);
}

static void VmLog(void *ctx, int level, const char *message)
{
    (void)ctx;
    int prio = level >= WOLF_LOG_ERROR ? ANDROID_LOG_ERROR : level >= WOLF_LOG_WARN ? ANDROID_LOG_WARN
             : level >= WOLF_LOG_INFO ? ANDROID_LOG_INFO : ANDROID_LOG_DEBUG;
    __android_log_print(prio, TAG, "%s", message);
}

/* The interpreter over the loaded game: its common events, and the three databases, which
 * command 250 and the database call values read and write in place (the core's
 * wolf_vm_set_databases). 0 when out of memory. */
static wolf_vm *StartVm(engine *e)
{
    wolf_config cfg;
    wolf_game_config(&e->game.game, &cfg);
    wolf_host host = {NULL, VmLog};
    wolf_vm *vm = wolf_vm_create(&host, &cfg);
    if (!vm) return NULL;
    wolf_vm_set_common_events(vm, e->game.commons.table, e->game.commons.count);
    wolf_vm_set_databases(vm, &e->game.db[WOLF_OPEN_DB_USER], &e->game.db[WOLF_OPEN_DB_CHANGEABLE],
                          &e->game.db[WOLF_OPEN_DB_SYSTEM]);
    return vm;
}

static jlong JNICALL NativeStart(JNIEnv *env, jclass cls, jstring gamePath)
{
    (void)cls;
    gError[0] = 0;
    const char *folder = (*env)->GetStringUTFChars(env, gamePath, NULL);
    if (!folder) return 0;
    engine *e = calloc(1, sizeof *e);
    if (!e) {
        snprintf(gError, sizeof gError, "Out of memory starting the Wolf core.");
    } else if (wolf_open(folder, &e->game, gError, sizeof gError, LogLine, NULL) != 0) {
        free(e);
        e = NULL;
    } else if (e->game.width > SCREEN_SIDE_MAX || e->game.height > SCREEN_SIDE_MAX) {
        snprintf(gError, sizeof gError, "Game.dat asks for a %ux%u screen, which this core does not accept.",
                 e->game.width, e->game.height);
        wolf_open_free(&e->game);
        free(e);
        e = NULL;
    } else if (!(e->vm = StartVm(e)) || !(e->frame = wolf_frame_start(folder, &e->game, e->vm)) ||
               !(e->pixels = malloc((size_t)e->game.width * e->game.height * sizeof *e->pixels))) {
        snprintf(gError, sizeof gError, "Out of memory starting the Wolf core.");
        wolf_frame_stop(e->frame);
        wolf_vm_destroy(e->vm);
        wolf_open_free(&e->game);
        free(e);
        e = NULL;
    } else {
        __android_log_print(ANDROID_LOG_INFO, TAG, "loaded from %s; running %u common events, pictures drawn in "
                            "software; messages and choices are headless (logged) for now", e->game.source,
                            e->game.commons.count);
    }
    (*env)->ReleaseStringUTFChars(env, gamePath, folder);
    return (jlong)(intptr_t)e;
}

static jstring JNICALL NativeError(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, gError[0] ? gError : "The Wolf core could not start.");
}

static jint JNICALL NativeWidth(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    return (jint)((engine *)(intptr_t)handle)->game.width;
}

static jint JNICALL NativeHeight(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    return (jint)((engine *)(intptr_t)handle)->game.height;
}

/* One frame: the game runs a step and the whole screen is drawn again. */
static jint JNICALL NativeStep(JNIEnv *env, jclass cls, jlong handle, jintArray pixels)
{
    (void)cls;
    engine *e = (engine *)(intptr_t)handle;
    jsize want = (jsize)(e->game.width * e->game.height);
    if ((*env)->GetArrayLength(env, pixels) < want) return -1;
    wolf_frame_step(e->frame, e->pixels); /* drawn outside the critical section */
    (*env)->SetIntArrayRegion(env, pixels, 0, want, (const jint *)e->pixels);
    return (jint)e->game.height; /* rows 0 .. height-1 changed */
}

static void JNICALL NativeStop(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    engine *e = (engine *)(intptr_t)handle;
    wolf_frame_stop(e->frame);
    free(e->pixels);
    wolf_vm_destroy(e->vm);
    wolf_open_free(&e->game);
    free(e);
}

/*
 * The isolated runtime loads this library from a descriptor, where nothing binds natives by
 * name, and calls this to bind WolfPlugin's itself (Enginehost's isolated_native_bridge.c).
 */
__attribute__((visibility("default")))
void enginehost_register_natives(JNIEnv *env, jclass cls)
{
    static const JNINativeMethod methods[] = {
        { "nativeStart", "(Ljava/lang/String;)J", (void *)NativeStart },
        { "nativeError", "()Ljava/lang/String;", (void *)NativeError },
        { "nativeWidth", "(J)I", (void *)NativeWidth },
        { "nativeHeight", "(J)I", (void *)NativeHeight },
        { "nativeStep", "(J[I)I", (void *)NativeStep },
        { "nativeStop", "(J)V", (void *)NativeStop },
    };
    (*env)->RegisterNatives(env, cls, methods, (jint)(sizeof methods / sizeof methods[0]));
}

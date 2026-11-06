/*
 * WolfPlugin's natives. Two ways to run (Droidtop/tracker#858, comment 6691411927):
 * - with a display (Enginehost's runtime process, allocate-then-clamp): GLES on the GL thread of
 *   WolfPlugin's GLSurfaceView (nativeGl*), sound through AAudio;
 * - isolated, step-driven, the fallback: software drawing into nativeStep's pixels, sound into
 *   the host's audio ring.
 * A game the core refuses or cannot read comes back as nativeError's plain sentence, which
 * WolfPlugin hands to host.fail.
 */
#include <android/log.h>
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enginehost_audio_ring.h"
#include "vm.h"
#include "wolf_aaudio.h"
#include "wolf_frame.h"
#include "wolf_gl.h"
#include "wolf_open.h"

#define TAG "wolf"
/* A screen larger than this is a damaged Game.dat, not a game. */
#define SCREEN_SIDE_MAX 4096u

typedef struct {
    wolf_open_game game;
    wolf_vm *vm; /* holds pointers into game: freed first */
    wolf_frame *frame; /* runs the game; holds pointers into vm and game: freed before both */
    uint32_t *pixels;
    struct enginehost_audio_ring ring; /* base NULL: no sound */
    uint32_t rate;
    int16_t *mix;                      /* one top-up's worth */
    /* the display path */
    wolf_aaudio *aaudio;
    wolf_gl *gl;                       /* GL thread only */
    int64_t last_ns;                   /* the previous drawn frame's time */
    int64_t due_ns;                    /* game time not yet run */
} engine;

#define GAME_FRAME_NS 16666667LL /* the runtime's 60 frames a second */
#define STEPS_PER_DRAW_MAX 4     /* after a stall, the game catches up this far and no further (ours) */

#define AUDIO_AHEAD_MS 100 /* sound kept queued in the host's ring (ours) */

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

/* Sound into the host's ring when it handed one over; without one the game runs silent. */
static void StartSound(engine *e, int ringFd, int sampleRate)
{
    if (ringFd < 0 || sampleRate <= 0 || sampleRate > 192000) return;
    if (enginehost_audio_ring_open(&e->ring, ringFd) != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "the audio ring could not be mapped; no sound");
        memset(&e->ring, 0, sizeof e->ring);
        return;
    }
    e->rate = (uint32_t)sampleRate;
    e->mix = malloc((size_t)e->rate * AUDIO_AHEAD_MS / 1000u * 4u);
}

static void Free(engine *e)
{
    wolf_aaudio_close(e->aaudio); /* no callback into the frame after this */
    wolf_gl_destroy(e->gl);
    wolf_frame_stop(e->frame);
    if (e->ring.base) enginehost_audio_ring_close(&e->ring);
    free(e->mix);
    free(e->pixels);
    wolf_vm_destroy(e->vm);
    wolf_open_free(&e->game);
    free(e);
}

static jlong JNICALL NativeStart(JNIEnv *env, jclass cls, jstring gamePath, jint ringFd, jint sampleRate,
                                 jboolean display)
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
    } else {
        uint32_t rate = 0;
        if (display) {
            e->aaudio = wolf_aaudio_open();
            if (e->aaudio) rate = wolf_aaudio_rate(e->aaudio);
        } else {
            StartSound(e, ringFd, sampleRate);
            if (e->mix) rate = e->rate;
        }
        e->vm = StartVm(e);
        if (e->vm) e->frame = wolf_frame_start(folder, &e->game, e->vm, rate);
        if (e->frame) e->pixels = malloc((size_t)e->game.width * e->game.height * sizeof *e->pixels);
        if (e->pixels && e->aaudio && wolf_aaudio_start(e->aaudio, e->frame) != 0) {
            __android_log_print(ANDROID_LOG_WARN, TAG, "the AAudio stream did not start; the game runs silent");
        }
        if (!e->pixels) {
            snprintf(gError, sizeof gError, "Out of memory starting the Wolf core.");
            Free(e);
            e = NULL;
        } else {
            __android_log_print(ANDROID_LOG_INFO, TAG, "loaded from %s; running %u common events, pictures drawn "
                                "%s, sound %s; messages and choices are headless (logged) for now",
                                e->game.source, e->game.commons.count, display ? "with GLES" : "in software",
                                rate ? "on" : "off");
        }
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
static jint JNICALL NativeStep(JNIEnv *env, jclass cls, jlong handle, jintArray pixels, jint keys)
{
    (void)cls;
    engine *e = (engine *)(intptr_t)handle;
    jsize want = (jsize)(e->game.width * e->game.height);
    if ((*env)->GetArrayLength(env, pixels) < want) return -1;
    wolf_frame_step(e->frame, (uint32_t)keys, e->pixels); /* drawn outside the critical section */
    (*env)->SetIntArrayRegion(env, pixels, 0, want, (const jint *)e->pixels);
    if (e->mix) { /* keep the ring AUDIO_AHEAD_MS ahead */
        uint32_t ahead = e->rate * AUDIO_AHEAD_MS / 1000u;
        uint32_t queued = enginehost_audio_ring_buffered_frames(&e->ring);
        uint32_t room = enginehost_audio_ring_free_frames(&e->ring);
        uint32_t frames = queued < ahead ? ahead - queued : 0;
        if (frames > room) frames = room;
        if (frames) {
            wolf_frame_audio(e->frame, e->mix, frames);
            enginehost_audio_ring_write(&e->ring, e->mix, frames);
        }
    }
    return (jint)e->game.height; /* rows 0 .. height-1 changed */
}

/* The display path, on WolfPlugin's GL thread. A new context: everything GL is made again. */
static void JNICALL NativeGlCreated(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    engine *e = (engine *)(intptr_t)handle;
    wolf_gl_destroy(e->gl); /* its GL objects went with the old context */
    e->gl = wolf_gl_create(e->game.width, e->game.height);
    e->last_ns = 0;
}

static void JNICALL NativeGlSize(JNIEnv *env, jclass cls, jlong handle, jint width, jint height)
{
    (void)env; (void)cls;
    engine *e = (engine *)(intptr_t)handle;
    if (e->gl && width > 0 && height > 0) wolf_gl_resize(e->gl, (uint32_t)width, (uint32_t)height);
}

/* One drawn frame: the game runs the 60 fps frames that are due by now (at most a few after a
 * stall), then the pictures are drawn. The display's own rate may be 60, 90 or 120 Hz. */
static void JNICALL NativeGlDraw(JNIEnv *env, jclass cls, jlong handle, jlong nowNs, jint keys)
{
    (void)env; (void)cls;
    engine *e = (engine *)(intptr_t)handle;
    if (e->last_ns == 0) {
        e->last_ns = nowNs;
        e->due_ns = GAME_FRAME_NS; /* the first draw runs one frame */
    } else {
        e->due_ns += nowNs - e->last_ns;
        e->last_ns = nowNs;
    }
    int steps = 0;
    while (e->due_ns >= GAME_FRAME_NS && steps < STEPS_PER_DRAW_MAX) {
        wolf_frame_run(e->frame, (uint32_t)keys);
        e->due_ns -= GAME_FRAME_NS;
        steps++;
    }
    if (e->due_ns > GAME_FRAME_NS) e->due_ns = GAME_FRAME_NS; /* the rest of a stall is dropped */
    if (e->gl) wolf_gl_draw(e->gl, e->frame);
}

static void JNICALL NativePause(JNIEnv *env, jclass cls, jlong handle, jboolean paused)
{
    (void)env; (void)cls;
    engine *e = (engine *)(intptr_t)handle;
    wolf_aaudio_pause(e->aaudio, paused);
    e->last_ns = 0; /* time while paused does not count */
}

static void JNICALL NativeStop(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    Free((engine *)(intptr_t)handle);
}

/*
 * The isolated runtime loads this library from a descriptor, where nothing binds natives by
 * name, and calls this to bind WolfPlugin's itself (Enginehost's isolated_native_bridge.c).
 */
__attribute__((visibility("default")))
void enginehost_register_natives(JNIEnv *env, jclass cls)
{
    static const JNINativeMethod methods[] = {
        { "nativeStart", "(Ljava/lang/String;IIZ)J", (void *)NativeStart },
        { "nativeError", "()Ljava/lang/String;", (void *)NativeError },
        { "nativeWidth", "(J)I", (void *)NativeWidth },
        { "nativeHeight", "(J)I", (void *)NativeHeight },
        { "nativeStep", "(J[II)I", (void *)NativeStep },
        { "nativeStop", "(J)V", (void *)NativeStop },
        { "nativeGlCreated", "(J)V", (void *)NativeGlCreated },
        { "nativeGlSize", "(JII)V", (void *)NativeGlSize },
        { "nativeGlDraw", "(JJI)V", (void *)NativeGlDraw },
        { "nativePause", "(JZ)V", (void *)NativePause },
    };
    (*env)->RegisterNatives(env, cls, methods, (jint)(sizeof methods / sizeof methods[0]));
}

/*
 * In Enginehost's runtime process (the display path) the library is loaded by
 * System.loadLibrary, and nothing calls enginehost_register_natives: bind WolfPlugin's natives
 * here, through the same table.
 */
__attribute__((visibility("default")))
JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass cls = (*env)->FindClass(env, "dev/enginehost/plugin/wolf/WolfPlugin");
    if (!cls) {
        (*env)->ExceptionClear(env);
        return JNI_VERSION_1_6; /* loaded some other way: the host binds them itself */
    }
    enginehost_register_natives(env, cls);
    return JNI_VERSION_1_6;
}

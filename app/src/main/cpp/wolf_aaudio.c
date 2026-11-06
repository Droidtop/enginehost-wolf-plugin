/* See wolf_aaudio.h. */
#include "wolf_aaudio.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <stdlib.h>

#define TAG "wolf"

struct wolf_aaudio {
    AAudioStream *stream;
    wolf_frame *frame;
    uint32_t rate;
};

static aaudio_data_callback_result_t pull(AAudioStream *s, void *ctx, void *data, int32_t frames) {
    (void)s;
    wolf_aaudio *a = ctx;
    if (frames > 0) wolf_frame_audio(a->frame, (int16_t *)data, (uint32_t)frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

wolf_aaudio *wolf_aaudio_open(void) {
    wolf_aaudio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
        free(a);
        return NULL;
    }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, pull, a);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &a->stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || AAudioStream_getFormat(a->stream) != AAUDIO_FORMAT_PCM_I16 ||
        AAudioStream_getChannelCount(a->stream) != 2 || AAudioStream_getSampleRate(a->stream) <= 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no AAudio stream (%s); the game runs silent",
                            r != AAUDIO_OK ? AAudio_convertResultToText(r) : "not stereo 16-bit");
        if (r == AAUDIO_OK) AAudioStream_close(a->stream);
        free(a);
        return NULL;
    }
    a->rate = (uint32_t)AAudioStream_getSampleRate(a->stream);
    return a;
}

uint32_t wolf_aaudio_rate(const wolf_aaudio *a) { return a->rate; }

int wolf_aaudio_start(wolf_aaudio *a, wolf_frame *f) {
    a->frame = f;
    return AAudioStream_requestStart(a->stream) == AAUDIO_OK ? 0 : -1;
}

void wolf_aaudio_pause(wolf_aaudio *a, int paused) {
    if (!a || !a->frame) return;
    if (paused) AAudioStream_requestPause(a->stream);
    else AAudioStream_requestStart(a->stream);
}

void wolf_aaudio_close(wolf_aaudio *a) {
    if (!a) return;
    /* stopped, then closed: no callback runs after this returns, so the frame can go */
    AAudioStream_requestStop(a->stream);
    AAudioStream_close(a->stream);
    free(a);
}

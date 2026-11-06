/* See wolf_decode.h. */
#include "wolf_decode.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

static int ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), e = strlen(ext);
    return n >= e && strcasecmp(s + n - e, ext) == 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* RIFF/WAVE, PCM only (format 1), 8 or 16 bits, 1 or 2 channels; every size checked. */
static int wav(const uint8_t *d, size_t n, wolf_pcm *out) {
    if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) return WOLF_DECODE_BAD;
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *samples = NULL;
    size_t samples_len = 0;
    size_t at = 12;
    while (at + 8 <= n) {
        uint32_t size = le32(d + at + 4);
        const uint8_t *body = d + at + 8;
        size_t room = n - at - 8;
        if (size > room) size = (uint32_t)room; /* a short last chunk: take what is there */
        if (memcmp(d + at, "fmt ", 4) == 0 && size >= 16) {
            format = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
        } else if (memcmp(d + at, "data", 4) == 0) {
            samples = body;
            samples_len = size;
        }
        at += 8 + (size_t)size + (size & 1);
    }
    if (format != 1 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16) || !rate || !samples)
        return format && format != 1 ? WOLF_DECODE_UNSUPPORTED : WOLF_DECODE_BAD;
    size_t per = (size_t)channels * (bits / 8u);
    size_t frames = samples_len / per;
    if (frames == 0 || frames > WOLF_DECODE_FRAMES_MAX) return WOLF_DECODE_BAD;
    int16_t *s = malloc(frames * 2 * sizeof *s);
    if (!s) return WOLF_DECODE_BAD;
    for (size_t i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            size_t ch = channels == 2 ? (size_t)c : 0;
            const uint8_t *p = samples + i * per + ch * (bits / 8u);
            s[i * 2 + c] = bits == 16 ? (int16_t)le16(p) : (int16_t)(((int)p[0] - 128) * 256);
        }
    }
    out->rate = rate;
    out->frames = (uint32_t)frames;
    out->stereo = s;
    return WOLF_DECODE_OK;
}

static int ogg(const uint8_t *d, size_t n, wolf_pcm *out) {
    if (n > 0x7FFFFFFF) return WOLF_DECODE_BAD;
    int channels = 0, rate = 0;
    short *pcm = NULL;
    int frames = stb_vorbis_decode_memory(d, (int)n, &channels, &rate, &pcm);
    if (frames <= 0 || !pcm || channels < 1 || rate <= 0 || (uint32_t)frames > WOLF_DECODE_FRAMES_MAX) {
        free(pcm);
        return WOLF_DECODE_BAD;
    }
    int16_t *s = malloc((size_t)frames * 2 * sizeof *s);
    if (!s) { free(pcm); return WOLF_DECODE_BAD; }
    for (size_t i = 0; i < (size_t)frames; i++) {
        s[i * 2] = pcm[i * (size_t)channels];
        s[i * 2 + 1] = pcm[i * (size_t)channels + (channels > 1 ? 1 : 0)];
    }
    free(pcm);
    out->rate = (uint32_t)rate;
    out->frames = (uint32_t)frames;
    out->stereo = s;
    return WOLF_DECODE_OK;
}

int wolf_decode_sound(const char *name, const uint8_t *data, size_t len, wolf_pcm *out) {
    memset(out, 0, sizeof *out);
    if (ends_with(name, ".ogg")) return ogg(data, len, out);
    if (ends_with(name, ".wav")) return wav(data, len, out);
    /* by content too: games name files freely */
    if (len >= 4 && memcmp(data, "OggS", 4) == 0) return ogg(data, len, out);
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0) return wav(data, len, out);
    return WOLF_DECODE_UNSUPPORTED; /* MP3, MIDI and anything else */
}

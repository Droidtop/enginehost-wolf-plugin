/* Tests for the mixer (wolf_mixer.c), with synthetic sounds. */
#include "../src/wolf_mixer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)

#define RATE 600 /* output frames a second: 10 per game frame, so fades are easy to count */

/* A constant sound of n frames at level (left) and -level (right). */
static wolf_pcm make(uint32_t n, int16_t level, uint32_t rate) {
    int16_t *s = malloc(n * 2 * sizeof *s);
    for (uint32_t i = 0; i < n; i++) { s[i * 2] = level; s[i * 2 + 1] = (int16_t)-level; }
    wolf_pcm p = {rate, n, s};
    return p;
}

static wolf_sound req(int kind, int32_t volume, int32_t frequency, int32_t fade) {
    wolf_sound r;
    memset(&r, 0, sizeof r);
    r.kind = kind;
    r.action = WOLF_SOUND_PLAY;
    r.file = "";
    r.volume = volume;
    r.frequency = frequency;
    r.fade_frames = fade;
    return r;
}

static int16_t out[RATE * 4];

int main(void) {
    wolf_pcm bgm = make(100, 1000, RATE), bgm2 = make(100, 300, RATE), se = make(20, 2000, RATE);
    /* a sound whose second half is louder: tells positions apart */
    wolf_pcm ramp = make(100, 0, RATE);
    for (uint32_t i = 50; i < 100; i++) ((int16_t *)ramp.stereo)[i * 2] = 500;
    wolf_mixer *m = wolf_mixer_create(RATE);

    /* silence */
    wolf_mixer_mix(m, out, 10);
    CHECK(out[0] == 0 && out[19] == 0);

    /* BGM at 50%: half the level, both channels, and it loops past its end */
    wolf_sound r = req(WOLF_SOUND_BGM, 50, 100, 0);
    wolf_mixer_play(m, &r, &bgm);
    wolf_mixer_mix(m, out, 250);
    CHECK(out[0] == 500 && out[1] == -500 && out[2 * 249] == 500 && wolf_mixer_playing(m, &bgm));

    /* the same BGM again: new volume, not restarted; another BGM replaces it */
    r.volume = 100;
    wolf_mixer_play(m, &r, &bgm);
    wolf_mixer_mix(m, out, 1);
    CHECK(out[0] == 1000);
    wolf_mixer_play(m, &r, &bgm2);
    wolf_mixer_mix(m, out, 1);
    CHECK(out[0] == 300 && !wolf_mixer_playing(m, &bgm));

    /* an SE on top, once: the sum, then the SE ends after its 20 frames */
    wolf_sound s = req(WOLF_SOUND_SE, 100, 100, 0);
    wolf_mixer_play(m, &s, &se);
    wolf_mixer_mix(m, out, 30);
    CHECK(out[0] == 2300 && out[2 * 19] == 2300 && out[2 * 20] == 300 && !wolf_mixer_playing(m, &se));
    /* at double frequency it ends twice as fast */
    s.frequency = 200;
    wolf_mixer_play(m, &s, &se);
    wolf_mixer_mix(m, out, 12);
    CHECK(out[2 * 9] == 2300 && out[2 * 10] == 300);
    /* the sum clamps */
    s.frequency = 100;
    for (int i = 0; i < 20; i++) wolf_mixer_play(m, &s, &se);
    wolf_mixer_mix(m, out, 1);
    CHECK(out[0] == 32767 && out[1] == -32768);
    wolf_sound stop_se = req(WOLF_SOUND_SE, 0, 0, 0);
    wolf_mixer_stop(m, &stop_se);
    CHECK(!wolf_mixer_playing(m, &se));

    /* more SEs than voices: the oldest gives way, the newest plays */
    for (int i = 0; i < WOLF_MIXER_SE_VOICES; i++) wolf_mixer_play(m, &s, &se);
    wolf_pcm last = make(5, 7, RATE);
    wolf_mixer_play(m, &s, &last);
    CHECK(wolf_mixer_playing(m, &last) && wolf_mixer_playing(m, &se));
    wolf_mixer_forget(m, &se);
    wolf_mixer_forget(m, &last);
    CHECK(!wolf_mixer_playing(m, &se) && !wolf_mixer_playing(m, &last));

    /* fade in over 6 game frames (60 output frames), fade out the same, then silence */
    wolf_mixer_stop(m, &(wolf_sound){WOLF_SOUND_BGM, WOLF_SOUND_STOP, "", 0, 0, 0, 0});
    CHECK(!wolf_mixer_playing(m, &bgm2));
    r = req(WOLF_SOUND_BGM, 100, 100, 6);
    wolf_mixer_play(m, &r, &bgm);
    wolf_mixer_mix(m, out, 70);
    CHECK(out[0] == 0 && out[2 * 30] > 400 && out[2 * 30] < 600 && out[2 * 65] == 1000);
    wolf_sound fade_out = {WOLF_SOUND_BGM, WOLF_SOUND_STOP, "", 6, 0, 0, 0};
    wolf_mixer_stop(m, &fade_out);
    wolf_mixer_mix(m, out, 70);
    CHECK(out[2 * 30] > 400 && out[2 * 30] < 600 && out[2 * 65] == 0 && !wolf_mixer_playing(m, &bgm));

    /* start position (BGM): 1/12 s in at RATE 600 is frame 50, the loud half */
    r = req(WOLF_SOUND_BGS, 100, 100, 0);
    r.start_ms = 1000 / 12 + 1;
    wolf_mixer_play(m, &r, &ramp);
    wolf_mixer_mix(m, out, 1);
    CHECK(out[0] == 500);
    /* asked again, the same music carries on from where it is (the loud half), not from the top */
    wolf_mixer_mix(m, out, 10);
    r.start_ms = 0; /* a restart would land in the quiet half */
    wolf_mixer_play(m, &r, &ramp);
    wolf_mixer_mix(m, out, 1);
    CHECK(out[0] == 500);
    /* a sound at another rate is resampled: half the output rate plays at half speed */
    wolf_mixer_stop(m, &(wolf_sound){WOLF_SOUND_BGS, WOLF_SOUND_STOP, "", 0, 0, 0, 0});
    wolf_pcm slow = make(10, 100, RATE / 2);
    wolf_mixer_play(m, &s, &slow);
    wolf_mixer_mix(m, out, 25);
    CHECK(out[2 * 19] == 100 && out[2 * 20] == 0);
    /* refusals: nothing to play */
    wolf_pcm empty = {RATE, 0, NULL};
    wolf_mixer_play(m, &s, &empty);
    wolf_mixer_play(m, &s, NULL);
    CHECK(!wolf_mixer_playing(m, &empty));

    wolf_mixer_destroy(m);
    free((void *)bgm.stereo); free((void *)bgm2.stereo); free((void *)se.stereo); free((void *)ramp.stereo);
    free((void *)last.stereo); free((void *)slow.stereo);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("mixer: all checks passed");
    return 0;
}

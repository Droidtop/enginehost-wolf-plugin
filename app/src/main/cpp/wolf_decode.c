/* See wolf_decode.h. The service's API is being built on Droidtop/tracker#906; what the Wolf core
 * needs from it is posted there (bytes in, since most sounds sit inside .wolf archives; s16 PCM
 * out with its rate and channels; a start offset). */
#include "wolf_decode.h"

#include <string.h>

int wolf_decode_sound(const char *name, const uint8_t *data, size_t len, wolf_pcm *out) {
    (void)name;
    (void)data;
    (void)len;
    memset(out, 0, sizeof *out);
    return WOLF_DECODE_WAITING;
}

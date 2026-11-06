/* See wolf_gl.h. Blends follow docs/behaviour/drawing.md, the same rules as the software renderer. */
#include "wolf_gl.h"

#include <GLES2/gl2.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define TAG "wolf"
#define TEXTURE_BYTES_MAX (256u << 20) /* texture memory kept; past this, all go and are made again */

typedef struct {
    char *file;
    GLuint id;      /* 0: the image is too large for this GPU */
    uint32_t w, h;
} texture;

struct wolf_gl {
    uint32_t gw, gh;             /* the game's screen */
    uint32_t vw, vh;             /* the view */
    GLuint program;
    GLint a_pos, a_uv, u_tex, u_mul, u_multiply;
    GLint max_texture;
    texture *tex;
    uint32_t ntex, captex;
    size_t tex_bytes;
};

static const char VERTEX[] =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

/* u_mul: colour multipliers and opacity. Multiply mode outputs mix(1, colour, alpha) for a
 * DST_COLOR x src blend, which gives dst * (1 - a + colour * a), as the software renderer does. */
static const char FRAGMENT[] =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_mul;\n"
    "uniform float u_multiply;\n"
    "void main() {\n"
    "  vec4 c = texture2D(u_tex, v_uv);\n"
    "  vec3 rgb = min(c.rgb * u_mul.rgb, vec3(1.0));\n"
    "  float a = c.a * u_mul.a;\n"
    "  if (u_multiply > 0.5) gl_FragColor = vec4(mix(vec3(1.0), rgb, a), 1.0);\n"
    "  else gl_FragColor = vec4(rgb, a);\n"
    "}\n";

static GLuint shader(GLenum kind, const char *src) {
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GL shader: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

wolf_gl *wolf_gl_create(uint32_t gw, uint32_t gh) {
    wolf_gl *gl = calloc(1, sizeof *gl);
    if (!gl) return NULL;
    gl->gw = gw;
    gl->gh = gh;
    GLuint vs = shader(GL_VERTEX_SHADER, VERTEX), fs = shader(GL_FRAGMENT_SHADER, FRAGMENT);
    if (!vs || !fs) {
        free(gl);
        return NULL;
    }
    gl->program = glCreateProgram();
    glAttachShader(gl->program, vs);
    glAttachShader(gl->program, fs);
    glLinkProgram(gl->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(gl->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GL program did not link");
        free(gl);
        return NULL;
    }
    gl->a_pos = glGetAttribLocation(gl->program, "a_pos");
    gl->a_uv = glGetAttribLocation(gl->program, "a_uv");
    gl->u_tex = glGetUniformLocation(gl->program, "u_tex");
    gl->u_mul = glGetUniformLocation(gl->program, "u_mul");
    gl->u_multiply = glGetUniformLocation(gl->program, "u_multiply");
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl->max_texture);
    __android_log_print(ANDROID_LOG_INFO, TAG, "GLES renderer: %s, %s, max texture %d", glGetString(GL_RENDERER),
                        glGetString(GL_VERSION), gl->max_texture);
    return gl;
}

void wolf_gl_resize(wolf_gl *gl, uint32_t vw, uint32_t vh) {
    gl->vw = vw;
    gl->vh = vh;
}

static void textures_clear(wolf_gl *gl) {
    for (uint32_t i = 0; i < gl->ntex; i++) {
        if (gl->tex[i].id) glDeleteTextures(1, &gl->tex[i].id);
        free(gl->tex[i].file);
    }
    gl->ntex = 0;
    gl->tex_bytes = 0;
}

/* The texture for a quad's image, uploaded on first use (ARGB words to RGBA bytes). */
static const texture *texture_for(wolf_gl *gl, const wolf_quad *q) {
    for (uint32_t i = 0; i < gl->ntex; i++)
        if (strcmp(gl->tex[i].file, q->file) == 0) return &gl->tex[i];
    if (gl->tex_bytes > TEXTURE_BYTES_MAX) textures_clear(gl);
    if (gl->ntex == gl->captex) {
        uint32_t cap = gl->captex ? gl->captex * 2 : 64;
        texture *t = realloc(gl->tex, cap * sizeof *t);
        if (!t) return NULL;
        gl->tex = t;
        gl->captex = cap;
    }
    texture *t = &gl->tex[gl->ntex];
    memset(t, 0, sizeof *t);
    size_t n = strlen(q->file);
    t->file = malloc(n + 1);
    if (!t->file) return NULL;
    memcpy(t->file, q->file, n + 1);
    const wolf_image *im = q->image;
    t->w = im->width;
    t->h = im->height;
    gl->ntex++;
    if ((GLint)im->width > gl->max_texture || (GLint)im->height > gl->max_texture) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "image %s is %ux%u, larger than this GPU's %d; not drawn", q->file,
                            im->width, im->height, gl->max_texture);
        return t;
    }
    size_t count = (size_t)im->width * im->height;
    uint8_t *rgba = malloc(count * 4);
    if (!rgba) return t;
    for (size_t i = 0; i < count; i++) {
        uint32_t p = im->argb[i];
        rgba[i * 4] = (uint8_t)(p >> 16);
        rgba[i * 4 + 1] = (uint8_t)(p >> 8);
        rgba[i * 4 + 2] = (uint8_t)p;
        rgba[i * 4 + 3] = (uint8_t)(p >> 24);
    }
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)im->width, (GLsizei)im->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba);
    free(rgba);
    /* nearest, as the software renderer samples [M: DX Library's filtering is Unknown] */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->tex_bytes += count * 4;
    return t;
}

static void blend_for(int blend) {
    switch (blend) {
    case WOLF_BLEND_ADD:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    case WOLF_BLEND_SUBTRACT: /* dst - src * a */
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    case WOLF_BLEND_MULTIPLY: /* dst * (the shader's mix) */
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
        break;
    default:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    }
}

static void draw_quad(void *ctx, const wolf_quad *q) {
    wolf_gl *gl = ctx;
    const texture *t = texture_for(gl, q);
    if (!t || !t->id) return;
    /* game pixels to clip space; texture pixels to 0-1 */
    GLfloat pos[8], uv[8];
    for (int k = 0; k < 4; k++) {
        pos[k * 2] = (GLfloat)(q->x[k] / gl->gw * 2.0 - 1.0);
        pos[k * 2 + 1] = (GLfloat)(1.0 - q->y[k] / gl->gh * 2.0);
    }
    const double us[4] = {q->u0, q->u1, q->u1, q->u0}, vs[4] = {q->v0, q->v0, q->v1, q->v1};
    for (int k = 0; k < 4; k++) {
        uv[k * 2] = (GLfloat)(us[k] / t->w);
        uv[k * 2 + 1] = (GLfloat)(vs[k] / t->h);
    }
    static const GLushort order[6] = {0, 1, 2, 0, 2, 3};
    glBindTexture(GL_TEXTURE_2D, t->id);
    glUniform4f(gl->u_mul, (GLfloat)q->r, (GLfloat)q->g, (GLfloat)q->b, (GLfloat)q->opacity);
    glUniform1f(gl->u_multiply, q->blend == WOLF_BLEND_MULTIPLY ? 1.0f : 0.0f);
    blend_for(q->blend);
    glVertexAttribPointer((GLuint)gl->a_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer((GLuint)gl->a_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, order);
}

void wolf_gl_draw(wolf_gl *gl, wolf_frame *f) {
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, (GLsizei)gl->vw, (GLsizei)gl->vh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!gl->vw || !gl->vh || !gl->gw || !gl->gh) return;
    /* the game's screen, scaled to fit and centred; the rest stays black */
    double scale = (double)gl->vw / gl->gw;
    if ((double)gl->vh / gl->gh < scale) scale = (double)gl->vh / gl->gh;
    GLsizei w = (GLsizei)(gl->gw * scale), h = (GLsizei)(gl->gh * scale);
    GLint x = (GLint)(gl->vw - (uint32_t)w) / 2, y = (GLint)(gl->vh - (uint32_t)h) / 2;
    glViewport(x, y, w, h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glUseProgram(gl->program);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(gl->u_tex, 0);
    glEnableVertexAttribArray((GLuint)gl->a_pos);
    glEnableVertexAttribArray((GLuint)gl->a_uv);
    glEnable(GL_BLEND);
    wolf_frame_quads(f, draw_quad, gl);
    glDisable(GL_SCISSOR_TEST);
}

void wolf_gl_destroy(wolf_gl *gl) {
    if (!gl) return;
    for (uint32_t i = 0; i < gl->ntex; i++) free(gl->tex[i].file);
    free(gl->tex);
    free(gl);
}

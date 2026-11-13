// VR stereo rendering: eyes, the stereo textures' twins, the scene camera's per-eye block. See vr.h.
#include "vr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "vk.h"

int g_vr_eye;

bool vr_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VR"); on = e && atoi(e) != 0; if (on) LOG("[vr] stereo rendering on\n"); }
    return on;
}

void vr_frame_begin(void) {}

// ------------------------------------------------------------------ the eyes
// The eye's place in the game camera's space (x right, y up, z back: GL view space), in the game's world units.
// LARECOMP_VR_IPD=<world units>: the distance between the eyes (until the headset gives it, with the world scale).
static float ipd_world(void) {
    static float v = -1;
    if (v < 0) { const char *e = getenv("LARECOMP_VR_IPD"); v = e ? (float)atof(e) : 0.064f * 10.0f; }
    return v;
}

// ------------------------------------------------------------------ stereo textures
static struct TexBackend **g_reg;
static int g_nreg, g_capreg;

void vr_register(TexBackend *b) {
    if (g_nreg == g_capreg) {
        g_capreg = g_capreg ? g_capreg * 2 : 64;
        g_reg = realloc(g_reg, sizeof *g_reg * (size_t)g_capreg);
    }
    g_reg[g_nreg++] = b;
    static int nlog;
    if (nlog++ < 40) LOG("[vr] stereo texture %ux%u fmt %d (%d stereo)\n", b->width, b->height, b->format, g_nreg);
}

void vr_unregister(TexBackend *b) {
    for (int i = 0; i < g_nreg; i++)
        if (g_reg[i] == b) { g_reg[i] = g_reg[--g_nreg]; return; }
}

void vr_swap(int eye) {
    // exec.c's per-frame texture memo and stage_set's set cache key on the texture object and vk_view_gen: the swap
    // changes the images behind the objects, so both must miss
    extern uint32_t vk_view_gen;
    vk_view_gen++;
    for (int i = 0; i < g_nreg; i++) {
        TexBackend *b = g_reg[i], *to = eye ? b->vr_twin : b;
        for (int k = 0; k < b->nusers; k++) b->users[k]->backend = to;
    }
}

bool vr_tex_stereo(const NvnTexture *t) {
    const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
    return b && (b->vr_twin || b->vr_is_twin);
}

void vr_mark(NvnTexture *t) {
    extern TexBackend *vk_tex_twin(NvnTexture *t);  // textures.c
    if (t && !vr_tex_stereo(t)) vk_tex_twin(t);
}

// ------------------------------------------------------------------ the scene camera's block
// sead / agl's view block (vec4 rows; GL conventions, column vectors, matrices stored as rows):
//   0-2 view (world -> view, 3x4), 3-6 view-projection, 7-10 projection, 11-13 inverse view (view -> world: its last
//   column is the camera's position), 14 near / far, 15 aspect, 17 tan(fov / 2) x / y and fov y, 18 render size,
//   21-34 the previous frame's 0-13, 42-49 the shadow cascades' matrices.
// It is the scene camera's when rows 7-10 are a perspective projection (row 10 = 0 0 -1 0) and rows 3-6 are that
// projection times the view (the shadow cameras' blocks have the same layout and orthographic projections).
#define ROWS_CUR 0
#define ROWS_PREV 21
bool vr_is_scene_camera(const void *p, uint64_t size) {
    if (size < 35 * 16) return false;
    const float (*r)[4] = p;
    if (fabsf(r[10][0]) > 1e-4f || fabsf(r[10][1]) > 1e-4f || fabsf(r[10][2] + 1.0f) > 1e-3f || fabsf(r[10][3]) > 1e-4f) return false;
    if (r[7][0] < 0.05f || r[8][1] < 0.05f || r[9][2] > -0.5f) return false;
    float vp = r[7][0] * r[0][0] + r[7][2] * r[2][0];  // (row 0 of P x V, first column)
    return fabsf(vp - r[3][0]) < 1e-3f * (1.0f + fabsf(r[3][0]));
}

// One view set (view, view-projection, projection, inverse view at rows base..base+13) moved to the eye at `ex` (view
// space x, world units): view' = translate(-ex) x view.
static void patch_set(float (*r)[4], int base, float ex) {
    float (*V)[4] = r + base, (*VP)[4] = r + base + 3, (*P)[4] = r + base + 7, (*IV)[4] = r + base + 11;
    V[0][3] -= ex;
    for (int i = 0; i < 3; i++) IV[i][3] += ex * V[0][i];  // (the camera moves along its right axis: row 0 of the view)
    for (int i = 0; i < 4; i++)                              // view-projection = projection x view (view's row 3: 0 0 0 1)
        for (int j = 0; j < 4; j++)
            VP[i][j] = P[i][0] * V[0][j] + P[i][1] * V[1][j] + P[i][2] * V[2][j] + (j == 3 ? P[i][3] : 0.0f);
}

void vr_patch_camera(float *u, uint64_t size, int eye) {
    float (*r)[4] = (float (*)[4])u;
    float ex = (eye ? 0.5f : -0.5f) * ipd_world();
    patch_set(r, ROWS_CUR, ex);
    if (size >= (ROWS_PREV + 14) * 16) patch_set(r, ROWS_PREV, ex);
}

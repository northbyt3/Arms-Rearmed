// VR stereo rendering: eyes, the stereo textures' twins, the scene camera's per-eye block. See vr.h.
#include "vr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "record.h"
#include "vk.h"

int g_vr_eye;

bool vr_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VR"); on = e && atoi(e) != 0; if (on) { LOG("[vr] stereo rendering on\n"); extern void vr_settings_init(void); vr_settings_init(); } }
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

static NvnTexture *hud_t;      // (the HUD section) while a HUD draw's target is swapped: the target and its own image
static TexBackend *hud_saved;

bool vr_tex_stereo(const NvnTexture *t) {
    const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
    if (t && t == hud_t) b = hud_saved;  // (its own image, not the HUD image it draws into for now)
    return b && (b->vr_twin || b->vr_is_twin);
}

void vr_mark(NvnTexture *t) {
    extern TexBackend *vk_tex_twin(NvnTexture *t);  // textures.c
    if (t && t != hud_t && !vr_tex_stereo(t)) vk_tex_twin(t);
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

// The game camera levelled: its yaw kept, pitch and roll gone (the camera's place unchanged). In VR the user's head
// sets pitch and roll; a camera pitched by the game while the head is level tilts the whole world.
// dy: moved up by that much (the height stabilisation, vr_frame_end)
static void level_set(float (*r)[4], int base, float dy) {
    float (*V)[4] = r + base, (*IV)[4] = r + base + 11;
    float c[3] = {IV[0][3], IV[1][3] + dy, IV[2][3]};  // the camera's place
    float bx = V[2][0], bz = V[2][2], l = sqrtf(bx * bx + bz * bz);
    if (l < 1e-4f) return;  // (looking straight down: keep it)
    bx /= l, bz /= l;
    float R[3][3] = {{bz, 0, -bx}, {0, 1, 0}, {bx, 0, bz}};  // rows: right, up, back (world)
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) V[i][j] = R[i][j], IV[j][i] = R[i][j];
        V[i][3] = -(R[i][0] * c[0] + R[i][1] * c[1] + R[i][2] * c[2]);
        IV[i][3] = c[i];
    }
}

// One view set moved to a headset eye: E = the eye's pose in the game camera's space (rotation R, place t in game
// units); view' = E^-1 x view, inverse view' = inverse view x E, the projection the eye's field of view (its depth
// rows as the game's: same near / far and depth convention).
static bool cam_stab;        // the height stabilisation applies this frame (vr_frame_end)
static float cam_smooth_y;   // the camera's smoothed height
static void xr_set(float (*r)[4], int base, const float q[4], const float t[3], const float tan[4], bool level) {
    if (level) level_set(r, base, cam_stab ? cam_smooth_y - r[base + 12][3] : 0.0f);
    float (*V)[4] = r + base, (*VP)[4] = r + base + 3, (*P)[4] = r + base + 7, (*IV)[4] = r + base + 11;
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                     {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                     {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    float nv[3][4], niv[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) nv[i][j] = R[0][i] * V[0][j] + R[1][i] * V[1][j] + R[2][i] * V[2][j];  // R^T x V
        nv[i][3] -= R[0][i] * t[0] + R[1][i] * t[1] + R[2][i] * t[2];
        for (int j = 0; j < 3; j++) niv[i][j] = IV[i][0] * R[0][j] + IV[i][1] * R[1][j] + IV[i][2] * R[2][j];
        niv[i][3] = IV[i][0] * t[0] + IV[i][1] * t[1] + IV[i][2] * t[2] + IV[i][3];
    }
    memcpy(V, nv, sizeof nv);
    memcpy(IV, niv, sizeof niv);
    float l = tan[0], rr = tan[1], u = tan[2], d = tan[3];
    P[0][0] = 2 / (rr - l), P[0][1] = 0, P[0][2] = (rr + l) / (rr - l), P[0][3] = 0;
    P[1][0] = 0, P[1][1] = 2 / (u - d), P[1][2] = (u + d) / (u - d), P[1][3] = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            VP[i][j] = P[i][0] * V[0][j] + P[i][1] * V[1][j] + P[i][2] * V[2][j] + (j == 3 ? P[i][3] : 0.0f);
}

void vr_patch_camera(float *u, uint64_t size, int eye) {
    float (*r)[4] = (float (*)[4])u;
    float q[4], p[3], tan[4];
    if (vr_eye_view(eye, q, p, tan)) {
        float t[3] = {p[0] * g_vrs.world_scale, p[1] * g_vrs.world_scale, p[2] * g_vrs.world_scale};
        xr_set(r, ROWS_CUR, q, t, tan, g_vrs.level);
        if (size >= (ROWS_PREV + 14) * 16) xr_set(r, ROWS_PREV, q, t, tan, g_vrs.level);
        if (size >= 18 * 16) {  // tan(fov / 2) x / y (screen-space effects): the eye's, as if symmetric
            r[17][0] = (tan[1] - tan[0]) * 0.5f;
            r[17][1] = (tan[2] - tan[3]) * 0.5f;
        }
        vr_note_scene();
        vr_note_scene_frame();
        return;
    }
    float ex = (eye ? 0.5f : -0.5f) * ipd_world();
    patch_set(r, ROWS_CUR, ex);
    if (size >= (ROWS_PREV + 14) * 16) patch_set(r, ROWS_PREV, ex);
}

// ------------------------------------------------------------------ which frames are immersive
// Only fights put the player in the arena: the fight camera (one scene camera, ARMS's 55 degree field of view) for
// 8 frames in a row turns it on, 20 frames without it turn it off. Everything else - menus, the ARMS LIVE intros,
// the VS screen (two cameras), KO replays, cutscenes - is shown on the big screen, flat (one eye's pass only).
// LARECOMP_VR_MODE=screen: always the big screen; =scene: every frame with a scene camera immersive.
static uint64_t frame_id;  // (render thread) frames presented since the start: the frame being translated
uint64_t vr_frame_id(void) { return frame_id; }
static int ncam;
static int nscene_tgt;  // (the HUD section below)
static const TexBackend *hud_frame_target;
static bool hud_began[2];  // this frame, per eye: a HUD draw was made (later draws into that target are the HUD's too)
static float cam_key[4][4];
static float cam_fovy, cam_pos[3], cam_yaw;  // (the frame's first scene camera)
static float vignette;                       // the comfort vignette's strength now (0..1)
float vr_vignette(void) { return vignette; }
static bool immersive;
static int good, bad;

void vr_note_camera(const float *u, uint64_t size) {
    const float (*r)[4] = (const float (*)[4])u;
    float k[4] = {r[11][3], r[12][3], r[13][3], r[7][0]};
    for (int i = 0; i < ncam && i < 4; i++) if (!memcmp(cam_key[i], k, sizeof k)) return;
    if (ncam < 4) memcpy(cam_key[ncam], k, sizeof k);
    if (!ncam) {
        cam_fovy = size >= 18 * 16 ? r[17][2] : 0;
        cam_pos[0] = r[11][3], cam_pos[1] = r[12][3], cam_pos[2] = r[13][3];
        cam_yaw = atan2f(-r[2][0], r[2][2]);  // (forward = -row 2: its heading)
    }
    ncam++;
    static int camlog = -1;
    if (camlog < 0) camlog = getenv("LARECOMP_VR_CAMLOG") != NULL;
    if (camlog) {
        LOG("[vrcam] f %llu #%d pos %.1f %.1f %.1f fwd %.2f %.2f %.2f P %.3f %.3f fovy %.3f size %.0fx%.0f\n",
            (unsigned long long)vr_frame_id(), ncam, r[11][3], r[12][3], r[13][3], -r[2][0], -r[2][1], -r[2][2], r[7][0],
            r[8][1], r[17][2], r[18][0], r[18][1]);
    }
}

bool vr_immersive(void) { return immersive; }

bool vr_stereo_frame(void) {
    float q[4], p[3], tan[4];
    return immersive && vr_eye_view(0, q, p, tan);
}

void vr_frame_end(void) {
    static int mode = -1;  // 0 auto, 1 screen, 2 scene
    if (mode < 0) {
        const char *e = getenv("LARECOMP_VR_MODE");
        mode = e && !strcmp(e, "screen") ? 1 : e && !strcmp(e, "scene") ? 2 : 0;
    }
    // the fight starts at the camera's resting 55 degrees; during it the camera zooms (42-57 degrees seen) - while
    // immersive, one camera with any such field of view keeps it so
    bool fight = mode == 2 ? ncam >= 1 : mode == 1 ? false
               : immersive ? ncam == 1 && cam_fovy > 0.6f && cam_fovy < 1.15f
                           : ncam == 1 && fabsf(cam_fovy - 0.9599f) < 0.035f;
    if (fight) good++, bad = 0;
    else bad++, good = 0;
    {   // height stabilisation (LARECOMP_VR_STABLE_HEIGHT=0: off): the camera's height smoothed over ~1.5 s, so a
        // jump doesn't lift the player's head with it (they look up at their fighter instead); a cut resets it
        static int stab = -1;
        static float smooth[3];
        static bool have;
        if (stab < 0) stab = !getenv("LARECOMP_VR_STABLE_HEIGHT") || atoi(getenv("LARECOMP_VR_STABLE_HEIGHT"));
        float dx = cam_pos[0] - smooth[0], dz = cam_pos[2] - smooth[2];
        if (!stab || !ncam || !immersive || !have || dx * dx + dz * dz > 25.0f * 25.0f) {
            memcpy(smooth, cam_pos, sizeof smooth);
            have = ncam > 0;
        } else {
            smooth[0] = cam_pos[0], smooth[2] = cam_pos[2];
            smooth[1] += (cam_pos[1] - smooth[1]) * 0.011f;  // (1 - e^(-1/90): ~1.5 s at 60 fps)
        }
        cam_stab = stab && immersive && have;  // (the next frame's camera goes to that height)
        cam_smooth_y = smooth[1];
    }
    {   // comfort vignette: the edges of the view darken while the game camera turns or moves fast (it yaws > 30
        // degrees/s a fifth of the time in a fight, dashes move it ~6 m/s): less of the artificial motion in the
        // periphery, where it makes people sick. LARECOMP_VR_VIGNETTE=0..1 (default 0.7, 0: off)
        static float last_yaw, last_pos[3];
        static bool have;
        float target = 0;
        if (immersive && ncam && have) {
            float dyaw = fabsf(remainderf(cam_yaw - last_yaw, 6.2831853f)) * 60 * 57.29578f;  // degrees/s
            float dx = cam_pos[0] - last_pos[0], dy = cam_pos[1] - last_pos[1], dz = cam_pos[2] - last_pos[2];
            float speed = sqrtf(dx * dx + dy * dy + dz * dz) * 60 / (g_vrs.world_scale > 0 ? g_vrs.world_scale : 10);  // m/s
            if (speed < 30) {  // (more: a camera cut, not motion)
                float a = (dyaw - 25) / 90, b = (speed - 3) / 8;
                target = fminf(1, fmaxf(0, fmaxf(a, b)));
            }
        }
        have = ncam > 0;
        last_yaw = cam_yaw;
        memcpy(last_pos, cam_pos, sizeof last_pos);
        float k = target > vignette ? 0.25f : 0.04f;  // (in fast, out slowly)
        vignette += (target - vignette) * k;
        if (!immersive) vignette = 0;
    }
    bool was = immersive;
    if (!immersive && good >= 8) immersive = true;
    if (immersive && bad >= 20) immersive = false;
    if (was != immersive) {
        extern void vr_xr_recenter(void);
        vr_xr_recenter();  // (the arena / the screen in front of the player)
        LOG("[vr] frame %llu: %s\n", (unsigned long long)vr_frame_id(),
            immersive ? "in the arena (immersive, head tracked)" : "on the big screen");
    }
    if (frame_id % 1800 == 0)  // (every ~30 s: the stereo images, to see none pile up)
        LOG("[vr] status: frame %llu, %s, %d stereo images, vignette %.2f\n", (unsigned long long)frame_id,
            immersive ? "arena" : "big screen", g_nreg, vignette);
    frame_id++;
    hud_began[0] = hud_began[1] = false;
    ncam = 0;
    nscene_tgt = 0;
    hud_frame_target = NULL;
}

// ------------------------------------------------------------------ the HUD drawn apart
// In a fight (the scene camera drawn with the eyes this frame), the draws after the scene that go into the window image
// but see nothing stereo are the HUD (health, timer, names, prompts) and screen-wide overlays. Drawn into the eye
// images they would sit at infinity, stretched over the whole field of view; instead they go into images of their
// own, the window image's twins: the left eye's pass draws them over black (A), the right eye's over white (B).
// Comparing the two gives each pixel's coverage, whatever blending the game used: alpha = 1 - (B - A), colour A
// (premultiplied). vr_xr.c shows the result on a panel in front of the player.
static TexBackend *hud_img[2], *hud_for;  // A / B, for the window image hud_for
static uint64_t hud_cleared[2];           // present count + 1 at the last clear of A / B
static bool hud_drawn;
static bool frame_scene_seen;

void vr_note_scene_frame(void) { frame_scene_seen = true; }

// The HUD's target: a stereo image the scene camera did not draw into this frame (ARMS: the 1600x900 composite the
// scene is upscaled into, then copied into the window image)
static TexBackend *scene_tgt[32];
static const TexBackend *hud_frame_target;  // this frame's HUD target (its own image)
static int nscene_tgt;
static const TexBackend *orig_of(const NvnTexture *t) {
    const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
    return b && b->vr_is_twin ? b->vr_orig : b;
}
void vr_note_scene_target(NvnTexture *t) {
    const TexBackend *b = orig_of(t);
    for (int i = 0; i < nscene_tgt; i++) if (scene_tgt[i] == b) return;
    if (b && nscene_tgt < 32) scene_tgt[nscene_tgt++] = (TexBackend *)b;
}

bool vr_hud_wanted(NvnTexture *t) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VR_HUD_PANEL") && !atoi(getenv("LARECOMP_VR_HUD_PANEL"));
    if (off || !frame_scene_seen) return false;
    const TexBackend *b = hud_t == t ? hud_saved : orig_of(t);
    if (b && b->vr_is_twin) b = b->vr_orig;
    if (hud_frame_target && hud_frame_target != b) return false;  // (one HUD image a frame: the first)
    for (int i = 0; i < nscene_tgt; i++) if (scene_tgt[i] == b) return false;
    return b != NULL;
}

// t is this eye's HUD target and the HUD has begun there this frame: whatever comes next into it is the HUD's (the
// ARMS icons: little 3D renders sampled from images of their own)
bool vr_hud_active(NvnTexture *t) {
    if (!hud_began[g_vr_eye & 1] || !hud_frame_target) return false;
    const TexBackend *b = hud_t == t ? hud_saved : orig_of(t);
    if (b && b->vr_is_twin) b = b->vr_orig;
    return b == hud_frame_target;
}

// a HUD draw into t: t's image becomes this eye's HUD image (cleared at its first draw of the frame); true when the
// target changed (the pass must begin again)
bool vr_hud_begin(NvnTexture *t) {
    extern TexBackend *vk_tex_clone(NvnTexture *t);
    extern void vk_tex_clone_free(TexBackend *w);
    extern void vk_end_pass(void);
    TexBackend *own = hud_t == t ? hud_saved : (TexBackend *)t->backend;
    TexBackend *orig = own && own->vr_is_twin ? own->vr_orig : own;
    if (!orig) return false;
    bool changed = false;
    if (hud_t && hud_t != t) { hud_t->backend = hud_saved; hud_t = NULL; changed = true; }
    if (hud_for != orig || !hud_img[0] || hud_img[0]->width != orig->width || hud_img[0]->height != orig->height ||
        hud_img[0]->ax != orig->ax || hud_img[0]->ay != orig->ay) {
        if (hud_t) { hud_t->backend = hud_saved; hud_t = NULL; }
        for (int i = 0; i < 2; i++) { vk_tex_clone_free(hud_img[i]); hud_img[i] = NULL; }
        NvnTexture *tt = t;
        void *keep = tt->backend;
        tt->backend = orig;  // (the clones copy the left eye's image)
        hud_img[0] = vk_tex_clone(tt);
        hud_img[1] = vk_tex_clone(tt);
        tt->backend = keep;
        hud_for = orig;
        hud_cleared[0] = hud_cleared[1] = 0;
        static int nlog;
        if (nlog++ < 4) LOG("[vr] HUD images %ux%u: the HUD goes on its own panel\n", orig->width, orig->height);
    }
    TexBackend *h = hud_img[g_vr_eye & 1];
    hud_frame_target = orig;
    hud_began[g_vr_eye & 1] = true;
    uint64_t now = vr_frame_id() + 1;
    if (hud_cleared[g_vr_eye & 1] != now) {  // this frame's first HUD draw for this eye: black / white
        hud_cleared[g_vr_eye & 1] = now;
        vk_end_pass();
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkClearColorValue c = {{0, 0, 0, 0}};
        if (g_vr_eye) c.float32[0] = c.float32[1] = c.float32[2] = c.float32[3] = 1;
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        rec_CmdClearColorImage(cb, h->image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rg);
        vk_full_barrier(cb);
        extern void vk_tex_mark_written(TexBackend *b);
        vk_tex_mark_written(h);  // (its first pass keeps that: no clear)
        changed = true;
    }
    if (hud_t != t) {
        hud_t = t;
        hud_saved = own;
        changed = true;
    }
    if (t->backend != h) { t->backend = h; changed = true; }
    h->sx = orig->sx, h->sy = orig->sy;  // (dynamic resolution: drawn at the window image's scale)
    hud_drawn = true;
    return changed;
}

// any other draw (or the end of an eye's pass): the target's own image again; true when it changed
bool vr_hud_end(void) {
    if (!hud_t) return false;
    hud_t->backend = hud_saved;
    hud_t = NULL;
    return true;
}

// (present) this frame's HUD images, if the HUD was drawn apart; the next frame starts over
bool vr_hud_on_window(void) { return hud_for && hud_for->presented; }

bool vr_hud_take(TexBackend **a, TexBackend **b) {
    bool d = hud_drawn && hud_img[0] && hud_img[1];
    hud_drawn = false;
    frame_scene_seen = false;
    *a = d ? hud_img[0] : NULL;
    *b = d ? hud_img[1] : NULL;
    return d;
}

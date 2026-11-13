// VR (runtime/nvn/vulkan/vr.c, vr_xr.c, vr_input.c). See docs/vr-report.md.
//
// Stereo: each command list the game submits is translated twice: for the left eye, then for the right one. The scene
// camera's constant block (cbuf slot 0, sead/agl's view block) is copied per eye with the eye's view; textures the
// scene camera draws into become stereo (vr_mark: a twin image is made, the right eye's), and while the right eye's
// translation runs every texture object of a stereo image points at its twin (vr_swap). Draws that touch nothing
// stereo (shadow maps) and side effects (counters, events, CPU copies) run in the left eye's pass only.
//
// With a headset (OpenXR, vr_xr.c) the eyes are the headset's: its poses (relative to the game camera, which becomes
// the place of the user's head at the recenter) and fields of view; the frame goes to the headset with them. Menus
// and other frames without the scene camera are shown on a big screen in front of the user; in fights the HUD is
// drawn apart and shown on a panel (vr.c vr_hud_*).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "../nvn.h"

struct TexBackend;

bool vr_on(void);           // LARECOMP_VR=1 (or the setting): stereo rendering
extern int g_vr_eye;        // the eye being translated: 0 left (and mono), 1 right
void vr_frame_begin(void);  // (backend present) per-frame state

// textures
bool vr_tex_stereo(const NvnTexture *t);  // t's image has a twin (or is one)
void vr_mark(NvnTexture *t);              // t becomes stereo (its twin is made at once)
void vr_register(struct TexBackend *b);   // (textures.c) b got a twin
void vr_unregister(struct TexBackend *b); // (textures.c) b is destroyed
void vr_swap(int eye);                    // every stereo texture object -> that eye's image

// the scene camera's block: true when `u` (size bytes) is one; vr_patch_camera rewrites a copy for `eye`
bool vr_is_scene_camera(const void *u, uint64_t size);
void vr_cam_record(const float *orig, const float *patched, size_t bytes, int eye);  // (exec.c) this eye's camera
bool vr_effect_block_has(const float *u, uint64_t bytes);  // another block holds copies of its matrices (effects)
void vr_patch_effect(float *u, uint64_t bytes, int eye);   // those copies -> the eye's
void vr_patch_camera(float *u, uint64_t size, int eye);

// settings (LARECOMP_VR_* and the VR tab)
typedef struct {
    float world_scale;            // game units per meter
    bool level;                   // the game camera's pitch and roll removed (the horizon stays level)
    float res;                    // eye images: x the headset's recommended size
    float panel_dist, panel_width;  // menus' screen (m)
    float hud_dist, hud_width;      // fights' HUD panel (m)
    float hz;                       // refresh rate to ask for (0: the headset's)
    float blink;                    // seconds of black when switching big screen <-> arena
    float vignette;                 // comfort vignette strength (0 off .. 1)
} VrSettings;
extern VrSettings g_vrs;

// OpenXR (vr_xr.c)
int vr_xr_instance_exts(const char **have, int nhave, const char **out, int cap);  // before vkCreateInstance
bool vr_xr_physical_device(VkInstance inst, VkPhysicalDevice *pd);                  // the headset's GPU
int vr_xr_device_exts(const char **have, int nhave, const char **out, int cap);    // before vkCreateDevice
bool vr_xr_on(void);       // a headset was found
bool vr_xr_running(void);  // its session runs
void vr_xr_frame_next(void);                         // (render thread, after a present) the next frame's eyes
void vr_xr_present(NvnTexture *t, const float crop[4]);  // (render thread, at present) the frame to the headset
void vr_xr_quit(void);
bool vr_eye_view(int eye, float q[4], float p[3], float tan[4]);  // the eye for this frame (false: none)
void vr_note_scene(void);  // (vr.c) this frame drew the scene with the eyes' views

// which frames are immersive (vr.c): fights; the rest on the big screen
void vr_note_camera(const float *u, uint64_t size);  // (exec.c, left eye) a scene camera block this frame
bool vr_immersive(void);                             // the frames are the fight's (the player in the arena)
bool vr_stereo_frame(void);                          // this frame: both eyes, the headset's views
void vr_frame_end(void);
float vr_vignette(void);       // the comfort vignette now (0..1, before the setting's scale)
uint64_t vr_frame_id(void);  // the frame being translated (frames presented since the start)                             // (present) the frame's kind decided
void vr_note_scene_target(NvnTexture *t);            // (exec.c) the scene camera drew into t this frame

// the HUD drawn apart (vr.c)
void vr_note_scene_frame(void);              // the scene camera was drawn with the eyes this frame
bool vr_hud_wanted(NvnTexture *t);
bool vr_hud_active(NvnTexture *t);           // the HUD has begun in t this frame (this eye): what follows there is the HUD's           // (exec.c draw) a mono draw into t (stereo) is the HUD's
bool vr_hud_begin(NvnTexture *t);            // its target becomes this eye's HUD image; true: the pass must begin again
bool vr_hud_end(void);                       // the target's own image again; true: the pass must begin again
bool vr_hud_on_window(void);  // the HUD went into the window image (else an image of its own size: shown whole)
bool vr_hud_take(struct TexBackend **a, struct TexBackend **b);  // (present) the HUD over black / white, if drawn

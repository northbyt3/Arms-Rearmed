// VR stereo rendering (runtime/nvn/vulkan/vr.c). See docs/vr-report.md.
//
// Each command list the game submits is translated twice: for the left eye, then for the right one. The scene camera's
// constant block (cbuf slot 0, sead/agl's view block) is copied per eye with the eye's view; textures the scene camera
// draws into become stereo (vr_mark: a twin image is made, the right eye's), and while the right eye's translation runs
// every texture object of a stereo image points at its twin (vr_swap). Draws that touch nothing stereo (shadow maps)
// and side effects (counters, events, CPU copies) run in the left eye's pass only.
#pragma once
#include <stdbool.h>
#include <stdint.h>

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
void vr_patch_camera(float *u, uint64_t size, int eye);

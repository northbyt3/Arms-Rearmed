// VR: punches and guards from the Touch controllers' motion (fights only). See vr_input.c, docs/vr-report.md.
//
// ARMS stays in its button mode; real punches become its punch buttons:
//   punch    a hand moving forward (the way the head faces, level) faster than LARECOMP_VR_PUNCH_SPEED m/s (1.6)
//            presses that side's punch (left B, right A) for 0.12 s; the hand re-arms when it slows down / comes back.
//            Both hands within 0.15 s of each other: the game sees A + B, a grab.
//   guard    both fists up in front of the face, close together: the guard (left stick press) while held.
// Pure functions of the poses (LOCAL space, meters), so tests/vr_gesture_test.c can feed made-up motions.
#include "vr_gesture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// the head's forward, level (x / z), from its orientation (x, y, z, w): R * (0, 0, -1)
static void level_forward(const float q[4], float f[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    f[0] = -(2 * (x * z + y * w));
    f[1] = 0;
    f[2] = -(1 - 2 * (x * x + y * y));
    float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (l < 1e-4f) f[0] = 0, f[2] = -1;
    else f[0] /= l, f[2] /= l;
}

void vr_gesture_init(VrGesture *g) {
    memset(g, 0, sizeof *g);
    g->armed[0] = g->armed[1] = true;
    const char *e = getenv("LARECOMP_VR_PUNCH_SPEED");
    g->punch_speed = e && *e ? (float)atof(e) : 1.6f;
}

// One frame (dt seconds): hand positions / linear velocities (ok: tracked), the head's position / orientation.
// Returns the game buttons the motion presses (VRG_* bits).
unsigned vr_gesture_step(VrGesture *g, float dt, const float hand_p[2][3], const float hand_v[2][3], const bool ok[2],
                         const float head_p[3], const float head_q[4]) {
    float f[3];
    level_forward(head_q, f);
    unsigned out = 0;
    for (int h = 0; h < 2; h++) {
        if (g->hold[h] > 0) g->hold[h] -= dt;
        if (!ok[h]) { g->armed[h] = true; continue; }
        float s = dot3(hand_v[h], f);                                    // forward speed
        float rel[3] = {hand_p[h][0] - head_p[0], hand_p[h][1] - head_p[1], hand_p[h][2] - head_p[2]};
        float reach = dot3(rel, f);                                      // how far in front of the head
        if (g->armed[h] && s > g->punch_speed && reach > -0.1f) {
            g->hold[h] = 0.12f;
            g->armed[h] = false;
            g->punches[h]++;
        } else if (!g->armed[h] && (s < 0.4f * g->punch_speed || reach < 0.15f)) {
            g->armed[h] = true;  // (slowed down or pulled back: the next punch can come)
        }
        if (g->hold[h] > 0) out |= h ? VRG_PUNCH_RIGHT : VRG_PUNCH_LEFT;
    }
    // guard: both hands up in front of the face, near each other
    bool guard = false;
    if (ok[0] && ok[1]) {
        float d[3] = {hand_p[0][0] - hand_p[1][0], hand_p[0][1] - hand_p[1][1], hand_p[0][2] - hand_p[1][2]};
        bool close = sqrtf(dot3(d, d)) < 0.30f;
        bool up = true;
        for (int h = 0; h < 2 && up; h++) {
            float rel[3] = {hand_p[h][0] - head_p[0], hand_p[h][1] - head_p[1], hand_p[h][2] - head_p[2]};
            float reach = dot3(rel, f), height = rel[1];
            up = reach > 0.05f && reach < 0.45f && height > -0.30f && height < 0.15f;
        }
        guard = close && up && g->hold[0] <= 0 && g->hold[1] <= 0;
    }
    g->guard_time = guard ? g->guard_time + dt : 0;
    if (g->guard_time > 0.08f) out |= VRG_GUARD;  // (a moment held: passing through that place mid-punch isn't a guard)
    return out;
}

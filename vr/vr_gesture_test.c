// Tests for runtime/nvn/vulkan/vr_gesture.c with made-up motions (meters, seconds, LOCAL space: -z forward).
//   clang -O1 -Iruntime/nvn/vulkan -o build/vr_gesture_test.exe vr/vr_gesture_test.c runtime/nvn/vulkan/vr_gesture.c && build/vr_gesture_test.exe
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "vr_gesture.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

typedef struct { float p[2][3], v[2][3]; bool ok[2]; float hp[3], hq[4]; } Pose;

static void rest(Pose *s) {
    memset(s, 0, sizeof *s);
    s->ok[0] = s->ok[1] = true;
    s->hp[1] = 1.6f;                       // head 1.6 m up, facing -z
    s->hq[3] = 1;
    for (int h = 0; h < 2; h++) {          // fists at the chest
        s->p[h][0] = h ? 0.2f : -0.2f;
        s->p[h][1] = 1.25f;
        s->p[h][2] = -0.25f;
    }
}
static void yaw(Pose *s, float deg) { float a = deg * 3.14159265f / 360; s->hq[1] = sinf(a), s->hq[3] = cosf(a); }

// a hand from its place along dir (unit) at speed m/s for t seconds, 90 Hz; returns OR of the buttons seen
static unsigned move(VrGesture *g, Pose *s, int h, const float dir[3], float speed, float t, unsigned *last) {
    unsigned all = 0;
    for (float u = 0; u < t; u += 1 / 90.0f) {
        for (int k = 0; k < 3; k++) s->v[h][k] = dir[k] * speed, s->p[h][k] += dir[k] * speed / 90;
        unsigned b = vr_gesture_step(g, 1 / 90.0f, (const float (*)[3])s->p, (const float (*)[3])s->v, s->ok, s->hp, s->hq);
        all |= b;
        if (last) *last = b;
    }
    memset(s->v[h], 0, sizeof s->v[h]);
    return all;
}
static unsigned idle(VrGesture *g, Pose *s, float t) {
    unsigned all = 0;
    for (float u = 0; u < t; u += 1 / 90.0f) all |= vr_gesture_step(g, 1 / 90.0f, (const float (*)[3])s->p, (const float (*)[3])s->v, s->ok, s->hp, s->hq);
    return all;
}

int main(void) {
    static const float fwd[3] = {0, 0, -1}, back[3] = {0, 0, 1}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0};
    VrGesture g;
    Pose s;

    vr_gesture_init(&g); rest(&s);
    CHECK(idle(&g, &s, 1) == 0, "resting fists press nothing");

    vr_gesture_init(&g); rest(&s);
    unsigned b = move(&g, &s, 1, fwd, 3.0f, 0.15f, NULL);
    CHECK(b & VRG_PUNCH_RIGHT, "a 3 m/s right jab punches right");
    CHECK(!(b & VRG_PUNCH_LEFT), "... and not left");
    CHECK(g.punches[1] == 1, "... once (%u)", g.punches[1]);
    unsigned after = idle(&g, &s, 0.3f);
    CHECK(!(after & VRG_PUNCH_RIGHT), "the button is released after the punch");

    move(&g, &s, 1, back, 1.5f, 0.15f, NULL);
    move(&g, &s, 1, fwd, 3.0f, 0.15f, NULL);
    CHECK(g.punches[1] == 2, "pull back + jab again: a second punch (%u)", g.punches[1]);

    vr_gesture_init(&g); rest(&s);
    move(&g, &s, 0, fwd, 1.0f, 0.4f, NULL);
    CHECK(g.punches[0] == 0, "a slow 1 m/s reach is not a punch");

    vr_gesture_init(&g); rest(&s);
    move(&g, &s, 0, right, 3.0f, 0.15f, NULL);
    CHECK(g.punches[0] == 0, "a fast sideways swipe is not a punch");

    vr_gesture_init(&g); rest(&s);
    move(&g, &s, 1, up, 3.0f, 0.15f, NULL);
    CHECK(g.punches[1] == 0, "raising a hand fast is not a punch");

    vr_gesture_init(&g); rest(&s); yaw(&s, 90);  // head turned left: forward is -x
    static const float left[3] = {-1, 0, 0};
    for (int h = 0; h < 2; h++) s.p[h][2] = 0, s.p[h][0] = -0.25f;
    move(&g, &s, 1, left, 3.0f, 0.15f, NULL);
    CHECK(g.punches[1] == 1, "head turned 90 degrees left: a jab that way punches (%u)", g.punches[1]);
    vr_gesture_init(&g); rest(&s); yaw(&s, 90);
    move(&g, &s, 1, fwd, 3.0f, 0.15f, NULL);
    CHECK(g.punches[1] == 0, "head turned: the old forward (now sideways) doesn't (%u)", g.punches[1]);

    vr_gesture_init(&g); rest(&s);
    unsigned both = 0;
    for (float u = 0; u < 0.15f; u += 1 / 90.0f) {  // both at once
        for (int h = 0; h < 2; h++) for (int k = 0; k < 3; k++) s.v[h][k] = fwd[k] * 3, s.p[h][k] += fwd[k] * 3 / 90;
        both |= vr_gesture_step(&g, 1 / 90.0f, (const float (*)[3])s.p, (const float (*)[3])s.v, s.ok, s.hp, s.hq);
    }
    CHECK((both & 3) == 3, "both fists together: both punches (the game's grab)");

    vr_gesture_init(&g); rest(&s);
    for (int h = 0; h < 2; h++) s.p[h][0] = h ? 0.08f : -0.08f, s.p[h][1] = 1.5f, s.p[h][2] = -0.2f;  // fists up at the face
    unsigned gd = idle(&g, &s, 0.3f);
    CHECK(gd & VRG_GUARD, "fists up in front of the face: guard");
    rest(&s);
    unsigned gd2 = 0;
    idle(&g, &s, 0.02f);
    gd2 = idle(&g, &s, 0.2f);
    CHECK(!(gd2 & VRG_GUARD), "fists back down: no guard");

    vr_gesture_init(&g); rest(&s);
    s.ok[1] = false;
    move(&g, &s, 1, fwd, 3.0f, 0.15f, NULL);
    CHECK(g.punches[1] == 0, "an untracked hand never punches");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}

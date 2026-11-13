// VR: punches and guards from the Touch controllers' motion (vr_gesture.c).
#pragma once
#include <stdbool.h>

enum { VRG_PUNCH_LEFT = 1, VRG_PUNCH_RIGHT = 2, VRG_GUARD = 4 };

typedef struct {
    bool armed[2];       // the hand may punch (it came back / slowed down since its last punch)
    float hold[2];       // seconds the punch button stays pressed
    float guard_time;    // seconds the guard pose has been held
    float punch_speed;   // m/s forward to punch
    unsigned punches[2]; // (counts, for tests and the log)
} VrGesture;

void vr_gesture_init(VrGesture *g);
unsigned vr_gesture_step(VrGesture *g, float dt, const float hand_p[2][3], const float hand_v[2][3], const bool ok[2],
                         const float head_p[3], const float head_q[4]);

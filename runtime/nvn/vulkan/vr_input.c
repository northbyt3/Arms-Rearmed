// VR: the Touch controllers (OpenXR actions) as the game's controller. See vr.h, docs/vr-report.md.
//
// ARMS's button controls (its tutorial's Pro Controller prompts): A / B punch right / left, A + B grab, X jump, Y dash,
// left stick press guard, L / R rush, the left stick moves and curves punches. On the Touch controllers:
//   right trigger  A (right punch)        left trigger   B (left punch)     (both: grab)
//   right grip     X (jump)               left grip      Y (dash)           (as the Joy-Cons' R / L in motion play)
//   A / B          A / B (menus)          X / Y          X / Y
//   left stick     move (and the d-pad in menus: the game reads the stick there too); its press: guard
//   right stick    right stick; its press: R (rush)      menu button    + (pause)
// The game thread reads it through g_vr_buttons / g_vr_stick (runtime/hle/input.c fill_state).
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../../rt.h"
#include "vr_gesture.h"

_Atomic uint64_t g_vr_buttons;   // Switch button bits (input.c's BTN_*)
_Atomic int32_t g_vr_stick[4];   // left x, y, right x, y (-32767..32767, y up)
_Atomic int g_vr_input_live;     // the controllers are being read

enum { BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
       BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN };

static PFN_xrStringToPath pStringToPath;
static PFN_xrCreateActionSet pCreateActionSet;
static PFN_xrCreateAction pCreateAction;
static PFN_xrSuggestInteractionProfileBindings pSuggest;
static PFN_xrAttachSessionActionSets pAttach;
static PFN_xrSyncActions pSync;
static PFN_xrGetActionStateBoolean pGetBool;
static PFN_xrGetActionStateFloat pGetFloat;
static PFN_xrGetActionStateVector2f pGetVec2;
static PFN_xrCreateActionSpace pCreateActionSpace;
static PFN_xrLocateSpace pLocateSpace;
static PFN_xrApplyHapticFeedback pHaptic;

static XrInstance inst;
static XrActionSet set;
enum { A_TRIG_L, A_TRIG_R, A_GRIP_L, A_GRIP_R, A_STICK_L, A_STICK_R, A_CLICK_L, A_CLICK_R, A_A, A_B, A_X, A_Y, A_MENU,
       A_POSE_L, A_POSE_R, A_HAPTIC_L, A_HAPTIC_R, A_N };
static XrAction act[A_N];
static XrPath hand[2];
static XrSpace pose_space[2];
static bool attached;

static XrPath path(const char *s) {
    XrPath p = XR_NULL_PATH;
    pStringToPath(inst, s, &p);
    return p;
}

static void make(int i, XrActionType type, const char *name, const char *loc, bool both_hands) {
    XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
    snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", loc);
    if (both_hands) ci.countSubactionPaths = 2, ci.subactionPaths = hand;
    if (pCreateAction(set, &ci, &act[i]) != XR_SUCCESS) LOG("[vr] action %s failed\n", name);
}

void vr_input_attach(XrInstance xi, XrSession xs, PFN_xrGetInstanceProcAddr gipa) {
    inst = xi;
#define G(f, v) gipa(xi, #f, (PFN_xrVoidFunction *)&v)
    G(xrStringToPath, pStringToPath); G(xrCreateActionSet, pCreateActionSet); G(xrCreateAction, pCreateAction);
    G(xrSuggestInteractionProfileBindings, pSuggest); G(xrAttachSessionActionSets, pAttach); G(xrSyncActions, pSync);
    G(xrGetActionStateBoolean, pGetBool); G(xrGetActionStateFloat, pGetFloat); G(xrGetActionStateVector2f, pGetVec2);
    G(xrCreateActionSpace, pCreateActionSpace); G(xrLocateSpace, pLocateSpace); G(xrApplyHapticFeedback, pHaptic);
    hand[0] = path("/user/hand/left");
    hand[1] = path("/user/hand/right");
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "arms");
    strcpy(si.localizedActionSetName, "ARMS");
    if (pCreateActionSet(xi, &si, &set) != XR_SUCCESS) { LOG("[vr] action set failed\n"); return; }
    make(A_TRIG_L, XR_ACTION_TYPE_FLOAT_INPUT, "punch_left", "Punch left", false);
    make(A_TRIG_R, XR_ACTION_TYPE_FLOAT_INPUT, "punch_right", "Punch right", false);
    make(A_GRIP_L, XR_ACTION_TYPE_FLOAT_INPUT, "dash", "Dash", false);
    make(A_GRIP_R, XR_ACTION_TYPE_FLOAT_INPUT, "jump", "Jump", false);
    make(A_STICK_L, XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move", false);
    make(A_STICK_R, XR_ACTION_TYPE_VECTOR2F_INPUT, "right_stick", "Right stick", false);
    make(A_CLICK_L, XR_ACTION_TYPE_BOOLEAN_INPUT, "guard", "Guard", false);
    make(A_CLICK_R, XR_ACTION_TYPE_BOOLEAN_INPUT, "rush", "Rush", false);
    make(A_A, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A", false);
    make(A_B, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B", false);
    make(A_X, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X", false);
    make(A_Y, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y", false);
    make(A_MENU, XR_ACTION_TYPE_BOOLEAN_INPUT, "pause", "Pause", false);
    make(A_POSE_L, XR_ACTION_TYPE_POSE_INPUT, "hand_left", "Left hand", false);
    make(A_POSE_R, XR_ACTION_TYPE_POSE_INPUT, "hand_right", "Right hand", false);
    make(A_HAPTIC_L, XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble_left", "Rumble left", false);
    make(A_HAPTIC_R, XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble_right", "Rumble right", false);
    struct { int a; const char *p; } touch[] = {
        {A_TRIG_L, "/user/hand/left/input/trigger/value"}, {A_TRIG_R, "/user/hand/right/input/trigger/value"},
        {A_GRIP_L, "/user/hand/left/input/squeeze/value"}, {A_GRIP_R, "/user/hand/right/input/squeeze/value"},
        {A_STICK_L, "/user/hand/left/input/thumbstick"}, {A_STICK_R, "/user/hand/right/input/thumbstick"},
        {A_CLICK_L, "/user/hand/left/input/thumbstick/click"}, {A_CLICK_R, "/user/hand/right/input/thumbstick/click"},
        {A_A, "/user/hand/right/input/a/click"}, {A_B, "/user/hand/right/input/b/click"},
        {A_X, "/user/hand/left/input/x/click"}, {A_Y, "/user/hand/left/input/y/click"},
        {A_MENU, "/user/hand/left/input/menu/click"},
        {A_POSE_L, "/user/hand/left/input/grip/pose"}, {A_POSE_R, "/user/hand/right/input/grip/pose"},
        {A_HAPTIC_L, "/user/hand/left/output/haptic"}, {A_HAPTIC_R, "/user/hand/right/output/haptic"}};
    XrActionSuggestedBinding sb[32];
    int n = 0;
    for (size_t i = 0; i < sizeof touch / sizeof *touch; i++) sb[n++] = (XrActionSuggestedBinding){act[touch[i].a], path(touch[i].p)};
    XrInteractionProfileSuggestedBinding pb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    pb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    pb.suggestedBindings = sb;
    pb.countSuggestedBindings = (uint32_t)n;
    if (pSuggest(xi, &pb) != XR_SUCCESS) LOG("[vr] Touch controller bindings refused\n");
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo ai = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ai.action = act[h ? A_POSE_R : A_POSE_L];
        ai.poseInActionSpace.orientation.w = 1;
        pCreateActionSpace(xs, &ai, &pose_space[h]);
    }
    XrSessionActionSetsAttachInfo at = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &set;
    attached = pAttach(xs, &at) == XR_SUCCESS;
    LOG("[vr] Touch controllers: %s\n", attached ? "bound (triggers punch, grips dash / jump, left stick press guards)" : "attach failed");
}

void vr_input_profile_changed(void) { LOG("[vr] controllers changed\n"); }

static float get_f(XrSession xs, int a) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, NULL, act[a]};
    XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
    return pGetFloat(xs, &gi, &s) == XR_SUCCESS && s.isActive ? s.currentState : 0;
}
static bool get_b(XrSession xs, int a) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, NULL, act[a]};
    XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
    return pGetBool(xs, &gi, &s) == XR_SUCCESS && s.isActive && s.currentState;
}
static XrVector2f get_v(XrSession xs, int a) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO, NULL, act[a]};
    XrActionStateVector2f s = {XR_TYPE_ACTION_STATE_VECTOR2F};
    if (pGetVec2(xs, &gi, &s) == XR_SUCCESS && s.isActive) return s.currentState;
    return (XrVector2f){0, 0};
}
static int32_t axis(float v) {
    if (fabsf(v) < 0.12f) return 0;  // (dead zone)
    int32_t i = (int32_t)(v * 32767);
    return i > 32767 ? 32767 : i < -32767 ? -32767 : i;
}

// Hand poses (LOCAL space) for motion play (vr_motion.c): the latest located
XrPosef g_vr_hand_pose[2];
XrVector3f g_vr_hand_vel[2], g_vr_hand_angvel[2];
_Atomic int g_vr_hand_ok[2];

// (render thread, each frame after xrWaitFrame)
void vr_input_sync(XrSession xs, XrSpace local, XrTime t, const XrPosef *head) {
    if (!attached) return;
    XrActiveActionSet as = {set, XR_NULL_PATH};
    XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (pSync(xs, &si) != XR_SUCCESS) return;  // (XR_SESSION_NOT_FOCUSED: the Oculus menu is up)
    // triggers / grips as buttons, with a little hysteresis
    static bool held[4];
    const int fa[4] = {A_TRIG_L, A_TRIG_R, A_GRIP_L, A_GRIP_R};
    for (int i = 0; i < 4; i++) {
        float v = get_f(xs, fa[i]);
        held[i] = held[i] ? v > 0.35f : v > 0.55f;
    }
    uint64_t b = 0;
    if (held[0]) b |= 1ull << BTN_B;
    if (held[1]) b |= 1ull << BTN_A;
    if (held[2]) b |= 1ull << BTN_Y;
    if (held[3]) b |= 1ull << BTN_X;
    if (get_b(xs, A_A)) b |= 1ull << BTN_A;
    if (get_b(xs, A_B)) b |= 1ull << BTN_B;
    if (get_b(xs, A_X)) b |= 1ull << BTN_X;
    if (get_b(xs, A_Y)) b |= 1ull << BTN_Y;
    if (get_b(xs, A_CLICK_L)) b |= 1ull << BTN_LSTICK;
    if (get_b(xs, A_CLICK_R)) b |= 1ull << BTN_R;
    if (get_b(xs, A_MENU)) b |= 1ull << BTN_PLUS;
    XrVector2f l = get_v(xs, A_STICK_L), r = get_v(xs, A_STICK_R);
    g_vr_stick[0] = axis(l.x), g_vr_stick[1] = axis(l.y), g_vr_stick[2] = axis(r.x), g_vr_stick[3] = axis(r.y);
    g_vr_input_live = 1;
    bool ok[2];
    float hp[2][3], hv[2][3];
    for (int h = 0; h < 2; h++) {
        XrSpaceVelocity v = {XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION, &v};
        ok[h] = pLocateSpace(pose_space[h], local, t, &loc) == XR_SUCCESS &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (v.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT);
        if (ok[h]) {
            g_vr_hand_pose[h] = loc.pose;
            g_vr_hand_vel[h] = v.linearVelocity;
            g_vr_hand_angvel[h] = v.angularVelocity;
        }
        g_vr_hand_ok[h] = ok[h];
        hp[h][0] = loc.pose.position.x, hp[h][1] = loc.pose.position.y, hp[h][2] = loc.pose.position.z;
        hv[h][0] = v.linearVelocity.x, hv[h][1] = v.linearVelocity.y, hv[h][2] = v.linearVelocity.z;
    }
    {   // punches and guards from the controllers' motion, in fights (vr_gesture.c). LARECOMP_VR_MOTION=0: off
        static int on = -1;
        static VrGesture g;
        static XrTime last;
        if (on < 0) {
            const char *e = getenv("LARECOMP_VR_MOTION");
            on = !e || atoi(e) != 0;
            vr_gesture_init(&g);
            if (on) LOG("[vr] motion punches on: punch forward faster than %.1f m/s, fists up at the face to guard\n", g.punch_speed);
        }
        extern bool vr_immersive(void);
        float dt = last && t > last ? (float)((t - last) / 1e9) : 1 / 60.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = t;
        if (on && head && vr_immersive()) {
            float h_p[3] = {head->position.x, head->position.y, head->position.z};
            float h_q[4] = {head->orientation.x, head->orientation.y, head->orientation.z, head->orientation.w};
            unsigned m = vr_gesture_step(&g, dt, (const float (*)[3])hp, (const float (*)[3])hv, ok, h_p, h_q);
            if (m & VRG_PUNCH_LEFT) b |= 1ull << BTN_B;
            if (m & VRG_PUNCH_RIGHT) b |= 1ull << BTN_A;
            if (m & VRG_GUARD) b |= 1ull << BTN_LSTICK;
            static unsigned prev[2], nlog;
            for (int h = 0; h < 2; h++) {
                if (g.punches[h] < prev[h]) prev[h] = 0;  // (reset since)
                if (g.punches[h] > prev[h] && nlog++ < 40) LOG("[vr] %s punch\n", h ? "right" : "left");
                prev[h] = g.punches[h];
            }
        } else if (on) {
            float sp = g.punch_speed;
            vr_gesture_init(&g);  // (out of fights: nothing half-done carries over)
            g.punch_speed = sp;
        }
    }
    g_vr_buttons = b;
    {   // the game's rumble on the controllers (amplitude of its low / high bands: the stronger one)
        extern float vr_rumble_amp(int side);
        static float last[2];
        for (int h = 0; h < 2; h++) {
            float amp = vr_rumble_amp(h);
            if (amp < 0.02f && last[h] < 0.02f) continue;
            last[h] = amp;
            XrHapticVibration hv = {XR_TYPE_HAPTIC_VIBRATION};
            hv.amplitude = amp > 1 ? 1 : amp;
            hv.duration = 40000000;  // 40 ms: renewed every frame while the game rumbles
            hv.frequency = XR_FREQUENCY_UNSPECIFIED;
            XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO, NULL, act[h ? A_HAPTIC_R : A_HAPTIC_L]};
            pHaptic(xs, &hi, (const XrHapticBaseHeader *)&hv);
        }
    }
}

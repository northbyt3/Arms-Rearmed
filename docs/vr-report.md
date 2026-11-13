# ARMS Rearmed VR — progress report

Branch `vr` (local only, not pushed), worktree `Arms-Rearmed-vr` next to the main checkout. The main checkout,
its settings and its save are untouched.

## TL;DR — how to try it (Quest 3 over Link)

1. On the PC: start **Meta Horizon Link** (the Oculus app). On the Quest 3: enable **Link** (cable or Air Link) so
   you are in the Link home.
2. Double-click **`ARMS-VR.bat`** in `Arms-Rearmed-vr`.
   - First run copies your save (`..\Arms-Rearmed\save` → `save-vr`) and makes `larecomp-vr.ini` (2x internal
     resolution). Your normal game's save and settings are never written.
3. Put the headset on. Menus, intros and the VS screen appear on a **big screen** in front of you. When a fight
   starts you are **in the arena**: head-tracked, stereo, standing where the game camera is (behind your fighter),
   with the HUD on its own panel in front of you.
4. Controls on the Touch controllers (ARMS's button scheme):

   | Touch | Game | In a fight |
   |---|---|---|
   | Right trigger | A | punch right |
   | Left trigger | B | punch left |
   | Both triggers | A + B | grab |
   | Right grip | X | jump |
   | Left grip | Y | dash |
   | Left stick (press) | L-stick | move (guard) |
   | Right stick (press) | R-stick (R) | (rush) |
   | A / B | A / B | menus |
   | X / Y (left) | L / R | rush; **X + Y together = "L + R"** (e.g. confirming your ARMS) |
   | ☰ (left menu button) | + | pause |

   **Motion punches (fights only)**: throw a real punch with a controller (forward, faster than ~1.6 m/s) and that
   arm punches; both at once = grab; hold both fists up in front of your face = guard. The triggers still work at
   the same time. Tune with `LARECOMP_VR_PUNCH_SPEED` or turn off with `LARECOMP_VR_MOTION=0` (in `ARMS-VR.bat`).

   A keyboard / gamepad still works too. The game's rumble goes to the Touch controllers.

**Recenter**: automatic — each time a fight starts (and each time you go back to the big screen), wherever your head
is and faces becomes "forward": the fight starts in front of you, screens appear where you look. Holding the
Oculus button (the standard Quest recenter) works too. `LARECOMP_VR_AUTO_RECENTER=0` turns the automatic one off.

If something looks wrong, the options at the bottom of `ARMS-VR.bat` (world scale, big-screen size, HUD panel,
refresh rate) are the first knobs. `LARECOMP_VR_MODE=screen` shows everything on the big screen (a 3D-less
fallback that still uses the headset).

## Status (what works, verified without the headset)

I could not wear the headset, so everything below was verified with:
- **Offline renders**: a made-up head pose (`LARECOMP_VR_FAKE_HEAD=yaw,pitch,fov`) runs the full VR render path
  and saves exactly what would go to the headset (`LARECOMP_VR_SHOT_AT=frame` → `build/frames/vr_N_{L,R,panel}.png`).
- **Meta XR Simulator** (installed through the Meta `metavr` tool into `%APPDATA%\metavr\tools\xrsim`, selected
  per process with `XR_RUNTIME_JSON`, so your system OpenXR runtime stayed Link): the real OpenXR path ran through
  menus → big screen → fight at a steady 60 fps, session states READY → SYNCHRONIZED → VISIBLE → FOCUSED, no
  OpenXR errors, Touch bindings accepted, refresh rates 72/80/90/120 offered.

| Feature | State |
|---|---|
| Stereo rendering (every scene pass rendered per eye) | ✅ verified: correct parallax (near fighter ~30 px, crowd ~3 px at 1x) |
| Head tracking (the headset's pose and per-eye field of view drive the camera) | ✅ math verified with fake poses; real headset untested |
| Levelled horizon (game camera pitch/roll removed; your head does that) | ✅ |
| Wide FOV (~100°) without culling holes | ✅ looking 70° sideways/down showed a complete arena |
| Fights immersive, everything else on a big screen | ✅ automatic (see below) |
| HUD on its own panel in fights (not smeared across your view) | ✅ with correct transparency |
| Touch controllers as the pad, rumble on the controllers | ✅ bindings accepted by the simulator; feel untested |
| 60 fps at 2x internal res, both eyes | ✅ GPU 3.3 ms/frame (GTX 1080 Ti) |
| Motion punches / guard (controller motion → the game's punch / guard buttons) | ✅ unit-tested (`vr/vr_gesture_test.c`, 15 cases), no false punches with idle controllers in the simulator; feel untested |
| Stays in the arena through a real fight (scripted 40 s fight: punches, grabs, jumps, dashes, a KO) | ✅ |
| Stability: 7-minute soak (two matches, several rounds, a stage change, scripted play) | ✅ no crash, stereo images bounded (~40-90, freed on stage changes), 60 fps |
| Comfort: vignette while the camera turns/moves fast, height stabilisation, blink on mode switches | ✅ |
| True Joy-Con motion mode (IMU emulation: curving by tilting, etc.) | ❌ not done, see below |

## How it works (short)

- **Two eyes from one game**: each command list the game submits is translated twice by the Vulkan backend, once
  per eye. Textures the scene camera draws into get a "twin" image for the right eye; during the right eye's pass
  every texture object of such an image points at its twin. Shadow maps and CPU-side effects run once.
- **The camera**: ARMS's scene camera constant block (view, view-projection, projection, inverse view, previous
  frame's set) is recognised in the constant buffers and rewritten per eye: view = eye pose⁻¹ × levelled game
  view (eye pose in meters × world scale, 10 units/m), projection = the eye's asymmetric frustum (the game's depth
  rows kept).
- **Which frames are immersive**: the fight camera has a signature (one scene camera, 55° FOV at rest). 8 frames of
  it turn the arena on; during the fight the camera zooms (42–57° measured), so one camera with any such FOV keeps
  it on, and 20 frames without (two cameras, none) turn it off. The ARMS LIVE intro (50°), character select, VS screen (two
  cameras) etc. go to the big screen, rendered once (no second eye pass: cheaper).
- **HUD**: in fights, draws after the scene into the HUD layer are redirected: the left pass draws them over black,
  the right pass over white; alpha = 1 − (white − black) recovers exact coverage for any blend mode; shown on an
  OpenXR quad layer at 1.6 m (1.8 m wide).
- **Comfort**: the fight camera yaws > 30°/s in 21% of frames (up to ~110°/s) and dashes move it ~6 m/s. While it
  turns or moves fast, the view's edges darken (vignette, `LARECOMP_VR_VIGNETTE`); its height is smoothed over
  ~1.5 s so jumps don't lift your head (you look up at your fighter instead); switching between the big screen and
  the arena blinks to black for 0.2 s instead of cutting.
- **OpenXR**: loaded at run time (`libopenxr_loader.dll`), Vulkan via `XR_KHR_vulkan_enable`; `xrWaitFrame` +
  pose after each present, frame end on the recorder thread (it owns the Vulkan queue) with the poses the frame was
  rendered with, so the runtime's reprojection (ASW/timewarp) is correct.

## Problems found and how they were solved

1. **Both eyes identical** — the backend memoizes texture views per texture object per frame; swapping a texture
   object's image for the right eye hit the memo. Fix: the swap bumps the view generation.
2. **Menus/intros/VS screen got head tracking** (split-screen VS views stretched, cinematic cameras moving you) —
   fixed by classifying frames by the camera signature; those go to the big screen.
3. **HUD stretched over the whole view at infinity** — HUD redirected into its own images and panel (above).
   Sub-problems: ARMS draws the HUD into a 1600×900 composite that is then copied to the window (so "the window
   image" was the wrong target); the HUD's 3D ARMS icons are little renders with their own camera (handled: once the
   HUD has begun, everything into that target is HUD); the backend's "first write clears the target" logic wiped
   the white background (marked as written); target swaps confused the stereo check (fixed).
4. **Crash in `vr_swap`** — a HUD image had been made stereo and later freed. HUD images are never made stereo now,
   and freeing one cleans up defensively.
5. **18 fps in the arena** — profiling showed descriptor-set building at ~1.9 s per 2 s. Cause: the camera block was
   patched in place inside the transient upload ring, which is write-combined (ReBAR) memory where CPU reads are
   very slow. Patching on the stack and copying once (plus a per-camera memo) → solid 60 fps.
6. **Flickering between arena and big screen mid-fight** — the fight camera zooms its FOV; fixed with hysteresis
   (enter on the resting 55°, stay while one camera remains).
7. **Test noise** — other agents' builds/games and a leftover XR Simulator process (50% GPU) ran on the same PC at
   times, causing unrelated slowdowns in some measurements. Simulator processes are stopped after each test.

## Known issues / things to check in the headset

- **World scale**: estimated (~10 game units per meter from the fighter's size and the measured parallax). If
  fighters feel like giants or toys, change `LARECOMP_VR_SCALE` (smaller value = bigger world).
- **Camera motion**: the game's chase camera still yaws to keep the opponent in view; that's artificial rotation.
  Levelled horizon, the vignette and height smoothing help; if it still makes you queasy, raise
  `LARECOMP_VR_VIGNETTE` to 1, or tell me and I'll add yaw smoothing / snap turning.
- **Your in-game F1 menu (ImGui) is not shown in the headset** — only on the desktop window. The game's own menus are.
- **Audio**: plays on Windows' default device; turn on "Oculus/Meta audio" in the Link settings to hear it in the
  headset.
- **Resolution**: the scene is a 16:9 render squeezed into a nearly square eye FOV, so horizontal detail is higher
  than vertical. 2x internal resolution is the default; 3x is affordable on the 1080 Ti if it looks soft
  (`resolution = 3x` in `larecomp-vr.ini`).
- **Frame pacing**: the game runs at 60 fps; Link at 72/90 Hz uses reprojection between frames. `LARECOMP_VR_HZ=120`
  makes the timing even (60 × 2) if your Link settings allow 120 Hz.
- The desktop window shows what the headset's left eye sees in fights (HUD panel included); menus as the big screen shows them.
- The HUD panel covers exactly the game camera's view (its zoom, pitch and height), so damage numbers and markers line up with the fighters.

## Why punches are gestures and not Joy-Con emulation

ARMS's own motion mode needs dual Joy-Cons with IMU data (`GetSixAxisSensorStates`: acceleration, angular
velocity, orientation in Joy-Con axes held in the thumbs-up grip). Emulating that from Touch poses is possible
(the game imports the six-axis functions; they can be replaced like `GetNpadStates`), but getting the axes and
units right needs trial with the controllers in hand, and a wrong guess makes the game unplayable. Gestures that
press the game's own buttons are robust and testable offline, so the first version uses them; curving is the
left stick, as in the button scheme. Joy-Con emulation is a good next experiment once you can try things.

## Building

```
cd Arms-Rearmed-vr
make -j24 PY=.venv/Scripts/python.exe build/armsrecomp.exe     (MSYS2 UCRT64 shell)
```
New dependency: MSYS2 package `mingw-w64-ucrt-x86_64-openxr-sdk` (installed); `libopenxr_loader.dll` copied into
`build/`.

## Test commands (for me / later)

```
# offline (no headset): head 0° yaw, -10° pitch, ±50° FOV; save what goes to the headset at frame 6800
LARECOMP_VR=1 LARECOMP_VR_NO_XR=1 LARECOMP_VR_FAKE_HEAD="0,-10,50" LARECOMP_VR_SHOT_AT=6800 bash build/vrtest/run.sh 130 test
# through the Meta XR Simulator
XR_RUNTIME_JSON="$APPDATA\metavr\tools\xrsim\meta_openxr_simulator.json" LARECOMP_VR=1 bash build/vrtest/run.sh 140 sim
```
Diagnostics: `LARECOMP_VR_CAMLOG=1` (scene cameras per frame), `LARECOMP_VR_DRAWLOG=<frame>` (each draw's VR kind).

## Log

- Stereo core; identical-eyes bug fixed; parallax measured.
- OpenXR integration; simulator end-to-end at 60 fps.
- Frame classification (fight / big screen); HUD panel with exact alpha; Touch input + rumble.
- Performance fix (write-combined memory reads): immersive 18 → 60 fps.
- Launcher `ARMS-VR.bat`, VR settings template.
- Motion punches / guard gestures (unit-tested); simulator re-run: no false punches, 60 fps.
- Fight hysteresis (camera zoom), height stabilisation, blink transitions, comfort vignette (verified in a scripted fight).
- Soft auto-recenter at each arena / big-screen switch; simulator re-run clean.
- 7-minute soak test: no crash, no leak, 60 fps.

## First live session on the headset (Quest 3S over Link, 2026-11-13)

Found and fixed while playing:
- **No L + R on the controllers** ("Choose your ARMS" asks for it): left X / Y are now L / R (press both).
- **Hit sparks, shockwaves, confetti misplaced / squeezed / stuck to the head**: ARMS's effect system keeps its own
  copy of the camera (view-projection in another constant block). Any block holding copies of the scene camera's
  matrices is now made per eye too.
- **HUD drifted from the fighters** (damage numbers, name tags): the HUD panel now covers exactly the game camera's
  view (its zoom, pitch and height), so screen-placed bits line up with the world.
- **Grey box over the arena on menus**: the game's full-screen dimming dropped from the HUD panel (per pixel: dark,
  not fully opaque). A first version that looked at the surroundings left offset bands shaped like each element
  (the "ghost doubles") - replaced.
- **HUD colours**: the panel goes to the headset in straight alpha (the compositor decodes its sRGB before blending;
  premultiplied gamma colour came out dark).
- **Link drops** (the cable was flaky): the game now reconnects by itself; `ARMS-VR.bat` waits a minute for the
  headset at start.
- **Seeing what the headset sees on the PC**: the Oculus Mirror (left eye, exact compositor output) opens with the
  session. The game window shows an approximation of the left eye (it can't include Link's last-moment reprojection).
- Live capture: create `build\vrtest\shot.req` while playing; the next frame's eyes, HUD panel and HUD renders are
  saved to `build\frames\vr_<frame>_*.png`.
- Useful when Link's PC service isn't running ("no headset"): open the Meta Horizon Link app first.

## Known issues (next session)

- On the "Choose your ARMS" screen the player's health bar (bottom right, "Spring Man") shows as a black silhouette
  instead of the translucent bar with its teal fill; in matches it looks right. Likely a blend mode the black/white
  HUD trick doesn't recover, or the shade removal taking its translucent backing. Use `build\vrtest\shot.req` on that
  screen to capture the HUD renders and compare.
- Minor graphics glitches remain here and there; capture with `shot.req` when seen.

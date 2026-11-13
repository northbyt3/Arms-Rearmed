# ARMS Rearmed

<a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

A static recompilation of *ARMS* (Nintendo Switch, version 5.4.0 or 5.5.1: the base game with one of those updates) into a native
Windows x86-64 program, built on [LAS Recomp](https://github.com/peiikai/LASRecomp)'s recompiler and Switch runtime
(by way of MUA3 Recomp and DOAX3 Recomp). The game's ARM64 code is translated to C and compiled for the PC; a Switch
runtime takes the console's place (graphics through Vulkan, audio, input, files, services).

**Status: playable offline.** Every offline mode has been played through at 60 fps with keyboard, mouse and
controller input: Grand Prix, Versus (Fight, Team Fight, V-Ball, Hoops, Skillshot, Hedlok Scramble, 1-on-100,
ARMS Test), Training, and the Badges, Gallery, Replays, Dashboard and Get ARMS screens. Shaders are prepared before
the game appears (a few seconds; about 12 s the first time after an install), so there is no stutter while it
starts and no model shows up late. Online play, local wireless, the web manual and the eShop aren't available: the
game says so and stays in its menus. Details: [docs/CHANGES.md](docs/CHANGES.md).

No game files are included or ever committed, and the releases hold no game code: the game code is recompiled from
your own copy on your own PC. You need your own legally obtained copy of the game, its 5.4.0 or 5.5.1 update and the keys from
your own console.

![ARMS Rearmed's icon (original art, no game assets)](docs/icon/icon.png)

## VR mode (Meta Quest over Link) — this fork

This branch adds a **full VR mode**: fights in the arena in stereo 3D, head-tracked, standing behind your fighter,
with the Touch controllers as the input — including **real punches** (throw a punch and that arm punches). Menus,
intros and replays play on a big virtual screen. It runs on the PC through OpenXR, with the headset connected over
Meta Horizon Link (cable or Air Link). Tested on a Quest 3S; other Link headsets (Quest 2 / 3 / Pro) should work
the same way.

**What you get**
- **Real 3D**: each eye is rendered from its own place; the arena, the fighters and the hit effects all have depth.
  A **3D depth** setting makes it stronger (default 1.5×, adjustable while playing).
- **Three camera modes** (right stick press; smooth glide between them): **regular** (the game's camera, behind your
  fighter), **far** (pulled back and up) and **stadium** (a still seat in the stands, with an arrow over your
  fighter — the most comfortable).
- **Motion punches**: punch forward with a controller to throw that arm, both at once to grab, fists up at your face
  to guard. The triggers punch too.
- **HUD in the world**: the life bars stay by the fighters, damage numbers float where the hits land, the timer and
  portraits sit on a panel in your view.
- **Quest button prompts** in the game (A / B / X / Y / grips instead of the Switch's), the game's rumble on the
  controllers, its sound in the headset.
- **Comfort**: levelled horizon, jumps don't lift your head, a vignette while the camera swings fast, a short blink
  when switching between the screen and the arena, automatic recenter at each fight.

**Controls (Touch)**

| Touch | In a fight | In menus |
|---|---|---|
| Right / left trigger (or a real punch) | punch right / left | A / B |
| Both triggers (or both fists) | grab | |
| Right grip / left grip | jump / dash | |
| Left stick (press: guard, or fists up) | move | move |
| Right stick press | next camera mode | |
| Hold right stick press + push up / down | stronger / weaker 3D | |
| A / B | A / B | A / B |
| X / Y (left) — together = "L + R" | L / R | L / R |
| Menu (left) | pause | + |

**How to play it**
1. Build this branch from source (below), so you have `build\armsrecomp.exe` and your game's files extracted.
2. Start **Meta Horizon Link** on the PC and connect the headset (Link cable or Air Link) so you're in the Link home.
3. Double-click **`ARMS-VR.bat`**. It uses its own settings file (`larecomp-vr.ini`) and its own save folder
   (`save-vr`, copied from your normal save the first time), so your normal game is untouched.
4. The **Oculus Mirror** window opens on the PC and shows exactly what your left eye sees.

Options (world scale, 3D depth, camera mode at start, HUD distance, comfort settings, refresh rate) are at the
bottom of `ARMS-VR.bat`. How it works, what was tried and the known issues: [docs/vr-report.md](docs/vr-report.md).

**Known issues**: on "Choose your ARMS" the player's bar shows dark and a thin line crosses the screen; the HUD in
the far / stadium views is smaller (it covers what the game's camera sees, from farther away).

## Play with RecompLauncher

ARMS Rearmed works with [RecompLauncher](https://recomplauncher.com): install it from the launcher, then give Easy
Setup your ARMS XCI or NSP, the 5.4.0 or 5.5.1 update NSP and your `prod.keys`. The setup step recompiles the game for your PC
(about 10 minutes; nothing else to install), then Play starts it. Video, sound and controller settings are on the
game's page. Keep your XCI / NSP files where they are: the game reads its data from them.

## Play with the release zip

1. Download `ARMS-Rearmed-v<version>-Windows-x64.zip` from [Releases](https://github.com/VirtuaQuest/Arms-Rearmed/releases)
   and unzip it somewhere you can write to (Documents or Desktop, not Program Files).
2. Run `armslauncher.exe`. On **GAME FILES**, choose your base game (XCI or NSP), the 5.4.0 or 5.5.1 update (NSP) and your
   `prod.keys`, then **INSTALL** (about 10 minutes the first time: extract, recompile, build, prepare the shaders).
3. **FIGHT!** on the PLAY page.

Needs Windows 10 or 11 (64-bit), a CPU with AVX2 (Intel 4th gen / AMD Ryzen or newer), a graphics driver with Vulkan
1.3, 8 GB of RAM (16 GB recommended) and about 2 GB free for the setup.

## Build from source (Windows)

1. Install [MSYS2](https://www.msys2.org) to `C:\msys64`, then in its UCRT64 shell:
   `pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld make mingw-w64-ucrt-x86_64-sdl3
   mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-vulkan-devel mingw-w64-ucrt-x86_64-shaderc
   mingw-w64-ucrt-x86_64-vulkan-memory-allocator`
   (as LAS Recomp's `tools/setup_windows.ps1`).
2. Install [Python 3.12](https://www.python.org) (the `py` launcher).
3. Build the launcher once, from Git Bash or the MSYS2 UCRT64 shell in this folder:
   `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"; make launcher dlls` (`dlls` copies the MSYS2 DLLs
   next to it, so it starts from Explorer).
4. Run `build\armslauncher.exe`. On **GAME FILES**, choose your base game (XCI or NSP), the 5.4.0 or 5.5.1 update (NSP) and
   your `prod.keys`, then **INSTALL**: it makes the Python environment, extracts the game's code, recompiles it and
   builds the PC version (about 10 minutes the first time). Then **FIGHT!** on the PLAY page.

Only the game's code (exefs) and control data are copied out; the game data is read in place from your files through
`extracted/romfs.src`, which holds your title key: keep it private.

### By hand

```sh
py -V:3.12 -m venv .venv
.venv/Scripts/python -m pip install unicorn==2.1.4 capstone==5.0.9 lz4==4.4.5 pycryptodome==3.23.0 pillow==13.0.0
export MSYS_NO_PATHCONV=1 PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
.venv/Scripts/python -I tools/nsp_extract.py "C:/path/ARMS.xci" "C:/path/prod.keys" extracted --update "C:/path/update.nsp"
make gen PY=.venv/Scripts/python.exe      # about 30 s
make -j16 PY=.venv/Scripts/python.exe     # about 8 minutes the first time
tools/run.sh                              # or build/armsrecomp.exe extracted (log: arms.log)
```

## Playing

| Action | Keyboard / mouse | Controller (by position, as on the Switch) |
|---|---|---|
| Move, curve a punch | W A S D | Left stick |
| Left / right punch | Left / right mouse (or Backspace / Enter) | Bottom / right face button (B / A) |
| Grab | Middle mouse, or both mouse buttons | Both punches (A + B) |
| Jump / dash | Space / Left Shift | Top / left face button (X / Y) |
| Guard | Left Ctrl | Left stick press |
| Rush | Q or E | L or R |
| Pause (+) / (-) | Tab / M | Start / Back |
| Menu (controls, video, audio) | Esc or F1 | Home / Guide |

Everything can be rebound in game (Esc or F1, then Controls): keys, mouse buttons and controller buttons. Any
controller SDL 3 knows works (Xbox, PlayStation, Switch Pro, and more), with rumble. The game's button icons follow
what you play with: PlayStation or Xbox buttons, or key caps and the mouse for the keyboard, switching as you go
(Controls tab, Button icons). The launcher's GRAPHICS page
(and the in-game Video tab) set the window mode and size, the internal resolution (up to 4x, or dynamic), FSR 1
upscaling, SMAA and the aspect ratio. ARMS runs at its own 60 fps: its fights step once a frame.

## Making a release

From the MSYS2 UCRT64 shell (with `nasm` and `zip` installed as well): `make deps-windows` (an LGPL-only FFmpeg),
`make gen-helper` (the extractor and recompiler frozen with PyInstaller), `tools/fetch_zig.sh` (the pinned compiler),
then `make dist`: `build/dist/ARMS-Rearmed-v<VERSION>-Windows-x64.zip`. It checks that no game code, game files, GPL
codec libraries or paths of the build machine are inside. Attach it to a GitHub release whose tag is `v<VERSION>`;
`recomplauncher.json` inside tells RecompLauncher how to set the game up.

## Credits

- **LAS Recomp** by Peiikai and contributors (MIT License, `LICENSE.LASRecomp`): the recompiler and the runtime.
- Third-party components keep their own licenses (`third_party/`): SDL 3, Dear ImGui, FFmpeg, AMD FSR, SMAA, and the
  Baloo 2 font (SIL Open Font License) the launcher uses. The release's bundled components, their licenses and
  their sources: [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).
- [Switchbrew](https://switchbrew.org) documentation.

*ARMS* is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo. The launcher's look is
original: no game art, fonts or logos.

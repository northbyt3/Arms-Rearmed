@echo off
rem ARMS Rearmed in VR (the "vr" branch). Quest 3 over Link: start Meta Horizon Link on the PC, connect the
rem headset (Link / Air Link), then run this. The desktop window mirrors the left eye; the headset gets the game.
rem
rem Your normal game is untouched: this uses its own settings (larecomp-vr.ini, made from vr\larecomp-vr.template.ini
rem on the first run) and its own save folder (save-vr, a copy of ..\Arms-Rearmed\save made on the first run).
rem Docs: docs\vr-report.md
setlocal
cd /d "%~dp0"
if not exist "build\armsrecomp.exe" (
  echo build\armsrecomp.exe is missing: build the vr branch first ^(see docs\vr-report.md^).
  pause
  exit /b 1
)
if not exist "larecomp-vr.ini" copy /y "vr\larecomp-vr.template.ini" "larecomp-vr.ini" >nul
if not exist "save-vr" (
  if exist "..\Arms-Rearmed\save" (
    xcopy /e /i /q /y "..\Arms-Rearmed\save" "save-vr" >nul
  ) else (
    mkdir "save-vr"
  )
)
set LARECOMP_VR=1
set LARECOMP_DATA_ROOT=%~dp0
set LARECOMP_SETTINGS=%~dp0larecomp-vr.ini
set LARECOMP_SAVE_DIR=%~dp0save-vr

rem ---- VR options (remove "rem" to use one) ----
rem World scale: game units per meter (default 10). Bigger = the world looks smaller.
rem set LARECOMP_VR_SCALE=10
rem Keep the game camera's pitch / roll (default 1 = levelled horizon, recommended)
rem set LARECOMP_VR_LEVEL=1
rem Eye image size x the headset's recommended size (default 1.0)
rem set LARECOMP_VR_RES=1.0
rem Which frames put you in the arena: auto (fights), screen (always the big screen), scene (any 3D camera)
rem set LARECOMP_VR_MODE=auto
rem Big screen: distance and width in meters (menus, intros, the VS screen)
rem set LARECOMP_VR_PANEL_DIST=2.2
rem set LARECOMP_VR_PANEL_WIDTH=3.0
rem HUD panel in fights: distance in meters; width 0 (default) = exactly the game camera's view, so damage numbers and
rem markers line up with the fighters; a width in meters = a fixed, smaller panel (easier to read, no longer lined up)
rem set LARECOMP_VR_HUD_DIST=1.6
rem set LARECOMP_VR_HUD_WIDTH=0
rem set LARECOMP_VR_HUD_PANEL=1
rem Comfort: vignette while the game camera turns / moves fast (0 = off, 1 = strong; default 0.7)
rem set LARECOMP_VR_VIGNETTE=0.7
rem Comfort: the camera's height smoothed so jumps don't lift your head (default 1; 0 = follow the game camera)
rem set LARECOMP_VR_STABLE_HEIGHT=1
rem Recenter automatically when a fight starts / the big screen comes back (default 1)
rem set LARECOMP_VR_AUTO_RECENTER=1
rem Comfort: seconds of black when switching between the big screen and the arena (default 0.2)
rem set LARECOMP_VR_BLINK=0.2
rem Headset refresh rate to ask for (72, 80, 90, 120; default: as set in the Quest's Link settings)
rem set LARECOMP_VR_HZ=120
rem Motion punches in fights (on by default): punch forward with a controller to throw that arm, fists up at the
rem face to guard. 0 turns them off (the triggers still punch). Punch speed needed, m/s (default 1.6).
rem set LARECOMP_VR_MOTION=1
rem set LARECOMP_VR_PUNCH_SPEED=1.6

start "" "build\armsrecomp.exe"
endlocal

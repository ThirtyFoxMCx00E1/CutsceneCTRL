# CutsceneCtrl v1.0.3  (Android / AndroidModLoader, GTA SA mobile **v2.00**)

Pause / resume / skip cutscenes and mission scenes with a big on-screen button, a floating
settings window, a free camera, blur, and gamepad support.
`libCutsceneCtrl32.so` (armeabi-v7a) and `libCutsceneCtrl64.so` (arm64-v8a).

> Version numbering restarts at **1.0.0** for this release: the whole UI was replaced by the real
> Dear ImGui. (The earlier home-made-OpenGL builds were 1.0.x - 1.2.0, see the end.)

## What changed in 1.0.3

* **Freeze frame** (`FreezeFrame=0`, "Freeze frame (hides drift)" in the PAUSE SCREEN section). While paused, the mod keeps drawing a
  **full-resolution, un-blurred copy of the screen taken at the moment of the pause**, opaque, instead of the live scene - the same trick
  as capturing the front raster and redrawing it until unpaused. The blur (if on) is drawn over it. It does not fix the game's
  physics creeping underneath, it hides it. The free camera always shows the live scene. Off by default. If the capture fails the live
  scene simply stays visible (one attempt per pause, logged). Ticking it while already paused captures on the next frame.
  Same RenderWare calls as the blur's capture, at 1:1 size (one extra screen-sized raster, created on first use).
* **Hold for fast forward - EXPERIMENTAL** (`[FastForward]`, off by default). While you hold the gamepad button (`FastForwardButton=R2`)
  or the on-screen **2x** button (next to SET: [2x] [SET] [PAUSE]), the scene runs at `FastForwardSpeed` (1.25-4, default 2).
  How: `CTimer::Update` derives both timesteps from `ms_fTimeScale x frame time` (checked in the disassembly), and everything
  the cutscene uses - the cutscene clock, animations, camera, script timers - runs off those. The mod raises the scale only *around* the
  call to `CTimer::Update` and puts the game's own value back right after (so slow-motion is multiplied, never replaced).
  Limits you should know: **dialogue audio does not speed up**, so by default audio is silenced while held (`FastForwardMuteAudio=1`) and speech is
  behind the picture afterwards; the game clips one timestep at 3.0, so below ~40 fps the world tops out under the requested speed;
  only real cutscene files by default (`FastForwardScriptedScenes=1` to also allow script-driven "semi-cutscenes", which depend on the
  script flow in `main.scm`); never while paused, in the free camera, while the screen is faded, or in gameplay.
* Fix: if the UI was switched off while a finger was still on it (e.g. a fade started), ImGui could keep a "mouse button down" until the next tap.

## What changed in 1.0.2

* **`FontStyle` - the game's own fonts for the overlay text.** `FontStyle=-1` keeps the built-in Arial (default); `0` Gothic,
  `1` Subtitles, `2` Menu, `3` Pricedown (the game's `CFont` style ids). It applies to the PAUSE / RESUME / SET /
  "Cutscene Controller Settings" labels, the PAUSED banner and the free-camera info line; the settings window keeps Arial.
  The text is printed by `CFont` inside the button's own draw list (so a window opened later still paints over it), the button
  is sized from the font's real text width, `LabelOpacity` and the drop shadow apply to it, and CFont's global state is saved and
  restored around every print so the game's HUD never sees a change. If a CFont symbol is missing, or a measurement fails,
  that label falls back to Arial for the frame. `FontYOffset` (-40..40 %) nudges game-font text up / down if it looks off-centre.
* **Vignette during cutscenes** (`Vignette=1`, `VignetteStrength=160` 0-255 = how dark the corners get, `VignetteSize=50` 0-100 % = how far
  the shade reaches in from the edges). Drawn behind the buttons, also while paused, not while the screen is faded, never in gameplay, and
  independent of `ShowInterface` (it adds no touch area). Sliders in the settings window ("VIGNETTE" section).
* **`OutlineOpacity`** (0-255, default 170 = the previous look): the outline of PAUSE / RESUME / SET / the settings button. 0 = no outline.
  Only if label, outline *and* plate are all 0 does a faint outline stay, so the button can never become impossible to find.
* **Free position for the outside-cutscene button:** `GameplayButtonX` / `GameplayButtonY` = where its CENTRE goes, in % of the screen
  (0-100). Default 50 / 4.5 = top-centre, clear of the radar (it overlapped the radar in the top-left in 1.0.1). It is always kept fully on
  screen, and the settings window opens beside / below it. Sliders "X position" / "Y position" in the window. (`GameplayButtonCorner` from
  1.0.1 is gone.) A screen button is flat, so there is no Z.

## What changed in 1.0.1

* **"Cutscene Controller Settings" button outside cutscenes.** When you are *not* in a cutscene, a button with the full
  name replaces the little `SET` button (which stays during cutscenes next to PAUSE / RESUME). It sits in the **top-left**
  by default, tucked close to the edge, and opens the same settings window. The window opens beside / below it, never on top of
  it, so the same button closes it again. Move it with `GameplayButtonCorner`, rename it with `SettingsLabelText`, switch
  it off with `GameplaySettingsButton=0` (or the "Show outside cutscenes" checkbox - an open window never vanishes when you
  untick it). `ShowInterface=0` hides it too. Outside cutscenes nothing else is drawn and only touches *on* the button / window are taken.
* **See-through PAUSE / RESUME / SET labels.** The dark plate behind the label is now fully transparent by default
  (`ButtonOpacity=0`) and the label text is 2/3 white (`LabelOpacity=170`, new slider "Label opacity" in the window). A faint
  drop shadow keeps it readable on dark scenes. The outline follows the label opacity but never fades below 25%, so
  even at 0 you can still find and tap the button. An existing `.ini` that still has the old untouched `ButtonOpacity=120`
  is switched to 0 once (a value you chose yourself is kept).
* Fix: touches on a button that has just been hidden were still swallowed for one frame.
* Label centring no longer depends on "Menu size".

## What changed in 1.0.0

### 1. The UI is now the real Dear ImGui from CLEO ImGui
Your own overlay never showed the **PAUSE / RESUME** text on the phone, while CLEO ImGui draws fine in the
same game. So the home-made renderer (`UIDraw` / `UIGL` / `FontData`) is **gone** and the mod now carries
the real **Dear ImGui 1.89.7 source copied unchanged from `CLEO_ImGui-main`** (`src/imgui/`), drawn the
exact way CLEO ImGui draws it:

* ImGui's vertices go to the game's own `RwIm2DRenderIndexedPrimitive` (same render states, same
  `CSprite2d::NearScreenZ / RecipNearClip`, same `CWidget::SetScissor` clipping) - no private GL context to desync.
* The font is the same Arial TTF from CLEO ImGui (`src/ArialFont.h` = its `arial.h`), uploaded as a
  RenderWare raster through `RwImage` / `RwRasterSetFromImage`.
* It is drawn from a hook on **`Render2dStuff`**, i.e. right after the game's own HUD - exactly where
  CLEO ImGui draws - so it always lands on top. (`RenderEffects` still does the pause blur, behind the HUD.)
* CLEO ImGui's backend hard-codes **armeabi-v7a** PLT offsets. This mod resolves every RenderWare function by
  name, so **the same code works on armeabi-v7a and arm64-v8a** (all symbols checked in both of your `libGTASA.so`).
* It is **self-contained**: it does not need the CLEO ImGui plugin installed, and its private ImGui copy is not
  exported (`-fvisibility=hidden`; verified with `readelf`), so it can't collide with CLEO ImGui's own ImGui.

### 2. PAUSE / RESUME label - fixed
* The big button is a normal `ImGui::Button` whose label is **never empty**: if `PauseLabelText=` /
  `ResumeLabelText=` are blank in the `.ini` it falls back to `PAUSE` / `RESUME` (before, a blank value meant a blank button).
* The label flips PAUSE -> RESUME without the widget changing ID or size (the button is as wide as the longer label).
* Every step of the font upload is checked and logged. If it ever fails the UI turns itself off with a log line
  (`[CutsceneCtrl] ImGui: ...`) instead of silently drawing nothing or crashing.

### 3. Layout from `docs/previews`
Bottom-right `PAUSE` / `RESUME` button (gold outline while paused) with a small `SET` button beside it, big
`PAUSED` banner, and the floating **"Cutscene Control Settings"** ImGui window: collapse arrow, close X, drag the
title bar to move, drag the corner grip to resize, scrollbar, section headers, checkboxes, sliders, combo boxes,
buttons. It holds every setting, applies instantly and saves to the `.ini` when you close the window.
Touch handling: a swipe that starts on a slider or checkbox **scrolls** the window (never changes the value);
a horizontal drag moves a slider; a tap on a slider jumps it. The window can't be dragged completely off screen.
`Menu size` (default 1.0 = the reference layout) rescales the window; the PAUSE button does not change with it.

### Carried over: peds drifting "like a ragdoll" while paused — fixed at the root
Disassembly shows why: `CTimer::Update` stores `ms_fTimeStepNonClipped` **before** its pause check,
and `CCutsceneMgr::Update_overlay` advances the cutscene clock (`ms_cutsceneTimer`) with *that*
value — so setting the pause flags never stopped the clock, and anything animated from it kept
moving while the rest of the world stood still (that is the same increment the PC mod NOPs).
Now, while paused: `CTimer::Update` is hooked to zero both timesteps, and the cutscene clock is
captured at pause time and held there. Tested: the clock stays at the exact captured value across
30 paused frames, and time flows normally again on resume. (If something *still* moves, tell me
which scene — it would then be something outside the game's timers, e.g. another mod's physics.)

## Settings (`.ini`)
```ini
[Input]    ButtonPause=Start  ButtonSkip=Cross  UseSkipGameKeys=1  SkipInPause=1  PauseOnlyDuringMissions=0
[Camera]   ToggleCameraButton=Select  InitialSpeed=0.3  Sensitivity=0.006  ShowSpeedNumber=1
[Interface]
ShowInterface=1  PauseButton=1  SettingsButton=1  PauseLabelText=PAUSE  ResumeLabelText=RESUME
GameplaySettingsButton=1  SettingsLabelText=Cutscene Controller Settings   ; the button shown when NOT in a cutscene
GameplayButtonX=50  GameplayButtonY=4.5   ; centre of that button, % of the screen (0-100)
ButtonCorner=0        ; (cutscene buttons) 0 bottom-right, 1 bottom-left, 2 top-right, 3 top-left
ButtonScale=1.0  ButtonOpacity=0  LabelOpacity=170  OutlineOpacity=170  ButtonMarginX=0.025  ButtonMarginY=0.06   ; plate fill / label text / outline, each 0-255
FontStyle=-1  FontYOffset=0   ; -1 = Arial, 0 Gothic, 1 Subtitles, 2 Menu, 3 Pricedown; nudge in % of the text height
Vignette=1  VignetteStrength=160  VignetteSize=50
MenuScale=1.0         ; size of the floating window (1.0 = reference layout)
ShowPauseText=1  PauseText=PAUSED  PauseTextScale=1.0
ActiveBlur=1  BlurAlpha=170  UseGaussianShader=0   ; 1 = real GLES2 blur (applies at the next pause)
FreezeFrame=0         ; keep drawing the screen captured at the moment of the pause (hides drift)
ShowMissionName=1  ShowSubtitles=1
[FastForward]         ; EXPERIMENTAL
HoldFastForward=0  FastForwardSpeed=2.0  FastForwardButton=R2  FastForwardScreenButton=1
FastForwardMuteAudio=1  FastForwardScriptedScenes=0
[Others]   FixAudioDesync=1 (pause on return from background mid-cutscene)  PauseGameAudio=1
[Audio]    PauseSound=  ResumeSound=
```
Removed: `ButtonFont`, `UIFlipY` (no longer needed). Old unused keys are ignored. An old `MenuScale=1.20` in an existing .ini is kept - delete the line or set it to 1.0 for the reference size.

## Verification
* `tools/verify_symbols.sh <libGTASA.so>` - all **75** runtime-resolved symbols exist in both your armeabi-v7a and arm64-v8a builds.
* `tests/run.sh` - **1545 checks**, under AddressSanitizer + UBSan: the real `UI.cpp` + `ImGuiRW.cpp` + Dear ImGui driven
  with simulated fingers against a software RenderWare rasteriser plus a fake CFont (1328: label pixels, centring, flip PAUSE/RESUME, corners,
  scales, every checkbox/slider/combo, scrolling, move/resize/collapse/close, 4 screen sizes, failure paths), the pause state
  machine incl. the `RenderEffects -> Render2dStuff` hand-off and the gameplay-button, vignette, fast-forward and freeze-frame rules (178), the real-Gaussian blur shader (9) and the freeze-frame capture against a fake RenderWare (30).
* **Not verified:** the real CFont output (the host test only checks the position / size / colour / order of what we ask CFont to print, using a fake CFont), and how a phone's GPU / the game's RenderWare turns those triangles into pixels. If anything is off, the log has
  `[CutsceneCtrl] ImGui ... ready`, `font raster ready`, `UI: ImGui render size ...`.
* Disassembly notes behind every hard-coded number: `tools/NOTES_offsets.md`.

## Layout
`src/imgui/` Dear ImGui 1.89.7 (from CLEO ImGui, unchanged) - `src/ImGuiRW.*` RenderWare backend (CLEO ImGui's, ported to
by-name symbols + checked) - `src/ArialFont.h` font - `src/UI.*` buttons, banner, settings window, touch routing -
`src/CutsceneCtrl.*` state machine/hooks/config - `src/BlurFX.*` blur - `src/GameSymbols.*` - `tests/` - `tools/` - `docs/previews/`.
Build: `ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=./Android.mk NDK_APPLICATION_MK=./Application.mk NDK_DEBUG=0`
(built here with NDK r30; the mod is ~1.1 MB because the Arial font is embedded).

## Earlier versions
1.0.2 game fonts, vignette, outline opacity, X/Y button - 1.0.1 outside-cutscene button, see-through labels - 1.0.0 Dear ImGui UI

## Before 1.0 (home-made OpenGL UI)
1.2.0 own GLES renderer + floating window, time-freeze fix - 1.1.0 stuck-pause safety nets, free-cam only while paused -
1.0.4 `CFont::SetColor` passes `CRGBA` by hidden pointer - 1.0.2 per-ABI `CCamera` offsets - 1.0.1 widget built too early.

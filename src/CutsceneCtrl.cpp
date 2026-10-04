#include "CutsceneCtrl.h"
#include "GameSymbols.h"
#include "BlurFX.h"
#include "PauseAudio.h"
#include "UI.h"
#include <mod/amlmod.h>
#include <mod/iaml.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <cstring>
#include <strings.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <ctime>
#include <vector>

// ===========================================================================
// Gamepad button helpers
// ===========================================================================
static const struct { const char* name; GamepadButton btn; } kButtonNames[] = {
    { "Cross", GamepadButton::Cross },   { "Circle", GamepadButton::Circle },
    { "Square", GamepadButton::Square }, { "Triangle", GamepadButton::Triangle },
    { "Start", GamepadButton::Start },   { "Select", GamepadButton::Select },
    { "L1", GamepadButton::L1 }, { "L2", GamepadButton::L2 }, { "R1", GamepadButton::R1 }, { "R2", GamepadButton::R2 },
    { "DPadUp", GamepadButton::DPadUp },     { "DPadDown", GamepadButton::DPadDown },
    { "DPadLeft", GamepadButton::DPadLeft }, { "DPadRight", GamepadButton::DPadRight },
};

GamepadButton ButtonFromString(const char* s)
{
    if (!s) return GamepadButton::None;
    for (const auto& e : kButtonNames)
        if (!strcasecmp(s, e.name)) return e.btn;
    return GamepadButton::None;
}

static const char* ButtonName(GamepadButton b)
{
    for (const auto& e : kButtonNames)
        if (e.btn == b) return e.name;
    return "None";
}

bool IsButtonDown(GamepadButton btn, const CControllerState& s)
{
    switch (btn)
    {
        case GamepadButton::Cross:     return s.ButtonCross != 0;
        case GamepadButton::Circle:    return s.ButtonCircle != 0;
        case GamepadButton::Square:    return s.ButtonSquare != 0;
        case GamepadButton::Triangle:  return s.ButtonTriangle != 0;
        case GamepadButton::Start:     return s.Start != 0;
        case GamepadButton::Select:    return s.Select != 0;
        case GamepadButton::L1:        return s.LeftShoulder1 != 0;
        case GamepadButton::L2:        return s.LeftShoulder2 != 0;
        case GamepadButton::R1:        return s.RightShoulder1 != 0;
        case GamepadButton::R2:        return s.RightShoulder2 != 0;
        case GamepadButton::DPadUp:    return s.DPadUp != 0;
        case GamepadButton::DPadDown:  return s.DPadDown != 0;
        case GamepadButton::DPadLeft:  return s.DPadLeft != 0;
        case GamepadButton::DPadRight: return s.DPadRight != 0;
        default: return false;
    }
}

namespace CutsceneCtrl
{
    Settings g_Settings;
    bool g_bCutscenePaused = false;
    bool g_bFreeCamActive = false;
    bool g_bFastForwarding = false;

    // ---- per-frame cached game state ------------------------------------
    // Everything below is refreshed exactly once per logic frame in
    // OnLogicFrame(), so every consumer in that frame (and the render frame
    // that follows) sees one consistent answer. Notably the debounce counter
    // must tick once per FRAME, not once per call.
    static bool     s_inCutscene = false;
    static bool     s_fadeBlocked = false;      // screen is mid-fade or fully black
    static bool     s_focus = true;
    static int      s_wideStreak = 0;
    static uint64_t s_cutsceneStartMs = 0;
    static uint64_t s_lastLogicMs = 0;
    static uint64_t s_lastFocusQueryMs = 0;

    static uint64_t s_lastPauseToggleMs = 0;
    static uint64_t s_lastCamToggleMs = 0;
    static bool     s_needBlurCapture = false;
    static bool     s_audioPaused = false;
    static float    s_frozenCutsceneTimer = 0.0f;   // ms_cutsceneTimer captured at pause time
    static bool     s_timerFrozen = false;

    // Free-camera working state
    static float s_camPosX, s_camPosY, s_camPosZ;
    static float s_camAngX, s_camAngY;
    static CVector s_lastFixedCamVector, s_lastFixedCamSource, s_lastFixedCamUpOffset;

    static uint64_t NowMs()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
    }

    // ===========================================================================
    // Game-state queries
    // ===========================================================================
    static bool CutsceneManagerRunning()
    {
        return Sym::CCutsceneMgr_ms_running && *Sym::CCutsceneMgr_ms_running;
    }

    // Raw detection, called once per logic frame. Debounced: interior
    // transitions and area loads can flicker m_WideScreenOn for a stray frame
    // or two without any real cutscene, so it must hold for 3 consecutive
    // frames (~50 ms) before it counts.
    static bool DetectCutscene()
    {
        const bool wideNow = Sym::TheCamera && Sym::TheCamera[CCameraOffsets::m_WideScreenOn] != 0;
        s_wideStreak = wideNow ? (s_wideStreak + 1) : 0;
        return CutsceneManagerRunning() || s_wideStreak >= 3;
    }

    bool IsOnAnyCutscene()      { return s_inCutscene; }
    bool IsOnScriptedCutscene() { return s_inCutscene && !CutsceneManagerRunning(); }

    static bool CanPauseNow()
    {
        if (g_Settings.pauseOnlyDuringMissions && Sym::CTheScripts_IsPlayerOnAMission)
            return Sym::CTheScripts_IsPlayerOnAMission();
        return true;
    }

    static bool QueryForeground()
    {
        JNIEnv* env = aml->GetJNIEnvironment();
        jobject activity = aml->GetCurrentActivity();
        if (!env || !activity) return true;       // fail open: never block input on a query error
        jclass cls = env->GetObjectClass(activity);
        if (!cls) return true;
        bool result = true;
        jmethodID mid = env->GetMethodID(cls, "hasWindowFocus", "()Z");
        if (mid) result = (env->CallBooleanMethod(activity, mid) == JNI_TRUE);
        if (env->ExceptionCheck()) { env->ExceptionClear(); result = true; }
        env->DeleteLocalRef(cls);
        return result;
    }

    // ===========================================================================
    // Pause / resume
    // ===========================================================================
    static void SetAudioPaused(bool pause, bool force = false)
    {
        if (pause == s_audioPaused) return;
        if (pause && !force && !g_Settings.pauseGameAudio) return;
        Sym::Fn_CAudioEngine_Void fn = pause ? Sym::CAudioEngine_PauseAllSounds : Sym::CAudioEngine_ResumeAllSounds;
        if (!fn) return;
        fn(Sym::AudioEngine);
        s_audioPaused = pause;
    }

    static void DeactivateFreeCam(bool cutsceneEnded);

    // ---- EXPERIMENTAL hold-for-fast-forward ---------------------------------------------------------------------------
    // CTimer::Update turns the real frame time into ms_fTimeStep AND ms_fTimeStepNonClipped through ms_fTimeScale (disassembly:
    // both come from scale * delta, unless the game is paused). The cutscene clock, the animations and the script timers all run off
    // those, so scaling it speeds the whole scene up consistently -- the cutscene clock itself can't be driven directly.
    // The scale is only raised *around* the call to CTimer::Update and put back afterwards, so nothing else ever sees our value.
    // ms_fTimeStep is clipped to 3.0 by the game, so on a slow device (< ~40 fps) the world tops out a bit below the requested speed.
    static bool s_ffAudioMuted = false;
    static void SetFastForward(bool on)
    {
        if (on == g_bFastForwarding) return;
        g_bFastForwarding = on;
        if (on)
        {
            if (g_Settings.ffMuteAudio && !s_audioPaused) { SetAudioPaused(true, true); s_ffAudioMuted = s_audioPaused; }
        }
        else if (s_ffAudioMuted)
        {
            s_ffAudioMuted = false;
            // paused (with "silence game audio" on) keeps the sound off; otherwise give it back
            if (!g_bCutscenePaused || !g_Settings.pauseGameAudio) SetAudioPaused(false);
        }
    }

    // The one place game pause state changes. Idempotent: only acts on a real
    // transition so it can never fight the game's own use of these flags.
    static void SetGamePaused(bool paused)
    {
        if (paused == g_bCutscenePaused) return;
        g_bCutscenePaused = paused;
        if (paused) { SetFastForward(false); BlurFX::InvalidateFrozen(); }

        if (Sym::CTimer_m_UserPause) *Sym::CTimer_m_UserPause = paused;
        if (Sym::CTimer_m_CodePause) *Sym::CTimer_m_CodePause = paused;
        SetAudioPaused(paused);

        // Freeze the cutscene clock. CCutsceneMgr::Update_overlay advances ms_cutsceneTimer with
        // CTimer::ms_fTimeStepNonClipped, which CTimer::Update stores BEFORE its pause check -- so the
        // pause flags alone never stop it (the PC mod NOPs the same increment). Left running, cutscene
        // animation keeps drifting forward while the world is "paused" (the floaty, ragdoll-like peds).
        s_timerFrozen = paused && Sym::CCutsceneMgr_ms_cutsceneTimer;
        if (s_timerFrozen) s_frozenCutsceneTimer = *Sym::CCutsceneMgr_ms_cutsceneTimer;

        if (paused)
        {
            s_needBlurCapture = true;
            if (g_Settings.pauseSoundFile[0]) PauseAudio::PlayFile(g_Settings.pauseSoundFile);
        }
        else
        {
            DeactivateFreeCam(false);   // the free camera only makes sense while paused
            if (g_Settings.resumeSoundFile[0]) PauseAudio::PlayFile(g_Settings.resumeSoundFile);
        }
    }

    bool TogglePause()
    {
        const uint64_t now = NowMs();
        if (now < s_lastPauseToggleMs + 350) return false;

        if (!g_bCutscenePaused)
        {
            // Never START a pause unless it is clearly safe: a real cutscene,
            // not mid-fade (pausing a fade freezes the screen half-black).
            if (!s_inCutscene || s_fadeBlocked || !CanPauseNow()) return false;
        }
        SetGamePaused(!g_bCutscenePaused);
        s_lastPauseToggleMs = now;
        return true;
    }

    // ===========================================================================
    // Free camera (only ever active while paused)
    // ===========================================================================
    static void UpdateFreeCamera(CPad* pad)
    {
        // m_UserPause/m_CodePause stop the game calling CCamera::Process, so
        // we drive it ourselves while paused (same as the PC version).
        if (Sym::CCamera_Process) Sym::CCamera_Process(Sym::TheCamera);
        if (!pad) return;

        s_camAngX += (float)pad->NewState.RightStickX * g_Settings.camSensitivity;
        s_camAngY -= (float)pad->NewState.RightStickY * g_Settings.camSensitivity;
        if (s_camAngY > 1.5f)  s_camAngY = 1.5f;
        if (s_camAngY < -1.5f) s_camAngY = -1.5f;

        const float dirX = cosf(s_camAngY) * sinf(s_camAngX);
        const float dirY = cosf(s_camAngY) * cosf(s_camAngX);
        const float dirZ = sinf(s_camAngY);
        const float sp = g_Settings.camSpeed;

        if (pad->NewState.RightShoulder1) { s_camPosX += dirX * sp; s_camPosY += dirY * sp; s_camPosZ += dirZ * sp; }
        if (pad->NewState.LeftShoulder1)  { s_camPosX -= dirX * sp; s_camPosY -= dirY * sp; s_camPosZ -= dirZ * sp; }
        if (pad->NewState.DPadUp   && !pad->OldState.DPadUp)   { g_Settings.camSpeed += 0.1f; SettingChanged(&g_Settings.camSpeed); }
        if (pad->NewState.DPadDown && !pad->OldState.DPadDown) { g_Settings.camSpeed = fmaxf(0.05f, g_Settings.camSpeed - 0.1f); SettingChanged(&g_Settings.camSpeed); }

        CVector camPos(s_camPosX, s_camPosY, s_camPosZ);
        CVector lookAt(s_camPosX + dirX, s_camPosY + dirY, s_camPosZ + dirZ);
        CVector zero(0.0f, 0.0f, 0.0f);
        if (Sym::CCamera_SetCamPositionForFixedMode) Sym::CCamera_SetCamPositionForFixedMode(Sym::TheCamera, camPos, zero);
        if (Sym::CCamera_TakeControlNoEntity)        Sym::CCamera_TakeControlNoEntity(Sym::TheCamera, lookAt, /*JUMPCUT*/2, /*INTERPOLATION*/1);
    }

    static void ActivateFreeCam()
    {
        if (!Sym::TheCamera || g_bFreeCamActive) return;
        s_lastFixedCamVector   = *(CVector*)(Sym::TheCamera + CCameraOffsets::m_vecFixedModeVector);
        s_lastFixedCamSource   = *(CVector*)(Sym::TheCamera + CCameraOffsets::m_vecFixedModeSource);
        s_lastFixedCamUpOffset = *(CVector*)(Sym::TheCamera + CCameraOffsets::m_vecFixedModeUpOffSet);
        s_camPosX = s_lastFixedCamSource.x; s_camPosY = s_lastFixedCamSource.y; s_camPosZ = s_lastFixedCamSource.z;
        s_camAngX = s_camAngY = 0.0f;
        g_bFreeCamActive = true;
    }

    // Always leaves the camera in a state the game can carry on from.
    static void DeactivateFreeCam(bool cutsceneEnded)
    {
        if (!g_bFreeCamActive) return;
        g_bFreeCamActive = false;
        if (!Sym::TheCamera) return;

        if (cutsceneEnded)
        {
            // The scene is over and we had hijacked the camera: hand it back to gameplay.
            if (Sym::CCamera_RestoreWithJumpCut) Sym::CCamera_RestoreWithJumpCut(Sym::TheCamera);
            return;
        }
        // SetCamPositionForFixedMode takes (vector, source) in that order and does
        // not touch UpOffSet, which is restored separately (disassembly-verified).
        if (IsOnScriptedCutscene() && Sym::CCamera_SetCamPositionForFixedMode)
        {
            Sym::CCamera_SetCamPositionForFixedMode(Sym::TheCamera, s_lastFixedCamVector, s_lastFixedCamSource);
            *(CVector*)(Sym::TheCamera + CCameraOffsets::m_vecFixedModeUpOffSet) = s_lastFixedCamUpOffset;
        }
        else if (Sym::CCamera_TakeControlWithSpline)
            Sym::CCamera_TakeControlWithSpline(Sym::TheCamera, /*JUMPCUT*/2);
    }

    static void ProcessFreeCamera(CPad* pad, uint64_t now)
    {
        if (!g_bCutscenePaused || !Sym::TheCamera) return;   // pause-only: no accidental hijacks during normal playback

        if (g_bFreeCamActive) UpdateFreeCamera(pad);

        const bool pressed = pad && IsButtonDown(g_Settings.buttonToggleCam, pad->NewState)
                                 && !IsButtonDown(g_Settings.buttonToggleCam, pad->OldState);
        if (pressed && now > s_lastCamToggleMs + 500)
        {
            if (g_bFreeCamActive) DeactivateFreeCam(false); else ActivateFreeCam();
            s_lastCamToggleMs = now;
        }
    }

    // ===========================================================================
    // Hooks
    // ===========================================================================

    // Skip button: gate on window focus, honour "skip while paused", and
    // otherwise defer to the game's own tap/Cross detection.
    DECL_HOOKb(CCutsceneMgr_IsCutsceneSkipButtonBeingPressed)
    {
        if (!s_focus) return false;
        if (g_bCutscenePaused && !g_Settings.skipInPause) return false;

        if (g_Settings.useSkipGameKeys)
            return CCutsceneMgr_IsCutsceneSkipButtonBeingPressed();

        CPad* pad = Sym::CPad_GetPad ? Sym::CPad_GetPad(0) : nullptr;
        return pad && IsButtonDown(g_Settings.buttonSkip, pad->NewState) && !IsButtonDown(g_Settings.buttonSkip, pad->OldState);
    }

    // Freeze the on-screen mission timer while paused (skipping its update
    // is safer than poking an internal flag whose offset we haven't verified).
    DECL_HOOKv(COnscreenTimerEntry_Process, void* self)
    {
        if (g_bCutscenePaused) return;
        COnscreenTimerEntry_Process(self);
    }

    DECL_HOOKv(CHud_DrawMissionTitle)
    {
        if (g_bCutscenePaused && !g_Settings.showMissionName) return;
        CHud_DrawMissionTitle();
    }

    DECL_HOOKv(CHud_DrawSubtitles)
    {
        if (g_bCutscenePaused && !g_Settings.showSubtitles) return;
        CHud_DrawSubtitles();
    }

    // The game's frame order is ... RenderScene -> RenderEffects -> Render2dStuff (HUD, radar, subtitles).
    // RenderEffects: decide whether the UI is wanted, capture + draw the blur (it must sit behind the HUD).
    // Render2dStuff: after the game's own 2D pass, draw the Dear ImGui overlay -- on top of everything, which is
    // exactly where the CLEO ImGui plugin draws.
    static bool s_uiFramePending = false;
    static UI::FrameInfo s_uiFrame;
    static char s_uiCamText[64];

    DECL_HOOKv(RenderEffects)
    {
        RenderEffects();
        OnRenderFrame();
        // If the game's 2D pass could not be hooked, still draw the UI from here (it then sits under the HUD).
        if (!Sym::addr_Render2dStuff && s_uiFramePending) { s_uiFramePending = false; UI::Frame(s_uiFrame); }
    }

    DECL_HOOKv(Render2dStuff)
    {
        Render2dStuff();
        if (s_uiFramePending) { s_uiFramePending = false; UI::Frame(s_uiFrame); }
    }

    // Runs at the start of every frame. While we hold the pause, make time truly stand still:
    // CTimer::Update leaves ms_fTimeStepNonClipped at the real frame time even when paused, and
    // several consumers (the cutscene clock, animation blending) read that one.
    DECL_HOOKv(CTimer_Update)
    {
        if (g_bFastForwarding && Sym::CTimer_ms_fTimeScale && !g_bCutscenePaused)
        {
            const float orig = *Sym::CTimer_ms_fTimeScale;
            const float spd = std::max(1.0f, std::min(g_Settings.ffSpeed, 4.0f));
            *Sym::CTimer_ms_fTimeScale = orig * spd;
            CTimer_Update();
            *Sym::CTimer_ms_fTimeScale = orig;
        }
        else CTimer_Update();
        if (g_bCutscenePaused)
        {
            if (Sym::CTimer_ms_fTimeStep)            *Sym::CTimer_ms_fTimeStep = 0.0f;
            if (Sym::CTimer_ms_fTimeStepNonClipped)  *Sym::CTimer_ms_fTimeStepNonClipped = 0.0f;
        }
    }

    DECL_HOOKv(CCutsceneMgr_Update)
    {
        CCutsceneMgr_Update();
        // Belt and braces: undo any advance of the cutscene clock that got through this frame.
        if (s_timerFrozen && g_bCutscenePaused && Sym::CCutsceneMgr_ms_cutsceneTimer)
            *Sym::CCutsceneMgr_ms_cutsceneTimer = s_frozenCutsceneTimer;
        OnLogicFrame();
    }

    // Runs on the Java UI thread: keep it tiny. Touches that start on one of
    // our buttons are swallowed so the game never sees them as taps.
    DECL_HOOKv(AND_TouchEvent, int type, int id, int x, int y)
    {
        if (UI::PushTouchEvent(type, id, x, y)) return;
        AND_TouchEvent(type, id, x, y);
    }

    // ===========================================================================
    // Per-frame logic
    // ===========================================================================
    static void OnCutsceneEnded()
    {
        // Hand the camera back to gameplay FIRST. (If we resumed first, the
        // normal resume path would quietly drop the free camera using the
        // in-cutscene restore, and the proper gameplay restore would be skipped.)
        DeactivateFreeCam(true);

        // Safety net #1: never leave the game frozen just because cutscene
        // detection dropped out from under us (an interior/area transition, a
        // script ending mid-pause, ...). Being stuck paused is far worse than
        // an unwanted auto-resume.
        if (g_bCutscenePaused)
        {
            logger->Info("[CutsceneCtrl] Cutscene ended (or detection dropped) while paused -- forcing resume.");
            SetGamePaused(false);
        }
        SetFastForward(false);
        SetAudioPaused(false);
        UI::OnCutsceneEnded();
    }

    // Is the 2x hold allowed right now (setting on, symbol found, a real cutscene file -- or a scripted scene if the user opted in,
    // not paused, not in the free camera, not faded)?
    static bool FastForwardAvailable()
    {
        return g_Settings.ffEnabled && Sym::CTimer_ms_fTimeScale && s_inCutscene && !g_bCutscenePaused && !g_bFreeCamActive && !s_fadeBlocked &&
               (CutsceneManagerRunning() || g_Settings.ffScripted);
    }

    static void ProcessFastForward(CPad* pad)
    {
        bool want = false;
        if (FastForwardAvailable())
        {
            const bool padHeld = pad && g_Settings.ffButton != GamepadButton::None && IsButtonDown(g_Settings.ffButton, pad->NewState);
            const bool uiHeld  = g_Settings.ffScreenButton && UI::FastForwardHeld();
            want = padHeld || uiHeld;
        }
        SetFastForward(want);
    }

    void OnLogicFrame()
    {
        if (!Sym::g_bCoreResolved) return;
        const uint64_t now = NowMs();

        // ---- refresh cached state (once per frame) -------------------------
        const bool detected = DetectCutscene();
        if (detected && !s_inCutscene) s_cutsceneStartMs = now;
        const bool wasInCutscene = s_inCutscene;
        s_inCutscene = detected;

        if (wasInCutscene && !s_inCutscene)
            OnCutsceneEnded();

        if (!s_inCutscene)
        {
            s_fadeBlocked = false;
            s_focus = true;             // don't carry a stale "unfocused" reading into the next cutscene
            s_lastLogicMs = now;
            // Belt and braces: whatever happened, outside a cutscene we must
            // not be holding the game paused.
            if (g_bCutscenePaused) SetGamePaused(false);
            return;
        }

        s_fadeBlocked = Sym::CCamera_GetScreenFadeStatus && Sym::CCamera_GetScreenFadeStatus(Sym::TheCamera) != 0;
        if (now - s_lastFocusQueryMs > 250) { s_focus = QueryForeground(); s_lastFocusQueryMs = now; }

        // Safety net #2: a fade starting while we're paused means the scene is
        // being cut away (or we caught a transition) -- don't freeze it half-black.
        if (g_bCutscenePaused && s_fadeBlocked)
        {
            logger->Info("[CutsceneCtrl] Screen fade began while paused -- forcing resume.");
            SetGamePaused(false);
        }

        // Coming back after the app was in the background mid-cutscene: the
        // game's clock jumped but its audio kept its own pace, which is what
        // desynchronises them. Pause so the player resumes on their terms.
        // Armed only once the scene has been running a while, so the long
        // asset-streaming stalls at the START of a cutscene never trigger it.
        if (g_Settings.fixAudioDesync && !g_bCutscenePaused && !s_fadeBlocked && s_lastLogicMs != 0 &&
            (now - s_lastLogicMs) > 3000 && (s_lastLogicMs - s_cutsceneStartMs) > 2500 && CanPauseNow())
        {
            logger->Info("[CutsceneCtrl] Returned after %llu ms away mid-cutscene -- pausing.", (unsigned long long)(now - s_lastLogicMs));
            SetGamePaused(true);
            s_lastPauseToggleMs = now;
        }
        s_lastLogicMs = now;

        CPad* pad = Sym::CPad_GetPad ? Sym::CPad_GetPad(0) : nullptr;

        // ---- gamepad pause / resume --------------------------------------
        if (pad && IsButtonDown(g_Settings.buttonPause, pad->NewState) && !IsButtonDown(g_Settings.buttonPause, pad->OldState))
            TogglePause();

        ProcessFreeCamera(pad, now);
        ProcessFastForward(pad);

        if (g_bCutscenePaused)
        {
            if (Sym::CCutsceneMgr_IsCutsceneSkipButtonBeingPressed && HookOf_CCutsceneMgr_IsCutsceneSkipButtonBeingPressed())
            {
                SetGamePaused(false);
                if (Sym::CCutsceneMgr_SkipCutscene) Sym::CCutsceneMgr_SkipCutscene();
            }
            else if (Sym::CTheScripts_Process)
            {
                // The game stops running scripts while paused; pump them so
                // CLEO scripts keep executing during a cutscene pause.
                Sym::CTheScripts_Process();
            }
        }
    }

    void OnRenderFrame()
    {
        s_uiFramePending = false;
        if (!Sym::g_bCoreResolved) return;

        // During a cutscene: the PAUSE / RESUME + SET buttons (hidden while the screen is faded).
        // Outside a cutscene: only the "Cutscene Controller Settings" button -- and the settings window if it is open
        // (so unticking the button's checkbox in the window never makes the window vanish under the finger).
        const bool inCutscene = s_inCutscene;
        const bool active = g_Settings.showInterface &&
                            (inCutscene ? !s_fadeBlocked : (g_Settings.showGameplayButton || UI::IsSettingsOpen()));
        // The vignette is its own switch: it is drawn in a cutscene even when the buttons are hidden (but not while the screen is faded).
        const bool vignette = inCutscene && !s_fadeBlocked && g_Settings.showVignette && g_Settings.vignetteStrength > 0;
        UI::SetActive(active);
        if (!active && !vignette) return;

        UI::FrameInfo& fi = s_uiFrame;
        fi = UI::FrameInfo();
        fi.inCutscene = inCutscene;
        fi.drawUI = active;
        fi.vignette = vignette;
        fi.paused = inCutscene && g_bCutscenePaused;
        fi.ffButton = inCutscene && !fi.paused && g_Settings.ffScreenButton && FastForwardAvailable();
        if (fi.paused && active)
        {
            // Capture at render time, after the game drew the scene and before any of our own overlay,
            // so the blurred image doesn't contain our buttons.
            const bool freeze = g_Settings.freezeFrame && !g_bFreeCamActive;     // the free camera needs the LIVE view
            if (s_needBlurCapture)
            {
                s_needBlurCapture = false;
                if (g_Settings.showBlur) BlurFX::CaptureNow(g_Settings.useGaussianShader);
                if (freeze) BlurFX::CaptureFrozen();
            }
            else if (freeze && !BlurFX::HasFrozen()) BlurFX::CaptureFrozen();     // ticked while already paused (BlurFX stops retrying after a failure)
            // Freeze frame: keep showing the pixels of the moment the pause began instead of the live scene. Vanilla physics /
            // animation keeps creeping while the game is "paused" and the menu normally hides it; this hides it the same way.
            if (freeze && BlurFX::HasFrozen()) BlurFX::DrawFrozen();
            if (g_Settings.showBlur) BlurFX::DrawFullscreen((uint8_t)g_Settings.blurAlpha, g_Settings.useGaussianShader);
            if (g_Settings.showPauseText) fi.banner = g_Settings.pauseText;
            if (g_bFreeCamActive && g_Settings.showCamSpeedText)
            {
                snprintf(s_uiCamText, sizeof(s_uiCamText), "Free camera   speed %.2f", g_Settings.camSpeed);
                fi.info = s_uiCamText;
            }
        }
        s_uiFramePending = true;
    }

    // ===========================================================================
    // Config registry -- every setting the floating panel can edit is bound
    // here, so the panel, the .ini and the in-memory value never disagree.
    // ===========================================================================
    struct Binding { void* field; char kind; size_t n; ConfigEntry* e; };
    static std::vector<Binding> s_bindings;
    static bool s_cfgDirty = false;

    static void BindBool(bool* f, const char* key, const char* sec)
    {
        ConfigEntry* e = cfg->Bind(key, *f, sec);
        if (e) *f = e->GetBool();
        s_bindings.push_back({ f, 'b', 0, e });
    }
    static void BindInt(int* f, const char* key, const char* sec, int lo, int hi)
    {
        ConfigEntry* e = cfg->Bind(key, *f, sec);
        if (e) { int v = e->GetInt(); *f = v < lo ? lo : (v > hi ? hi : v); }
        s_bindings.push_back({ f, 'i', 0, e });
    }
    static void BindFloat(float* f, const char* key, const char* sec, float lo, float hi)
    {
        ConfigEntry* e = cfg->Bind(key, *f, sec);
        if (e) { float v = e->GetFloat(); *f = v < lo ? lo : (v > hi ? hi : v); }
        s_bindings.push_back({ f, 'f', 0, e });
    }
    static void BindStr(char* buf, size_t n, const char* key, const char* sec)
    {
        ConfigEntry* e = cfg->Bind(key, (const char*)buf, sec);
        if (e) { strncpy(buf, e->GetString(), n - 1); buf[n - 1] = 0; }
        s_bindings.push_back({ buf, 's', n, e });
    }
    static void BindPad(GamepadButton* f, const char* key, const char* sec)
    {
        ConfigEntry* e = cfg->Bind(key, ButtonName(*f), sec);
        if (e) { GamepadButton b = ButtonFromString(e->GetString()); if (b != GamepadButton::None) *f = b; }
        s_bindings.push_back({ f, 'p', 0, e });
    }

    static void Reload(const Binding& b)
    {
        if (!b.e) return;
        switch (b.kind)
        {
            case 'b': *(bool*)b.field = b.e->GetBool(); break;
            case 'i': *(int*)b.field = b.e->GetInt(); break;
            case 'f': *(float*)b.field = b.e->GetFloat(); break;
            case 's': strncpy((char*)b.field, b.e->GetString(), b.n - 1); ((char*)b.field)[b.n - 1] = 0; break;
            case 'p': { GamepadButton g = ButtonFromString(b.e->GetString()); if (g != GamepadButton::None) *(GamepadButton*)b.field = g; break; }
        }
    }

    void SettingChanged(const void* field)
    {
        for (const Binding& b : s_bindings)
        {
            if (b.field != field) continue;
            if (b.e)
            {
                switch (b.kind)
                {
                    case 'b': b.e->SetBool(*(bool*)b.field); break;
                    case 'i': b.e->SetInt(*(int*)b.field); break;
                    case 'f': b.e->SetFloat(*(float*)b.field); break;
                    case 's': b.e->SetString((const char*)b.field); break;
                    case 'p': b.e->SetString(ButtonName(*(GamepadButton*)b.field)); break;
                }
            }
            s_cfgDirty = true;
            return;
        }
    }

    void SaveConfigNow()
    {
        if (s_cfgDirty && cfg) { cfg->Save(); s_cfgDirty = false; }
    }

    void ResetAllSettings()
    {
        for (const Binding& b : s_bindings)
        {
            if (!b.e) continue;
            b.e->Reset();
            Reload(b);
        }
        s_cfgDirty = true;
        SaveConfigNow();
        logger->Info("[CutsceneCtrl] All settings reset to defaults.");
    }

    void ApplyPreset(int preset)
    {
        if (preset == 0) { ResetAllSettings(); return; }
        // Minimal: just a small button, no blur, no banner.
        Settings& s = g_Settings;
        s.showBlur = false; s.showPauseText = false; s.buttonScale = 0.8f; s.buttonPlateAlpha = 0;
        for (const void* f : { (const void*)&s.showBlur, (const void*)&s.showPauseText, (const void*)&s.buttonScale, (const void*)&s.buttonPlateAlpha })
            SettingChanged(f);
        SaveConfigNow();
        logger->Info("[CutsceneCtrl] Applied the minimal preset.");
    }

    void LoadConfig()
    {
        if (!cfg) return;
        Settings& s = g_Settings;
        s_bindings.clear();

        BindPad  (&s.buttonPause,   "ButtonPause",  "Input");
        BindPad  (&s.buttonSkip,    "ButtonSkip",   "Input");
        BindBool (&s.useSkipGameKeys,          "UseSkipGameKeys",         "Input");
        BindBool (&s.skipInPause,              "SkipInPause",             "Input");
        BindBool (&s.pauseOnlyDuringMissions,  "PauseOnlyDuringMissions", "Input");

        BindPad  (&s.buttonToggleCam, "ToggleCameraButton", "Camera");
        BindFloat(&s.camSpeed,       "InitialSpeed", "Camera", 0.05f, 3.0f);
        BindFloat(&s.camSensitivity, "Sensitivity",  "Camera", 0.002f, 0.02f);
        BindBool (&s.showCamSpeedText, "ShowSpeedNumber", "Camera");

        BindBool (&s.showInterface,      "ShowInterface",  "Interface");
        BindBool (&s.showPauseButton,    "PauseButton",    "Interface");
        BindBool (&s.showSettingsButton, "SettingsButton", "Interface");
        BindBool (&s.showGameplayButton, "GameplaySettingsButton", "Interface");
        BindStr  (s.settingsButtonLabel, sizeof(s.settingsButtonLabel), "SettingsLabelText", "Interface");
        BindFloat(&s.gameplayButtonX,  "GameplayButtonX", "Interface", 0.0f, 100.0f);
        BindFloat(&s.gameplayButtonY,  "GameplayButtonY", "Interface", 0.0f, 100.0f);
        BindStr  (s.pauseButtonLabel,  sizeof(s.pauseButtonLabel),  "PauseLabelText",  "Interface");
        BindStr  (s.resumeButtonLabel, sizeof(s.resumeButtonLabel), "ResumeLabelText", "Interface");
        BindInt  (&s.buttonCorner,    "ButtonCorner",  "Interface", 0, Corner_Count - 1);   // 0=BottomRight 1=BottomLeft 2=TopRight 3=TopLeft
        BindFloat(&s.buttonScale,     "ButtonScale",   "Interface", 0.5f, 2.0f);
        BindInt  (&s.buttonPlateAlpha,"ButtonOpacity", "Interface", 0, 255);
        BindInt  (&s.labelAlpha,      "LabelOpacity",  "Interface", 0, 255);
        BindInt  (&s.outlineAlpha,    "OutlineOpacity", "Interface", 0, 255);
        BindInt  (&s.fontStyle,       "FontStyle",     "Interface", -1, 3);
        BindInt  (&s.fontYOffset,     "FontYOffset",   "Interface", -40, 40);
        BindBool (&s.showVignette,    "Vignette",      "Interface");
        BindInt  (&s.vignetteStrength,"VignetteStrength", "Interface", 0, 255);
        BindInt  (&s.vignetteSize,    "VignetteSize",  "Interface", 0, 100);
        BindFloat(&s.buttonMarginX,   "ButtonMarginX", "Interface", 0.0f, 0.3f);
        BindFloat(&s.buttonMarginY,   "ButtonMarginY", "Interface", 0.0f, 0.3f);
        BindFloat(&s.menuScale,       "MenuScale",     "Interface", 0.7f, 2.5f);
        BindBool (&s.showPauseText,   "ShowPauseText", "Interface");
        BindStr  (s.pauseText, sizeof(s.pauseText), "PauseText", "Interface");
        BindFloat(&s.pauseTextScale,  "PauseTextScale", "Interface", 0.3f, 3.0f);
        BindBool (&s.showBlur,        "ActiveBlur",     "Interface");
        BindInt  (&s.blurAlpha,       "BlurAlpha",      "Interface", 0, 255);
        BindBool (&s.useGaussianShader, "UseGaussianShader", "Interface");
        BindBool (&s.freezeFrame,     "FreezeFrame",     "Interface");
        BindBool (&s.showMissionName, "ShowMissionName", "Interface");
        BindBool (&s.showSubtitles,   "ShowSubtitles",   "Interface");

        BindBool (&s.ffEnabled,       "HoldFastForward",           "FastForward");   // EXPERIMENTAL
        BindFloat(&s.ffSpeed,         "FastForwardSpeed",          "FastForward", 1.25f, 4.0f);
        BindPad  (&s.ffButton,        "FastForwardButton",         "FastForward");
        BindBool (&s.ffScreenButton,  "FastForwardScreenButton",   "FastForward");
        BindBool (&s.ffMuteAudio,     "FastForwardMuteAudio",      "FastForward");
        BindBool (&s.ffScripted,      "FastForwardScriptedScenes", "FastForward");
        BindBool (&s.fixAudioDesync,  "FixAudioDesync",  "Others");
        BindBool (&s.pauseGameAudio,  "PauseGameAudio",  "Others");

        BindStr  (s.pauseSoundFile,  sizeof(s.pauseSoundFile),  "PauseSound",  "Audio");
        BindStr  (s.resumeSoundFile, sizeof(s.resumeSoundFile), "ResumeSound", "Audio");

        // One-time migration for an .ini written by an older build: the buttons are now see-through by default.
        // An untouched old default plate (120) becomes 0; a value the user chose themselves is left alone.
        // (UIVersion is deliberately NOT in s_bindings, so "Reset ALL settings" never re-arms the migration.)
        if (ConfigEntry* ve = cfg->Bind("UIVersion", 0, "Interface"))
        {
            if (ve->GetInt() < 1)
            {
                if (s.buttonPlateAlpha == 120) { s.buttonPlateAlpha = 0; SettingChanged(&s.buttonPlateAlpha); }
                ve->SetInt(1);
            }
        }

        s_cfgDirty = true;      // Bind() filled in any missing keys: write a complete ini
        SaveConfigNow();
    }

    // ===========================================================================
    // Hook installation
    // ===========================================================================
    void InstallHooks()
    {
        if (!Sym::g_bCoreResolved)
        {
            logger->Info("[CutsceneCtrl] Core symbols missing -- hooks NOT installed, mod is inert.");
            return;
        }

        HOOK(CCutsceneMgr_Update, Sym::addr_CCutsceneMgr_Update);
        HOOK(CCutsceneMgr_IsCutsceneSkipButtonBeingPressed, Sym::addr_CCutsceneMgr_IsCutsceneSkipButtonBeingPressed);

        if (Sym::addr_RenderEffects) HOOK(RenderEffects, Sym::addr_RenderEffects);
        else logger->Info("[CutsceneCtrl] RenderEffects not found -- no on-screen UI (pause logic still works).");
        if (Sym::addr_Render2dStuff) HOOK(Render2dStuff, Sym::addr_Render2dStuff);
        else logger->Info("[CutsceneCtrl] Render2dStuff not found -- the UI will be drawn from RenderEffects instead.");

        if (Sym::addr_CTimer_Update) HOOK(CTimer_Update, Sym::addr_CTimer_Update);
        else logger->Info("[CutsceneCtrl] CTimer::Update not found -- relying on the cutscene-clock restore alone to freeze animation.");

        if (Sym::addr_AND_TouchEvent) HOOK(AND_TouchEvent, Sym::addr_AND_TouchEvent);
        else logger->Info("[CutsceneCtrl] AND_TouchEvent not found -- on-screen buttons can't receive touches (gamepad still works).");

        if (Sym::addr_COnscreenTimerEntry_Process) HOOK(COnscreenTimerEntry_Process, Sym::addr_COnscreenTimerEntry_Process);
        if (Sym::addr_CHud_DrawMissionTitle)       HOOK(CHud_DrawMissionTitle, Sym::addr_CHud_DrawMissionTitle);
        if (Sym::addr_CHud_DrawSubtitles)          HOOK(CHud_DrawSubtitles, Sym::addr_CHud_DrawSubtitles);

        BlurFX::Init();
        UI::Init();
        logger->Info("[CutsceneCtrl] Hooks installed.");
    }
}

// Lets other plugins / CLEO scripts ask whether a cutscene is currently paused.
extern "C" JNIEXPORT bool IsCutscenePaused()
{
    return CutsceneCtrl::g_bCutscenePaused;
}

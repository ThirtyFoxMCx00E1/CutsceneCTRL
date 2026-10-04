#pragma once
#include "GameTypes.h"
#include <cstdint>

// Mirrors the button names used by CControllerState (GameTypes.h), so the
// user can select a gamepad button from the .ini by name.
enum class GamepadButton
{
    None = 0,
    Cross, Circle, Square, Triangle,
    Start, Select,
    L1, L2, R1, R2,
    DPadUp, DPadDown, DPadLeft, DPadRight
};

GamepadButton ButtonFromString(const char* s);
bool IsButtonDown(GamepadButton btn, const CControllerState& state);

namespace CutsceneCtrl
{
    enum ButtonCorner { Corner_BottomRight = 0, Corner_BottomLeft = 1, Corner_TopRight = 2, Corner_TopLeft = 3, Corner_Count = 4 };

    // Everything editable from the floating settings panel is a plain
    // bool/int/float field here (the panel edits them in place and writes
    // them back to the .ini through the config registry in LoadConfig()).
    struct Settings
    {
        // ---- input -----------------------------------------------------
        GamepadButton buttonPause = GamepadButton::Start;
        GamepadButton buttonSkip  = GamepadButton::Cross;   // only used if useSkipGameKeys == false
        GamepadButton buttonToggleCam = GamepadButton::Select;   // (L1/R1 are move back/forward while flying, so not L1)
        bool  useSkipGameKeys = true;    // reuse the game's own skip detection
        bool  skipInPause     = true;
        bool  pauseOnlyDuringMissions = false;

        // ---- free camera ---------------------------------------------------
        float camSpeed = 0.3f;
        float camSensitivity = 0.006f;   // radians per stick unit (sticks report ~-128..128)
        bool  showCamSpeedText = true;

        // ---- on-screen pause button ---------------------------------------
        bool  showInterface = true;      // master switch for every overlay this mod draws
        bool  showPauseButton = true;
        bool  showSettingsButton = true; // the small "SET" button (during cutscenes) that opens the floating panel
        bool  showGameplayButton = true; // the "Cutscene Controller Settings" button shown when NOT in a cutscene
        char  settingsButtonLabel[40] = "Cutscene Controller Settings";
        float gameplayButtonX = 50.0f;   // CENTRE of the outside-cutscene button, % of the screen width  (0 = left edge)
        float gameplayButtonY = 4.5f;    // CENTRE of the outside-cutscene button, % of the screen height (0 = top edge)
        char  pauseButtonLabel[32]  = "PAUSE";
        char  resumeButtonLabel[32] = "RESUME";
        int   buttonCorner = Corner_BottomRight;
        float buttonScale = 1.0f;        // 1.0 = "medium big" (~12% of screen width for the text)
        int   buttonPlateAlpha = 0;      // 0-255, the plate behind the label (0 = fully transparent, only the outline + text remain)
        int   labelAlpha = 170;          // 0-255, opacity of the PAUSE / RESUME / SET label text (255 = solid white)
        int   outlineAlpha = 170;        // 0-255, opacity of the button outline (0 = no outline)
        int   fontStyle = -1;            // text of the buttons / banner: -1 = built-in Arial, else the GAME's font: 0 Gothic, 1 Subtitles, 2 Menu, 3 Pricedown
        int   fontYOffset = 0;           // game fonts only: nudge the text up (-) / down (+), in % of the text height
        bool  showVignette = true;       // darkened screen edges during cutscenes
        int   vignetteStrength = 160;    // 0-255, how dark the corners get
        int   vignetteSize = 50;         // 0-100 %, how far the darkening reaches in from the edges
        float buttonMarginX = 0.025f;    // fractions of screen width/height
        float buttonMarginY = 0.060f;
        float menuScale = 1.0f;          // size of the floating settings window (1.0 = the reference layout)

        // ---- pause screen -------------------------------------------------
        bool  showPauseText = true;      // big centred banner
        char  pauseText[64] = "PAUSED";
        float pauseTextScale = 1.0f;
        bool  showBlur = true;
        int   blurAlpha = 170;           // 0-255
        bool  useGaussianShader = false; // true = real GLES2 shader, false = simulated RW-raster blur
        bool  freezeFrame = false;       // while paused, keep drawing the pixels captured at the moment of the pause instead of the live scene (hides physics drift)
        bool  showMissionName = true;
        bool  showSubtitles   = true;
        bool  fixAudioDesync  = true;    // pause if the app comes back from the background mid-cutscene
        bool  pauseGameAudio  = true;    // silence game audio (CAudioEngine::PauseAllSounds) while paused

        // ---- EXPERIMENTAL: hold for fast-forward ----------------------------
        bool  ffEnabled = false;         // off by default
        float ffSpeed = 2.0f;            // time multiplier while held (CTimer::ms_fTimeScale x this)
        GamepadButton ffButton = GamepadButton::R2;
        bool  ffScreenButton = true;     // the on-screen "2x" hold button next to SET
        bool  ffMuteAudio = true;        // silence game audio while fast-forwarding (dialogue does not speed up)
        bool  ffScripted = false;        // also in script-driven scenes ("semi-cutscenes"), not only real cutscene files

        // ---- audio ------------------------------------------------------
        char pauseSoundFile[128]  = "";
        char resumeSoundFile[128] = "";
    };

    extern Settings g_Settings;

    // Public state queried by the exported IsCutscenePaused().
    extern bool g_bCutscenePaused;
    extern bool g_bFreeCamActive;
    // True while the 2x hold is actually speeding time up (UI highlights its button with this).
    extern bool g_bFastForwarding;

    void LoadConfig();
    void InstallHooks();

    // Called once per game logic frame (from the CCutsceneMgr::Update hook,
    // which the game calls BEFORE its own pause gate, so it keeps running
    // while paused -- verified in CGame::Process).
    void OnLogicFrame();

    // Called once per rendered frame after the game has drawn the scene
    // (from the RenderEffects hook). It prepares the frame (blur, state); the ImGui UI itself is drawn right
    // afterwards from the Render2dStuff hook so it lands on top of the game's HUD.
    void OnRenderFrame();

    // Debounced "a cutscene is playing" state, refreshed once per logic frame.
    bool IsOnAnyCutscene();
    bool IsOnScriptedCutscene();

    // ---- API used by the UI module ---------------------------------------
    // Toggle pause with all the safety guards. Returns true if it changed.
    bool TogglePause();
    // A field of g_Settings was edited in the panel: mirror it into the .ini.
    void SettingChanged(const void* field);
    void ResetAllSettings();
    void SaveConfigNow();
    // 0 = defaults (same as reset), 1 = minimal (no blur, no banner, smaller button)
    void ApplyPreset(int preset);
}

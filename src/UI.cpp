// UI.cpp -- the on-screen controls, built with Dear ImGui (real source from CLEO ImGui).
//
//   * PAUSE / RESUME button + SET button : two small borderless ImGui windows anchored to a screen corner.
//   * PAUSED banner / free-cam info      : text on the background draw list.
//   * Settings window                    : a normal ImGui window (title bar with collapse arrow + close X, drag to
//                                          move, corner grip to resize, scrollbar, checkboxes, sliders, combos...).
//
// The label of the big button is ALWAYS a non-empty string (falls back to PAUSE / RESUME when the .ini value is
// blank) and is drawn with ImGui's own text renderer through the game's RenderWare 2D pipeline.
#include "UI.h"
#include "ImGuiRW.h"
#include "GameText.h"
#include "imgui/imgui_internal.h"
#include "GameSymbols.h"
#include "CutsceneCtrl.h"
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mutex>
#include <chrono>
#include <vector>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <algorithm>

using CutsceneCtrl::g_Settings;

namespace UI
{
    // =====================================================================
    // Touch queue (producer: Java UI thread; consumer: render thread)
    // =====================================================================
    struct TouchEvent { int type, id; float x, y; };
    static constexpr int kQueueSize = 128;
    static constexpr int kMaxPointers = 10;
    static constexpr int kMaxHitRects = 8;

    static std::mutex s_mtx;
    static TouchEvent s_queue[kQueueSize];
    static int   s_qHead = 0, s_qTail = 0;
    static float s_hit[kMaxHitRects][4];              // l,t,r,b in render pixels (published by the last frame)
    static int   s_hitCount = 0;
    static bool  s_hitAll = false;                    // a popup is open: every touch belongs to us (so a tap outside closes it)
    static bool  s_active = false;
    static bool  s_touchReset = false;                // the UI went inactive with a finger possibly down: forget the gesture at the next frame
    static bool  s_ffHeld = false;                    // the on-screen 2x button is held down (render thread only)
    static bool  s_captured[kMaxPointers] = {};
    static float s_txf = 1.0f, s_tyf = 1.0f;          // touch space -> render space

    static bool HitSnapshot(float x, float y)         // caller holds s_mtx
    {
        if (s_hitAll) return true;
        for (int i = 0; i < s_hitCount; ++i)
            if (x >= s_hit[i][0] && x <= s_hit[i][2] && y >= s_hit[i][1] && y <= s_hit[i][3]) return true;
        return false;
    }

    bool PushTouchEvent(int type, int id, int x, int y)
    {
        if (id < 0 || id >= kMaxPointers) return false;
        std::lock_guard<std::mutex> lock(s_mtx);
        const float rx = (float)x * s_txf, ry = (float)y * s_tyf;
        const bool isUp = (type == 1 || type == 4);   // decoded from AND_TouchEvent: 2 = press, 1/4 = release, else move
        if (type == 2) s_captured[id] = s_active && HitSnapshot(rx, ry);
        const bool mine = s_captured[id];
        if (mine)
        {
            int next = (s_qTail + 1) % kQueueSize;
            if (next != s_qHead) { s_queue[s_qTail] = { type, id, rx, ry }; s_qTail = next; }   // full: drop, never block the UI thread
        }
        if (isUp) s_captured[id] = false;
        return mine;
    }

    void SetActive(bool active)
    {
        std::lock_guard<std::mutex> lock(s_mtx);
        if (s_active && !active)
        {
            for (int i = 0; i < kMaxPointers; ++i) s_captured[i] = false;      // never leave a touch swallowed forever
            s_touchReset = true;                                               // ...nor a half-finished gesture inside ImGui (its "mouse" would stay down)
            s_qHead = s_qTail = 0;
        }
        s_active = active;
        if (!active) { s_hitCount = 0; s_hitAll = false; s_ffHeld = false; }      // a hold never outlives the UI that shows its button
    }

    static bool PopEvent(TouchEvent& e)
    {
        std::lock_guard<std::mutex> lock(s_mtx);
        if (s_qHead == s_qTail) return false;
        e = s_queue[s_qHead]; s_qHead = (s_qHead + 1) % kQueueSize;
        return true;
    }
    static void ClearQueue() { std::lock_guard<std::mutex> lock(s_mtx); s_qHead = s_qTail = 0; }

    // =====================================================================
    // Settings table
    // =====================================================================
    enum Kind { K_HEADER, K_TEXT, K_CHECK, K_SLIDER_F, K_SLIDER_I, K_COMBO, K_BUTTON };

    struct Item
    {
        Kind kind; const char* label; void* field;
        float lo, hi; const char* fmt;
        const char* const* names; int nnames, base;
        void (*action)();
    };

    static const char* const kCornerNames[] = { "Bottom right", "Bottom left", "Top right", "Top left" };
    static const char* const kFontNames[] = { "Arial (default)", "Gothic", "Subtitles", "Menu", "Pricedown" };
    static const char* const kPadNames[] = { "Cross", "Circle", "Square", "Triangle", "Start", "Select", "L1", "L2", "R1", "R2",
                                             "D-pad up", "D-pad down", "D-pad left", "D-pad right" };
    static_assert(sizeof(GamepadButton) == sizeof(int), "combo boxes edit GamepadButton through an int*");

    static void ActMinimal() { CutsceneCtrl::ApplyPreset(1); }
    static void ActReset()   { CutsceneCtrl::ResetAllSettings(); }

    #define HEADER(txt)                       { K_HEADER, txt, nullptr, 0, 0, nullptr, nullptr, 0, 0, nullptr }
    #define TEXTB(txt)                        { K_TEXT, txt, nullptr, 0, 0, nullptr, nullptr, 0, 0, nullptr }
    #define CHECK(txt, f)                     { K_CHECK, txt, &g_Settings.f, 0, 1, nullptr, nullptr, 0, 0, nullptr }
    #define SLIDERF(txt, f, lo, hi, fmt)      { K_SLIDER_F, txt, &g_Settings.f, lo, hi, fmt, nullptr, 0, 0, nullptr }
    #define SLIDERI(txt, f, lo, hi)           { K_SLIDER_I, txt, &g_Settings.f, (float)(lo), (float)(hi), "%d", nullptr, 0, 0, nullptr }
    #define SLIDERIP(txt, f, lo, hi, fmt)     { K_SLIDER_I, txt, &g_Settings.f, (float)(lo), (float)(hi), fmt, nullptr, 0, 0, nullptr }
    #define COMBO(txt, f, names, n, base)     { K_COMBO, txt, &g_Settings.f, 0, (float)((n) - 1), nullptr, names, n, base, nullptr }
    #define BUTTON(txt, fn)                   { K_BUTTON, txt, nullptr, 0, 0, nullptr, nullptr, 0, 0, fn }

    // Every setting the .ini has, in the order of the reference window.
    static const Item kItems[] = {
        HEADER("PAUSE BUTTON"),
        CHECK (    "Show pause button",   showPauseButton),
        COMBO (    "Button corner",       buttonCorner, kCornerNames, 4, 0),
        SLIDERF(   "Button size",         buttonScale, 0.5f, 2.0f, "%.2fx"),
        SLIDERI(   "Button opacity",      buttonPlateAlpha, 0, 255),
        SLIDERI(   "Label opacity",       labelAlpha, 0, 255),
        SLIDERI(   "Outline opacity",     outlineAlpha, 0, 255),
        HEADER("TEXT"),
        COMBO (    "Font style",          fontStyle, kFontNames, 5, -1),
        SLIDERIP(  "Font vertical nudge", fontYOffset, -40, 40, "%d%%"),
        HEADER("SETTINGS BUTTON"),
        CHECK (    "Show outside cutscenes", showGameplayButton),
        SLIDERF(   "X position",          gameplayButtonX, 0.0f, 100.0f, "%.1f%%"),
        SLIDERF(   "Y position",          gameplayButtonY, 0.0f, 100.0f, "%.1f%%"),
        HEADER("VIGNETTE"),
        CHECK (    "Vignette in cutscenes", showVignette),
        SLIDERI(   "Vignette strength",   vignetteStrength, 0, 255),
        SLIDERIP(  "Vignette size",       vignetteSize, 0, 100, "%d%%"),
        HEADER("PAUSE SCREEN"),
        CHECK (    "PAUSED banner",       showPauseText),
        CHECK (    "Background blur",     showBlur),
        SLIDERI(   "Blur strength",       blurAlpha, 0, 255),
        CHECK (    "Real Gaussian blur",  useGaussianShader),
        CHECK (    "Freeze frame (hides drift)", freezeFrame),
        CHECK (    "Show subtitles",      showSubtitles),
        CHECK (    "Show mission name",   showMissionName),
        CHECK (    "Silence game audio",  pauseGameAudio),
        HEADER("CONTROLS"),
        CHECK (    "Skip while paused",   skipInPause),
        CHECK (    "Use game skip keys",  useSkipGameKeys),
        CHECK (    "Only during missions",pauseOnlyDuringMissions),
        COMBO (    "Gamepad pause",       buttonPause,     kPadNames, 14, 1),
        COMBO (    "Gamepad skip",        buttonSkip,      kPadNames, 14, 1),
        COMBO (    "Gamepad free cam",    buttonToggleCam, kPadNames, 14, 1),
        HEADER("FREE CAMERA"),
        TEXTB (    "Works only while paused. Press the free-cam button, then: right stick looks, R1 / L1 fly forward / back, D-pad up / down changes speed."),
        SLIDERF(   "Camera speed",        camSpeed, 0.05f, 3.0f, "%.2f"),
        SLIDERF(   "Look sensitivity",    camSensitivity, 0.002f, 0.02f, "%.3f"),
        CHECK (    "Show speed text",     showCamSpeedText),
        HEADER("FAST FORWARD (EXPERIMENTAL)"),
        CHECK (    "Hold for fast forward", ffEnabled),
        SLIDERF(   "Speed",               ffSpeed, 1.25f, 4.0f, "%.2fx"),
        CHECK (    "On-screen hold button", ffScreenButton),
        COMBO (    "Gamepad fast-forward", ffButton, kPadNames, 14, 1),
        CHECK (    "Silence audio while held", ffMuteAudio),
        CHECK (    "Also in scripted scenes", ffScripted),
        TEXTB (    "Hold the button to run the scene faster. Dialogue audio does not speed up, so speech lags behind the picture afterwards."),
        HEADER("MISC"),
        CHECK (    "Pause on app return", fixAudioDesync),
        SLIDERF(   "Menu size",           menuScale, 0.7f, 2.5f, "%.2fx"),
        BUTTON(    "Minimal preset (no blur / banner)", ActMinimal),
        BUTTON(    "Reset ALL settings",  ActReset),
        TEXTB (    "Changes apply instantly and are saved when you close this window. Drag the title bar to move it, the corner to resize it."),
    };
    static constexpr int kItemCount = (int)(sizeof(kItems) / sizeof(kItems[0]));

    // =====================================================================
    // State (render thread)
    // =====================================================================
    static const char* const kWinName = "Cutscene Control Settings###ccwin";
    static bool  s_winOpen = false;
    static bool  s_wasOpen = false;
    static float s_scrollDelta = 0.0f;                // pixels the finger moved since the last frame (applied inside the window)
    static float s_W = 1280, s_H = 720, s_S = 1;
    static float s_lastScale = -1.0f;
    static ImGuiStyle s_baseStyle;
    static bool  s_styleReady = false;
    static ImGuiContext* s_ctxSeen = nullptr;
    static float s_fixedDt = 0.0f;                    // tests only
    static float s_hr = 1.0f;                         // current render height / height the fonts were built for

    // frame results published for hit-testing / tests
    struct R4 { float v[4] = {}; bool ok = false; };
    static R4  s_rPause, s_rSet, s_rWin, s_rContent;
    static bool s_winCollapsed = false, s_popupOpen = false;
    static float s_scrollY = 0, s_scrollMax = 0;
    static char s_pauseLabel[40] = {};
    static bool s_gameplayMode = false;
    static R4   s_rFF;
    static R4  s_itemRect[64];
    static bool s_itemVisible[64];

    // ---- touch router (render thread): one finger drives the ImGui "mouse" ---------------------------------
    // A press that lands on the scrolling part of the settings window is held back until we know whether it is a
    // tap (-> click), a vertical swipe (-> scroll the window, never touching a value) or a horizontal drag (-> press
    // goes through, so sliders drag). Presses anywhere else (title bar, scrollbar, grip, buttons, popups) go straight in.
    enum class Route { Idle, Pending, Scroll, Forward };
    static Route s_route = Route::Idle;
    static int   s_routeId = -1;
    static float s_sx = 0, s_sy = 0, s_lx = 0, s_ly = 0;

    static bool InScrollContent(float x, float y)
    {
        if (!s_rContent.ok || s_popupOpen) return false;
        const float* r = s_rContent.v;
        return x >= r[0] && x < r[2] && y >= r[1] && y < r[3];
    }

    static void MouseTo(float x, float y)           { ImGui::GetIO().AddMousePosEvent(x, y); }
    static void MouseButton(bool down)              { ImGui::GetIO().AddMouseButtonEvent(0, down); }

    static void HandleEvent(const TouchEvent& e)
    {
        const bool isUp = (e.type == 1 || e.type == 4);
        const float thr = std::max(6.0f, 11.0f * s_S);
        if (e.type == 2)                                                // press
        {
            if (s_routeId != -1) return;                                // a second finger: swallowed, ignored
            s_routeId = e.id; s_sx = s_lx = e.x; s_sy = s_ly = e.y;
            if (InScrollContent(e.x, e.y)) s_route = Route::Pending;
            else { MouseTo(e.x, e.y); MouseButton(true); s_route = Route::Forward; }
            return;
        }
        if (e.id != s_routeId) return;
        if (!isUp)                                                      // move
        {
            if (s_route == Route::Pending)
            {
                const float dx = e.x - s_sx, dy = e.y - s_sy;
                if (std::fabs(dy) > thr && std::fabs(dy) >= std::fabs(dx))
                {
                    s_route = Route::Scroll; s_scrollDelta += dy; s_ly = e.y;      // swipe: scroll, include the distance already travelled
                }
                else if (std::fabs(dx) > thr)
                {
                    MouseTo(s_sx, s_sy); MouseButton(true); MouseTo(e.x, e.y);     // horizontal drag: let the widget under the finger have it
                    s_route = Route::Forward;
                }
            }
            else if (s_route == Route::Scroll) { s_scrollDelta += e.y - s_ly; s_ly = e.y; }
            else if (s_route == Route::Forward) MouseTo(e.x, e.y);
            return;
        }
        // release
        if (s_route == Route::Pending) { MouseTo(s_sx, s_sy); MouseButton(true); MouseButton(false); }   // a tap
        else if (s_route == Route::Forward) { MouseTo(e.x, e.y); MouseButton(false); }
        MouseTo(-FLT_MAX, -FLT_MAX);                                    // finger left the screen: clear hover
        s_route = Route::Idle; s_routeId = -1;
    }

    // =====================================================================
    // Style
    // =====================================================================
    static void BuildBaseStyle()
    {
        ImGuiStyle& st = ImGui::GetStyle();
        ImGui::StyleColorsDark();
        ImVec4* c = st.Colors;
        c[ImGuiCol_WindowBg]         = ImVec4(0.055f, 0.055f, 0.078f, 0.94f);
        c[ImGuiCol_TitleBg]          = ImVec4(0.039f, 0.039f, 0.055f, 0.98f);
        c[ImGuiCol_TitleBgActive]    = ImVec4(0.039f, 0.039f, 0.055f, 0.98f);
        c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.039f, 0.039f, 0.055f, 0.78f);
        c[ImGuiCol_Border]           = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
        st.WindowPadding     = ImVec2(10, 8);
        st.FramePadding      = ImVec2(8, 5);
        st.ItemSpacing       = ImVec2(8, 4);
        st.ItemInnerSpacing  = ImVec2(8, 4);
        st.ScrollbarSize     = 16;
        st.GrabMinSize       = 20;
        st.WindowRounding    = 0; st.FrameRounding = 2; st.GrabRounding = 2; st.ScrollbarRounding = 2;
        st.WindowBorderSize  = 1; st.FrameBorderSize = 0; st.PopupBorderSize = 1;
        st.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
        st.WindowMenuButtonPosition = ImGuiDir_Left;                   // the collapse arrow on the left, like the reference
        st.TouchExtraPadding = ImVec2(2, 2);
        st.DisplayWindowPadding = ImVec2(8, 8);
        st.DisplaySafeAreaPadding = ImVec2(0, 0);
        s_baseStyle = st;
        s_styleReady = true;
    }

    static void ApplyScale()
    {
        const float menu = std::max(0.7f, std::min(g_Settings.menuScale, 2.5f));
        const float scale = s_S * menu;
        s_hr = s_H / std::max(1.0f, ImGuiRW::BuildHeight());
        if (std::fabs(scale - s_lastScale) < 1e-4f) return;
        s_lastScale = scale;
        ImGuiStyle& st = ImGui::GetStyle();
        st = s_baseStyle;
        st.ScaleAllSizes(scale);
        st.WindowBorderSize = 1; st.PopupBorderSize = 1;
        ImGui::GetIO().FontGlobalScale = menu * s_hr;                 // fonts were rasterised for BuildHeight(); follow the screen if it changes
    }

    // =====================================================================
    // Widgets
    // =====================================================================
    static const char* LabelOrDefault(const char* v, const char* def) { return (v && *v) ? v : def; }

    // ---- the game's own fonts (CFont) -------------------------------------------------------------------------------------
    // Text in a game font is not drawn by ImGui: the widget only reserves the space, and a draw-list callback prints the string
    // with CFont at the right moment of that window's draw list (so it keeps the correct z-order against other windows).
    static bool UseGameFont() { return g_Settings.fontStyle >= 0 && GameText::Available(); }

    struct TextJob { char text[64]; int style; float x, y, lineH; uint32_t rgba; int shadow; };
    static constexpr int kMaxJobs = 8;
    static TextJob s_jobs[kMaxJobs];
    static int     s_jobCount = 0;

    static void RunTextJob(const ImDrawList*, const ImDrawCmd* cmd)
    {
        const TextJob* j = (const TextJob*)cmd->UserCallbackData;
        if (j) GameText::Draw(j->text, j->style, j->x, j->y, j->lineH, j->rgba, j->shadow);
    }
    static void QueueGameText(ImDrawList* dl, const char* text, float x, float y, float lineH, uint32_t rgba, int shadow)
    {
        if (s_jobCount >= kMaxJobs) return;
        TextJob& j = s_jobs[s_jobCount++];
        snprintf(j.text, sizeof(j.text), "%s", text);
        j.style = g_Settings.fontStyle; j.x = x; j.y = y; j.lineH = lineH; j.rgba = rgba; j.shadow = shadow;
        dl->AddCallback(RunTextJob, &j);
    }
    static float GameTextY(float top, float height, float lineH)      // top of the text for a line box [top, top + height], plus the user's nudge
    {
        return top + (height - lineH) * 0.5f + lineH * (float)g_Settings.fontYOffset * 0.01f;
    }

    // The reference UI uses a heavy display face for the big texts. We only ship the regular Arial from CLEO ImGui, so
    // the heavy look is made by drawing the same string a few sub-pixel offsets apart (still plain ImGui text).
    static void BoldText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text)
    {
        const float d = std::max(0.7f, size * 0.022f);
        dl->AddText(font, size, pos, col, text);
        dl->AddText(font, size, ImVec2(pos.x + d, pos.y), col, text);
        dl->AddText(font, size, ImVec2(pos.x, pos.y + d * 0.6f), col, text);
        dl->AddText(font, size, ImVec2(pos.x + d, pos.y + d * 0.6f), col, text);
    }

    // Draws one of the corner buttons. Returns true when it was tapped.
    //   pivot       : which point of the button sits on `anchor` ((0,0) top-left ... (1,1) bottom-right, (.5,.5) centre)
    //   plateAlpha  : 0-255 fill behind the label (0 = fully transparent)
    //   labelAlpha  : 0-255 opacity of the label text
    //   outlineAlpha: 0-255 opacity of the outline (0 = none)
    static bool CornerButton(const char* windowId, const char* label, const char* widestLabel, float fontMul, ImVec2 anchor, ImVec2 pivot,
                             bool highlight, int plateAlpha, int labelAlpha, int outlineAlpha, R4& outRect, ImVec2* outSize, bool* outHeld = nullptr)
    {
        ImFont* big = ImGuiRW::FontBig();
        const float bs = std::max(0.3f, std::min(g_Settings.buttonScale, 3.0f));
        const float fs = big->FontSize * s_hr * bs * fontMul;                   // text pixel size of this button
        const bool gf = UseGameFont();
        float tw = gf ? GameText::Measure(widestLabel, g_Settings.fontStyle, fs) : 0.0f;
        const bool useGame = gf && tw > 1.0f;                                    // a failed measurement falls back to Arial for this frame
        if (!useGame) tw = big->CalcTextSizeA(fs, FLT_MAX, 0.0f, widestLabel).x;
        const float padX = 0.45f * fs, padY = 0.20f * fs;
        const ImVec2 size(std::ceil(tw + 2 * padX), std::ceil(fs + 2 * padY));
        ImVec2 pos(anchor.x - size.x * pivot.x, anchor.y - size.y * pivot.y);
        pos.x = std::max(0.0f, std::min(pos.x, s_W - size.x));                   // a button is never (partly) off screen
        pos.y = std::max(0.0f, std::min(pos.y, s_H - size.y));
        if (outSize) *outSize = size;

        // Arial path: the label is drawn as 4 slightly offset copies (heavy look). Their alphas compound where they overlap, so each
        // copy gets the alpha that adds up to the requested opacity A inside the strokes.
        const float A  = std::max(0, std::min(labelAlpha, 255)) / 255.0f;
        const float ac = (A >= 0.999f) ? 1.0f : 1.0f - std::pow(1.0f - A, 0.25f);
        float bA = std::max(0, std::min(outlineAlpha, 255)) / 255.0f;
        if (labelAlpha <= 0 && outlineAlpha <= 0 && plateAlpha <= 0) bA = 0.25f; // nothing visible at all would lock you out of the settings: keep a faint outline

        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, std::round(2.0f * s_S)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));          // label centring never depends on "Menu size"
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        const float a = std::max(0, std::min(plateAlpha, 255)) / 255.0f;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, a));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, std::min(1.0f, a + 0.12f)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.12f, 0.12f, 0.12f, std::min(1.0f, a + 0.30f)));
        ImGui::PushStyleColor(ImGuiCol_Border,        highlight ? ImVec4(1.0f, 0.78f, 0.16f, 0.95f * bA) : ImVec4(1.0f, 1.0f, 1.0f, 0.55f * bA));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 1.0f, 1.0f, ac));
        bool tapped = false;
        ImGui::Begin(windowId, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PushFont(big);
        ImGui::SetWindowFontScale(bs * fontMul * s_hr / std::max(0.01f, ImGui::GetIO().FontGlobalScale));   // pixel size is independent of "Menu size"
        const float cfs = ImGui::GetFontSize();
        const ImVec2 cs = ImGui::GetCursorScreenPos();                          // window top-left (no padding)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        char id[96];
        if (useGame)
        {
            snprintf(id, sizeof(id), "###btn");                                 // no ImGui text at all: the label is printed by CFont below
            tapped = ImGui::Button(id, size);
            const float lw = GameText::Measure(label, g_Settings.fontStyle, fs);
            QueueGameText(dl, label, cs.x + (size.x - lw) * 0.5f, GameTextY(cs.y, size.y, fs), fs,
                          IM_COL32(255, 255, 255, (int)(A * 255.0f + 0.5f)), (int)(150.0f * A));
        }
        else
        {
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const ImVec2 tp(cs.x + (size.x - ts.x) * 0.5f, cs.y + (size.y - ts.y) * 0.5f);
            const float d = std::max(0.7f, cfs * 0.022f);
            {   // soft drop shadow so a see-through label stays readable on bright scenes (drawn first, i.e. underneath)
                const float sd = std::max(1.0f, cfs * 0.035f);
                dl->AddText(ImVec2(tp.x + sd, tp.y + sd), IM_COL32(0, 0, 0, (int)(150.0f * A)), label);
            }
            snprintf(id, sizeof(id), "%s###btn", label);                        // stable ID while the label flips
            tapped = ImGui::Button(id, size);                                   // draws the plate, the outline and the first label copy
            const ImU32 col = IM_COL32(255, 255, 255, (int)(ac * 255.0f + 0.5f));
            dl->AddText(ImVec2(tp.x + d, tp.y), col, label);                    // heavier strokes: three more copies a fraction of a pixel away
            dl->AddText(ImVec2(tp.x, tp.y + d * 0.6f), col, label);
            dl->AddText(ImVec2(tp.x + d, tp.y + d * 0.6f), col, label);
        }
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        if (outHeld) *outHeld = ImGui::IsItemActive();                          // finger down on the button right now
        outRect.v[0] = mn.x; outRect.v[1] = mn.y; outRect.v[2] = mx.x; outRect.v[3] = mx.y; outRect.ok = true;
        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(7);
        return tapped;
    }

    static void DrawBanner(const char* text)
    {
        ImFont* big = ImGuiRW::FontBig();
        const float fs = big->FontSize * s_hr * 2.2f * std::max(0.3f, std::min(g_Settings.pauseTextScale, 3.0f));
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (UseGameFont())
        {
            const float w = GameText::Measure(text, g_Settings.fontStyle, fs);
            if (w > 1.0f)
            {
                QueueGameText(dl, text, std::floor((s_W - w) * 0.5f), GameTextY(0.15f * s_H, fs, fs), fs, IM_COL32(255, 255, 255, 255), 215);
                return;
            }
        }
        const ImVec2 ts = big->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
        const ImVec2 pos(std::floor((s_W - ts.x) * 0.5f), std::floor(0.15f * s_H));
        const float sh = std::max(2.0f, fs * 0.045f);
        BoldText(dl, big, fs, ImVec2(pos.x + sh, pos.y + sh), IM_COL32(0, 0, 0, 215), text);
        BoldText(dl, big, fs, pos, IM_COL32(255, 255, 255, 255), text);
    }

    static void DrawInfo(const char* text)
    {
        ImFont* f = ImGuiRW::FontUI();
        const float fs = f->FontSize * s_hr * 1.15f * std::max(0.7f, std::min(g_Settings.menuScale, 2.5f));
        const ImVec2 pos(0.02f * s_W, 0.90f * s_H);
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (UseGameFont() && GameText::Measure(text, g_Settings.fontStyle, fs) > 1.0f)
        {
            QueueGameText(dl, text, pos.x, GameTextY(pos.y, fs, fs), fs, IM_COL32(255, 255, 255, 255), 220);
            return;
        }
        dl->AddText(f, fs, ImVec2(pos.x + 2, pos.y + 2), IM_COL32(0, 0, 0, 220), text);
        dl->AddText(f, fs, pos, IM_COL32(255, 255, 255, 255), text);
    }

    // Darkened screen edges: a grid mesh over the whole screen whose vertex alpha follows an elliptical radius (0 centre,
    // 1 edge middles, ~1.41 corners). Black, alpha = strength * smoothstep((r - inner) / (outer - inner)); "size" moves `inner`.
    static void DrawVignette()
    {
        const float strength = std::max(0, std::min(g_Settings.vignetteStrength, 255)) / 255.0f;
        if (strength <= 0.0f) return;
        const float size = std::max(0, std::min(g_Settings.vignetteSize, 100)) * 0.01f;
        const float ri = 1.15f - 0.95f * size, ro = 1.5f;
        constexpr int NX = 40, NY = 22;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        dl->PrimReserve(NX * NY * 6, (NX + 1) * (NY + 1));
        const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
        for (int j = 0; j <= NY; ++j)
            for (int i = 0; i <= NX; ++i)
            {
                const float nx = 2.0f * i / NX - 1.0f, ny = 2.0f * j / NY - 1.0f;
                const float r = std::sqrt(nx * nx + ny * ny);
                float t = (r - ri) / (ro - ri); t = std::max(0.0f, std::min(t, 1.0f));
                const float sm = t * t * (3.0f - 2.0f * t);
                dl->PrimWriteVtx(ImVec2(s_W * i / NX, s_H * j / NY), uv, IM_COL32(0, 0, 0, (int)(strength * sm * 255.0f + 0.5f)));
            }
        for (int j = 0; j < NY; ++j)
            for (int i = 0; i < NX; ++i)
            {
                const ImDrawIdx v00 = (ImDrawIdx)(base + j * (NX + 1) + i), v10 = (ImDrawIdx)(v00 + 1);
                const ImDrawIdx v01 = (ImDrawIdx)(v00 + NX + 1), v11 = (ImDrawIdx)(v01 + 1);
                dl->PrimWriteIdx(v00); dl->PrimWriteIdx(v10); dl->PrimWriteIdx(v11);
                dl->PrimWriteIdx(v00); dl->PrimWriteIdx(v11); dl->PrimWriteIdx(v01);
            }
    }

    // Item `i` of the table. Returns nothing; edits g_Settings in place and tells CutsceneCtrl what changed.
    static void DrawItem(int i)
    {
        const Item& it = kItems[i];
        ImGui::PushID(i);
        bool changed = false;
        const float frameW = ImGui::CalcItemWidth();                    // width of the slider / combo frame; the label sits to its right
        switch (it.kind)
        {
            case K_HEADER:
                ImGui::SeparatorText(it.label);
                break;
            case K_TEXT:
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", it.label);
                ImGui::PopStyleColor();
                break;
            case K_CHECK:
                changed = ImGui::Checkbox(it.label, (bool*)it.field);
                break;
            case K_SLIDER_F:
                changed = ImGui::SliderFloat(it.label, (float*)it.field, it.lo, it.hi, it.fmt, ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
                break;
            case K_SLIDER_I:
                changed = ImGui::SliderInt(it.label, (int*)it.field, (int)it.lo, (int)it.hi, it.fmt, ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
                break;
            case K_COMBO:
            {
                int idx = *(int*)it.field - it.base;
                idx = std::max(0, std::min(idx, it.nnames - 1));
                if (ImGui::Combo(it.label, &idx, it.names, it.nnames, it.nnames))
                {
                    *(int*)it.field = idx + it.base;
                    changed = true;
                }
                break;
            }
            case K_BUTTON:
                if (ImGui::Button(it.label, ImVec2(-FLT_MIN, 0.0f)) && it.action) it.action();
                break;
        }
        if (i < 64)
        {
            const ImVec2 mn = ImGui::GetItemRectMin(); ImVec2 mx = ImGui::GetItemRectMax();
            if (it.kind == K_SLIDER_F || it.kind == K_SLIDER_I || it.kind == K_COMBO) mx.x = std::min(mx.x, mn.x + frameW);
            s_itemRect[i].v[0] = mn.x; s_itemRect[i].v[1] = mn.y; s_itemRect[i].v[2] = mx.x; s_itemRect[i].v[3] = mx.y;
            s_itemRect[i].ok = true; s_itemVisible[i] = ImGui::IsItemVisible();
        }
        ImGui::PopID();
        if (changed && it.field) CutsceneCtrl::SettingChanged(it.field);
    }

    static void DrawSettingsWindow()
    {
        s_rWin.ok = s_rContent.ok = false; s_winCollapsed = false; s_scrollY = s_scrollMax = 0;
        for (int i = 0; i < 64; ++i) s_itemRect[i].ok = false;
        if (!s_winOpen) { if (s_wasOpen) CutsceneCtrl::SaveConfigNow(); s_wasOpen = false; return; }
        const bool justOpened = !s_wasOpen;
        s_wasOpen = true;

        // Opened from the gameplay button: keep the window off that button (it sits in a screen corner, the window's default
        // spot is the top-left), by using the free band above / below it.
        ImVec2 defPos(0.03f * s_W, 0.04f * s_H), defSize(0.375f * s_W, 0.90f * s_H);
        const bool avoid = s_gameplayMode && s_rSet.ok;
        const float gapY = 0.012f * s_H;
        float bandTop = 0.0f, bandBottom = s_H;                           // the vertical band the window may use
        if (avoid)
        {
            if ((s_rSet.v[1] + s_rSet.v[3]) * 0.5f < s_H * 0.5f) bandTop = s_rSet.v[3] + gapY; else bandBottom = s_rSet.v[1] - gapY;
            defPos.y = std::max(defPos.y, bandTop);
            defSize.y = std::min(defSize.y, std::max(0.25f * s_H, bandBottom - defPos.y - (bandBottom >= s_H ? 0.02f * s_H : 0.0f)));
        }
        ImGui::SetNextWindowPos(defPos, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(defSize, ImGuiCond_FirstUseEver);
        // The window can be dragged anywhere, but never so far that its title bar (the only handle to move / close it) is lost:
        // the whole title bar stays on screen vertically, and at least ~4 title-bar heights of it stay on screen horizontally.
        if (ImGuiWindow* ex = ImGui::FindWindowByName(kWinName))
        {
            const float tbH = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
            const float keep = std::min(ex->Size.x, 4.5f * tbH);          // enough title text left to grab without hitting the X
            ImVec2 p = ex->Pos;
            p.x = std::max(keep - ex->Size.x, std::min(p.x, s_W - keep));
            p.y = std::max(0.0f, std::min(p.y, s_H - tbH));
            // re-opened from the gameplay button on top of that button? slide it into the free band (size shrinks to fit, never grows)
            if (avoid && justOpened)
            {
                const bool overlapsX = ex->Pos.x < s_rSet.v[2] && ex->Pos.x + ex->Size.x > s_rSet.v[0];
                const bool overlapsY = ex->Pos.y < s_rSet.v[3] + gapY && ex->Pos.y + ex->Size.y > s_rSet.v[1] - gapY;
                if (overlapsX && overlapsY)
                {
                    p.y = std::max(bandTop, std::min(p.y, bandBottom - 0.25f * s_H));
                    if (p.y < bandTop) p.y = bandTop;
                    const float maxH = std::max(0.25f * s_H, bandBottom - p.y);
                    if (ex->Size.y > maxH) ImGui::SetNextWindowSize(ImVec2(ex->Size.x, maxH), ImGuiCond_Always);
                }
            }
            if (p.x != ex->Pos.x || p.y != ex->Pos.y) ImGui::SetNextWindowPos(p, ImGuiCond_Always);
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.22f * s_W, 0.20f * s_H), ImVec2(s_W, s_H));
        const bool visible = ImGui::Begin(kWinName, &s_winOpen, ImGuiWindowFlags_NoSavedSettings);
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        s_winCollapsed = w->Collapsed;
        if (visible)
        {
            if (s_scrollDelta != 0.0f) { ImGui::SetScrollY(ImGui::GetScrollY() - s_scrollDelta); s_scrollDelta = 0.0f; }
            for (int i = 0; i < kItemCount; ++i) DrawItem(i);
            s_scrollY = ImGui::GetScrollY(); s_scrollMax = ImGui::GetScrollMaxY();
        }
        else s_scrollDelta = 0.0f;
        // Finger-scroll region for the NEXT frame's touches: the window body without scrollbar, title bar and the corner grip.
        if (visible && !w->Collapsed)
        {
            const float grip = ImGui::GetFontSize() * 1.6f;
            s_rContent.v[0] = w->InnerRect.Min.x; s_rContent.v[1] = w->InnerRect.Min.y;
            s_rContent.v[2] = w->InnerRect.Max.x; s_rContent.v[3] = w->InnerRect.Max.y - grip;
            s_rContent.ok = true;
        }
        ImGui::End();
        if (s_wasOpen && !s_winOpen) { CutsceneCtrl::SaveConfigNow(); s_wasOpen = false; }      // closed with the X
    }

    // Rectangles that own touches next frame: our windows and popups (a combo's list can stick out of the window).
    static void PublishHitRects()
    {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        float rects[kMaxHitRects][4]; int n = 0;
        for (ImGuiWindow* w : g.Windows)
        {
            if (!w->Active) continue;                                   // only what was actually submitted this frame owns touches
            if (w->Hidden || (w->Flags & ImGuiWindowFlags_ChildWindow)) continue;
            const bool ours = (strstr(w->Name, "##cc") != nullptr) || (w->Flags & ImGuiWindowFlags_Popup);
            if (!ours || n >= kMaxHitRects) continue;
            rects[n][0] = w->Pos.x; rects[n][1] = w->Pos.y; rects[n][2] = w->Pos.x + w->Size.x; rects[n][3] = w->Pos.y + w->Size.y; ++n;
        }
        s_popupOpen = g.OpenPopupStack.Size > 0;
        std::lock_guard<std::mutex> lock(s_mtx);
        memcpy(s_hit, rects, sizeof(float) * 4 * n);
        s_hitCount = n;
        s_hitAll = s_popupOpen;
    }

    // =====================================================================
    // Public
    // =====================================================================
    void Init() {}

    bool IsSettingsOpen() { return s_winOpen; }
    bool FastForwardHeld() { return s_ffHeld; }

    void Shutdown() { ImGuiRW::Shutdown(); s_ctxSeen = nullptr; s_styleReady = false; s_lastScale = -1.0f; }

    void OnCutsceneEnded()
    {
        if (s_winOpen || s_wasOpen) { s_winOpen = false; s_wasOpen = false; CutsceneCtrl::SaveConfigNow(); }
        s_route = Route::Idle; s_routeId = -1; s_scrollDelta = 0; s_ffHeld = false;
        s_rContent.ok = false; s_popupOpen = false;
        ClearQueue();
        { std::lock_guard<std::mutex> lock(s_mtx); s_hitAll = false; }
        if (ImGuiRW::IsReady()) { ImGui::GetIO().AddMouseButtonEvent(0, false); ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX); }
    }

    void Frame(const FrameInfo& fi)
    {
        int rw = 0, rh = 0;
        if (Sym::RsGlobal) { rw = Sym::RsGlobal->maximumWidth; rh = Sym::RsGlobal->maximumHeight; }
        int ww = Sym::OS_ScreenGetWidth ? Sym::OS_ScreenGetWidth() : 0, wh = Sym::OS_ScreenGetHeight ? Sym::OS_ScreenGetHeight() : 0;
        if (ww <= 0 || wh <= 0) aml->GetDisplaySize(&ww, &wh);
        if (rw <= 0 || rh <= 0) { rw = ww; rh = wh; }
        if (rw <= 0 || rh <= 0) return;
        s_W = (float)rw; s_H = (float)rh; s_S = s_H / 720.0f;
        {
            std::lock_guard<std::mutex> lock(s_mtx);                       // touches arrive in window pixels
            s_txf = (ww > 0) ? s_W / (float)ww : 1.0f;
            s_tyf = (wh > 0) ? s_H / (float)wh : 1.0f;
        }

        if (!ImGuiRW::Init(s_H)) return;
        static std::chrono::steady_clock::time_point s_prev = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        float dt = s_fixedDt > 0.0f ? s_fixedDt : std::chrono::duration<float>(now - s_prev).count();
        s_prev = now;
        if (dt > 0.25f) dt = 0.25f;
        if (!ImGuiRW::NewFrame(s_W, s_H, dt)) return;

        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            logger->Info("[CutsceneCtrl] UI: ImGui render size %dx%d, window %dx%d (touch->render scale %.3f,%.3f)", rw, rh, ww, wh, s_txf, s_tyf);
        }

        if (ImGui::GetCurrentContext() != s_ctxSeen) { s_ctxSeen = ImGui::GetCurrentContext(); s_styleReady = false; s_lastScale = -1.0f; }
        if (!s_styleReady) BuildBaseStyle();
        ApplyScale();

        {
            bool reset; { std::lock_guard<std::mutex> lock(s_mtx); reset = s_touchReset; s_touchReset = false; }
            if (reset)
            {   // the UI was switched off while a finger was down on it: its release will never arrive (it was no longer ours)
                s_route = Route::Idle; s_routeId = -1; s_scrollDelta = 0;
                ImGui::GetIO().AddMouseButtonEvent(0, false); ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            }
        }
        TouchEvent e;
        while (PopEvent(e)) HandleEvent(e);

        s_jobCount = 0;
        ImGui::NewFrame();

        if (fi.vignette) DrawVignette();                 // behind everything else we draw

        s_rPause.ok = s_rSet.ok = false;
        s_rFF.ok = false;
        s_ffHeld = false;                                // set again below only if the 2x button is drawn and held
        s_pauseLabel[0] = 0;
        s_gameplayMode = !fi.inCutscene;
        if (!fi.drawUI)
        {
            s_rContent.ok = false; s_rWin.ok = false; s_popupOpen = false;
        }
        else
        {
        if (fi.info && *fi.info)                         DrawInfo(fi.info);
        if (fi.paused && fi.banner && *fi.banner)        DrawBanner(fi.banner);

        // ---- the corner buttons ---------------------------------------------------------------------------
        const char* pauseLbl  = LabelOrDefault(g_Settings.pauseButtonLabel,  "PAUSE");
        const char* resumeLbl = LabelOrDefault(g_Settings.resumeButtonLabel, "RESUME");
        const char* shown = fi.paused ? resumeLbl : pauseLbl;
        snprintf(s_pauseLabel, sizeof(s_pauseLabel), "%s", fi.inCutscene ? shown : "");

        if (fi.inCutscene)
        {
            const int corner = std::max(0, std::min(g_Settings.buttonCorner, CutsceneCtrl::Corner_Count - 1));
            const bool right  = (corner == CutsceneCtrl::Corner_BottomRight || corner == CutsceneCtrl::Corner_TopRight);
            const bool bottom = (corner == CutsceneCtrl::Corner_BottomRight || corner == CutsceneCtrl::Corner_BottomLeft);
            const float mx = g_Settings.buttonMarginX * s_W, my = g_Settings.buttonMarginY * s_H;
            const ImVec2 anchor(right ? s_W - mx : mx, bottom ? s_H - my : my);
            const ImVec2 pivot(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f);

            // the button is as wide as the longer of the two labels, so it doesn't jump when it flips
            char widest[64]; snprintf(widest, sizeof(widest), "%s", strlen(pauseLbl) >= strlen(resumeLbl) ? pauseLbl : resumeLbl);

            // Laid out from the corner inwards:  [PAUSE] [SET] [2x]  (mirrored for the left corners).
            const float gap = 0.012f * s_W;
            const float dir = right ? -1.0f : 1.0f;
            ImVec2 pauseSize(0, 0), setSize(0, 0);
            float nextX = anchor.x;
            if (g_Settings.showPauseButton)
            {
                if (CornerButton("##ccpause", shown, widest, 1.0f, anchor, pivot, fi.paused, g_Settings.buttonPlateAlpha, g_Settings.labelAlpha, g_Settings.outlineAlpha, s_rPause, &pauseSize))
                    CutsceneCtrl::TogglePause();
                nextX += dir * (pauseSize.x + gap);
            }
            // the small buttons are centred vertically against the big one
            ImFont* big = ImGuiRW::FontBig();
            const float bs = std::max(0.3f, std::min(g_Settings.buttonScale, 3.0f));
            const float bigH = std::ceil(1.4f * big->FontSize * s_hr * bs);
            const float smallH = std::ceil(1.4f * 0.5f * big->FontSize * s_hr * bs);
            ImVec2 smallAnchor(nextX, anchor.y);
            if (g_Settings.showPauseButton) smallAnchor.y += bottom ? -(bigH - smallH) * 0.5f : (bigH - smallH) * 0.5f;
            if (g_Settings.showSettingsButton)
            {
                if (CornerButton("##ccset", "SET", "SET", 0.5f, smallAnchor, pivot, s_winOpen, g_Settings.buttonPlateAlpha, g_Settings.labelAlpha, g_Settings.outlineAlpha, s_rSet, &setSize))
                    s_winOpen = !s_winOpen;
                smallAnchor.x += dir * (setSize.x + gap);
            }
            // EXPERIMENTAL hold-for-fast-forward button: held = running at the chosen speed, released = normal
            s_rFF.ok = false;
            bool held = false;
            if (fi.ffButton && !fi.paused)
            {
                char ffLbl[16]; snprintf(ffLbl, sizeof(ffLbl), "%gx", std::max(1.0f, std::min(g_Settings.ffSpeed, 4.0f)));
                CornerButton("##ccff", ffLbl, ffLbl, 0.5f, smallAnchor, pivot, CutsceneCtrl::g_bFastForwarding, g_Settings.buttonPlateAlpha,
                             g_Settings.labelAlpha, g_Settings.outlineAlpha, s_rFF, nullptr, &held);
            }
            s_ffHeld = held;
        }
        else if (g_Settings.showGameplayButton)
        {
            // Outside a cutscene: one smaller button with the full name, centred on the X / Y the user chose (percent of the screen).
            const float cx = std::max(0.0f, std::min(g_Settings.gameplayButtonX, 100.0f)) * 0.01f * s_W;
            const float cy = std::max(0.0f, std::min(g_Settings.gameplayButtonY, 100.0f)) * 0.01f * s_H;
            const char* lbl = LabelOrDefault(g_Settings.settingsButtonLabel, "Cutscene Controller Settings");
            if (CornerButton("##ccset", lbl, lbl, 0.42f, ImVec2(cx, cy), ImVec2(0.5f, 0.5f), s_winOpen, g_Settings.buttonPlateAlpha, g_Settings.labelAlpha, g_Settings.outlineAlpha, s_rSet, nullptr))
                s_winOpen = !s_winOpen;
        }

        DrawSettingsWindow();
        }
        PublishHitRects();

        ImGui::Render();
        ImGuiRW::RenderDrawData(ImGui::GetDrawData());
    }

    // ---- test introspection -----------------------------------------------------------------------------
    DebugInfo GetDebug()
    {
        DebugInfo d;
        d.uiReady = ImGuiRW::IsReady();
        d.pauseShown = s_rPause.ok; d.setShown = s_rSet.ok; d.winOpen = s_winOpen; d.winCollapsed = s_winCollapsed; d.popupOpen = s_popupOpen; d.gameplay = s_gameplayMode;
        memcpy(d.pause, s_rPause.v, sizeof(d.pause)); memcpy(d.set, s_rSet.v, sizeof(d.set)); d.ffShown = s_rFF.ok; memcpy(d.ff, s_rFF.v, sizeof(d.ff));
        memcpy(d.content, s_rContent.v, sizeof(d.content));
        d.scrollY = s_scrollY; d.scrollMax = s_scrollMax; d.scale = s_S;
        snprintf(d.pauseLabel, sizeof(d.pauseLabel), "%s", s_pauseLabel);
        d.itemCount = kItemCount;
        if (s_winOpen && ImGui::GetCurrentContext())
            if (ImGuiWindow* w = ImGui::FindWindowByName(kWinName))
            { d.win[0] = w->Pos.x; d.win[1] = w->Pos.y; d.win[2] = w->Pos.x + w->Size.x; d.win[3] = w->Pos.y + w->Size.y; }
        return d;
    }
    bool DebugItemRect(int i, float o[4])
    {
        if (i < 0 || i >= 64 || i >= kItemCount || !s_itemRect[i].ok || !s_itemVisible[i]) return false;
        memcpy(o, s_itemRect[i].v, sizeof(float) * 4); return true;
    }
    const char* DebugItemLabel(int i) { return (i >= 0 && i < kItemCount) ? kItems[i].label : ""; }
    int   DebugItemCount() { return kItemCount; }
    void  DebugSetFixedDt(float s) { s_fixedDt = s; }
}

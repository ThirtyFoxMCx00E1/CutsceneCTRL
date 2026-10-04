#pragma once
// UI -- the on-screen PAUSE / RESUME button, the small SET button, the big PAUSED banner and the floating
// settings window, all built with Dear ImGui (the real source from CLEO ImGui, see ImGuiRW.h) and drawn through
// the game's own RenderWare renderer right after its 2D pass.
//
// Touch input comes from the game's low-level AND_TouchEvent(type, id, x, y). That runs on the Java UI thread,
// so PushTouchEvent only appends to a small mutex-protected queue and decides, from the rectangles published by
// the previous frame, whether the touch belongs to our UI (and so must NOT reach the game). Every ImGui call
// happens on the render thread inside Frame().
namespace UI
{
    void Init();
    void Shutdown();

    // Java UI thread. Returns true if the touch belongs to our UI and must NOT be forwarded to the game.
    bool PushTouchEvent(int type, int pointerId, int x, int y);

    // Render thread: whether our UI is on screen and may capture touches.
    void SetActive(bool active);

    struct FrameInfo
    {
        bool drawUI = true;             // buttons / banner / settings window (false: only the vignette is wanted this frame)
        bool vignette = false;          // darkened screen edges (cutscenes only; strength / size from the settings)
        bool ffButton = false;          // show the on-screen "2x" hold button (the 2x hold is enabled and currently possible)
        bool inCutscene = true;         // false = gameplay: only the "Cutscene Controller Settings" button (+ the window if open)
        bool paused = false;
        const char* banner = nullptr;   // big centred text while paused (null = none)
        const char* info = nullptr;     // small line at the bottom-left (null = none)
    };

    // Render thread, once per rendered frame while active (called right after the game's 2D pass): feeds the queued
    // touches to ImGui, builds the widgets and submits the draw lists to the game's renderer.
    void Frame(const FrameInfo& fi);

    // Cutscene ended: close the window (saving settings), drop in-flight touches.
    void OnCutsceneEnded();

    // Render thread: is the floating settings window currently open?
    bool IsSettingsOpen();

    // Render thread: is the on-screen "2x" button being held down right now?
    bool FastForwardHeld();

    // ---- introspection for the host-side tests ----------------------------------------------------------
    struct DebugInfo
    {
        bool  uiReady = false;
        bool  pauseShown = false, setShown = false, winOpen = false, winCollapsed = false, popupOpen = false, gameplay = false;
        bool  ffShown = false;
        float ff[4] = {};                                                   // the 2x button
        float pause[4] = {}, set[4] = {}, win[4] = {}, content[4] = {};     // l,t,r,b in render pixels
        float scrollY = 0, scrollMax = 0, scale = 1;
        char  pauseLabel[40] = {};                                          // exactly what was handed to ImGui::Button
        int   itemCount = 0;
    };
    DebugInfo GetDebug();
    // Screen rectangle of settings row `index` (the order of the table in UI.cpp); false if scrolled out / hidden.
    bool  DebugItemRect(int index, float out[4]);
    const char* DebugItemLabel(int index);
    int   DebugItemCount();
    // Tests: use a fixed frame time (seconds) instead of the real clock, so ImGui's double-click timing is deterministic. 0 = real clock.
    void  DebugSetFixedDt(float seconds);
}

#pragma once
// ImGuiRW -- Dear ImGui (the real 1.89.7 source from CLEO ImGui, vendored in src/imgui/) drawn through the
// game's own RenderWare 2D immediate-mode renderer, exactly like the CLEO ImGui plugin does.
//
// Why this instead of the old home-made OpenGL ES overlay: the game's own renderer draws into the right
// framebuffer, at the right moment, with the game's own render-state bookkeeping, so nothing can desync.
// The old GL overlay is what silently failed on real phones (no button, no PAUSE / RESUME text).
//
// Threading: everything here runs on the render thread only.
#include "imgui/imgui.h"

namespace ImGuiRW
{
    // Creates the ImGui context and builds the fonts on the CPU. Safe to call repeatedly (no-op once ready).
    // `displayH` is the render height in pixels, used to pick font pixel sizes.
    bool Init(float displayH);
    bool IsReady();

    // Call once per frame before ImGui::NewFrame(). Uploads the font atlas to a RenderWare raster the first
    // time, and fills io.DisplaySize / io.DeltaTime. Returns false if the font texture could not be created.
    bool NewFrame(float displayW, float displayH, float deltaSeconds);

    // Submit ImGui::GetDrawData() to the game's renderer.
    void RenderDrawData(ImDrawData* drawData);

    void Shutdown();
    // Tests: forget a previous permanent failure so Init() may be tried again.
    void DebugClearFailure();

    // Fonts built by Init(): the UI font, and a large heavy one for the PAUSE / RESUME button and the banner.
    ImFont* FontUI();
    ImFont* FontBig();
    // Render height (pixels) the fonts were rasterised for; UI scales by displayH / BuildHeight() if the screen size changes.
    float   BuildHeight();
}

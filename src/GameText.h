#pragma once
// GameText -- draws a short label with one of the GAME's own fonts (CFont) at an exact pixel position / size, so the PAUSE /
// RESUME / SET / settings-button labels and the PAUSED banner can use the same typefaces as the game's HUD.
//
//   FontStyle (the game's CFont style ids):  0 = Gothic   1 = Subtitles   2 = Menu   3 = Pricedown
//
// Coordinates are render pixels (CFont culls against RsGlobal.maximumWidth/Height, verified in PrintChar's disassembly).
// Everything here runs on the render thread. Every CFont call this module makes is on symbols that exist in both the
// armeabi-v7a and arm64-v8a libGTASA.so; if any is missing Available() is false and the UI keeps using ImGui's Arial.
#include <cstdint>

namespace GameText
{
    // True when every CFont function (and the two state blocks) resolved.
    bool Available();

    // Width in pixels of `text` when the font's LINE HEIGHT (CFont::GetHeight) is `lineH` pixels.
    float Measure(const char* text, int style, float lineH);

    // Prints `text` with its top-left at (x, y). `rgba` is 0xAABBGGRR (ImGui's IM_COL32 layout). A black drop shadow with
    // alpha `shadowAlpha` (0-255) is printed 1-2 px behind it. The CFont state is restored afterwards.
    void Draw(const char* text, int style, float x, float y, float lineH, uint32_t rgba, int shadowAlpha);
}

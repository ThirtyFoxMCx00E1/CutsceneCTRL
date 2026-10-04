#pragma once
#include <cstdint>
// BlurFX -- two independent implementations of the pause-screen blur:
//
//  * Simulated (default): a classic downsample/upsample blur done entirely
//    through RenderWare raster calls (RwRasterCreate/RenderFast + a
//    CSprite2d draw). No shaders, no raw GL calls -- works on literally
//    any GPU the game itself already runs on. This is what
//    "ActiveBlur"/"simulate Gaussian blur" refers to.
//
//  * Real (opt-in, "UseGaussianShader"): captures the frame into a GLES2
//    texture and runs an actual 9-tap Gaussian fragment shader over it.
//    Sharper and more correct, at the cost of needing a working GLES2
//    context (which the game already has, since we render from inside
//    its own render hook) and slightly more GPU work.
namespace BlurFX
{
    void Init();
    void Shutdown();

    // Called at RENDER time on the first paused frame, after the game has
    // drawn the scene and before our own overlay is drawn, so the captured
    // image doesn't contain our own buttons.
    void CaptureNow(bool useGaussianShader);

    // Called every frame the pause screen is showing; draws the captured
    // blur across the whole screen with the given alpha (0-255).
    void DrawFullscreen(uint8_t alpha, bool useGaussianShader);

    // ---- Freeze frame ---------------------------------------------------------------------------------
    // A full-resolution, un-blurred copy of the front buffer taken at the moment the pause begins, redrawn opaque over the live
    // scene every paused frame, so whatever the game keeps moving underneath (vanilla physics / animation drift) stays hidden.
    // Same RenderWare calls as the simulated blur, just at 1:1 size. A failed capture leaves the live scene visible
    // (HasFrozen() stays false and nothing is retried until InvalidateFrozen()).
    bool CaptureFrozen();
    void InvalidateFrozen();       // a new pause begins: forget the old picture
    bool HasFrozen();
    void DrawFrozen();             // opaque, full screen
}

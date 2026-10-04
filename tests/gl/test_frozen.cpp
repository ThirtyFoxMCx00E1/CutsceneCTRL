// Freeze frame (BlurFX::CaptureFrozen / DrawFrozen) against a fake RenderWare: checks WHICH calls are made with WHAT arguments
// (full-size raster, 1:1 copy of the front buffer, opaque full-screen draw of that raster's texture), the failure paths and the
// resource lifetime. It cannot show what the real RenderWare does with those calls.
#include <mod/logger.h>
#include <mod/amlmod.h>
FakeLogger fl; FakeLogger* logger = &fl; FakeAML fa; FakeAML* aml = &fa;
#include "BlurFX.h"
#include "GameSymbols.h"
#include "GameTypes.h"
#include <vector>
#include <cstring>
#include <cstdio>
#define CHECK(c) do{ if(!(c)){ printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails; } else ++passes; }while(0)
static int fails = 0, passes = 0;

struct FakeRaster { int w, h, depth, flags; };
struct FakeTexture { FakeRaster* r; };
static int rastersAlive = 0, texturesAlive = 0, spritesAlive = 0, rasterCreates = 0, drawCalls = 0, renderFasts = 0, pushes = 0, pops = 0;
static FakeRaster* ctx = nullptr; static FakeRaster* copiedInto = nullptr; static void* copiedFrom = nullptr; static int copyX = -1, copyY = -1;
static int createW = 0, createH = 0, createFlags = 0, createDepth = 0;
static bool failRaster = false, failTexture = false;
static float lastRect[4]; static uint8_t lastColor[4]; static void* lastSpriteTexture = nullptr;
static char frontBuffer[16];

static void* f_RasterCreate(int32_t w, int32_t h, int32_t d, int32_t f) { ++rasterCreates; if (failRaster) return nullptr; ++rastersAlive; createW = w; createH = h; createDepth = d; createFlags = f; return new FakeRaster{ w, h, d, f }; }
static void  f_RasterDestroy(void* r) { delete (FakeRaster*)r; --rastersAlive; }
static void* f_TextureCreate(void* r) { if (failTexture) return nullptr; ++texturesAlive; return new FakeTexture{ (FakeRaster*)r }; }
static void  f_TextureDestroy(void* t) { delete (FakeTexture*)t; --texturesAlive; }
static void  f_Push(void* r) { ++pushes; ctx = (FakeRaster*)r; }
static void  f_Pop() { ++pops; ctx = nullptr; }
static void  f_RenderFast(void* src, int x, int y) { ++renderFasts; copiedInto = ctx; copiedFrom = src; copyX = x; copyY = y; }
static void  f_SpriteCtor(void* s) { ++spritesAlive; memset(s, 0, 8); }
static void  f_SpriteDtor(void* s) { (void)s; --spritesAlive; }
static void  f_SpriteDraw(void* s, const CRect& r, const CRGBA& c) { ++drawCalls; lastSpriteTexture = *(void**)s; lastRect[0] = r.left; lastRect[1] = r.bottom; lastRect[2] = r.right; lastRect[3] = r.top; lastColor[0] = c.r; lastColor[1] = c.g; lastColor[2] = c.b; lastColor[3] = c.a; }

int main()
{
    void* fb = frontBuffer; Sym::CPostEffects_pRasterFrontBuffer = &fb;
    Sym::RwRasterCreate = f_RasterCreate; Sym::RwRasterDestroy = f_RasterDestroy; Sym::RwTextureCreate = f_TextureCreate; Sym::RwTextureDestroy = f_TextureDestroy;
    Sym::RwRasterPushContext = f_Push; Sym::RwRasterPopContext = f_Pop; Sym::RwRasterRenderFast = f_RenderFast;
    Sym::CSprite2d_Ctor = f_SpriteCtor; Sym::CSprite2d_Dtor = f_SpriteDtor; Sym::CSprite2d_Draw = f_SpriteDraw;

    printf("== freeze frame ==\n");
    CHECK(!BlurFX::HasFrozen());
    BlurFX::DrawFrozen(); CHECK(drawCalls == 0);                                           // nothing captured: nothing drawn (the live scene stays)
    CHECK(BlurFX::CaptureFrozen()); CHECK(BlurFX::HasFrozen());
    CHECK(rastersAlive == 1 && texturesAlive == 1 && spritesAlive == 1);
    CHECK(createW == 1600 && createH == 720 && createFlags == 0x04 && createDepth == 0);   // FULL size (the blur uses a quarter), a camera texture like the blur's
    CHECK(pushes == 1 && pops == 1 && renderFasts == 1 && ctx == nullptr);                  // push -> copy -> pop, context restored
    CHECK(copiedInto && copiedInto->w == 1600 && copiedFrom == (void*)frontBuffer && copyX == 0 && copyY == 0);   // the front buffer, 1:1, at the origin
    BlurFX::DrawFrozen();
    CHECK(drawCalls == 1);
    CHECK(lastRect[0] == 0 && lastRect[1] == 720 && lastRect[2] == 1600 && lastRect[3] == 0);   // the whole screen (CRect: left, bottom, right, top)
    CHECK(lastColor[0] == 255 && lastColor[1] == 255 && lastColor[2] == 255 && lastColor[3] == 255);   // opaque, untinted
    CHECK(lastSpriteTexture && ((FakeTexture*)lastSpriteTexture)->r == copiedInto);         // drawing exactly the raster that was filled
    for (int i = 0; i < 5; ++i) BlurFX::DrawFrozen(); CHECK(drawCalls == 6 && renderFasts == 1 && rasterCreates == 1);   // redrawn every frame, never re-captured

    // a new pause: forget it, then capture again WITHOUT making a second raster
    BlurFX::InvalidateFrozen(); CHECK(!BlurFX::HasFrozen()); BlurFX::DrawFrozen(); CHECK(drawCalls == 6);
    CHECK(BlurFX::CaptureFrozen() && rasterCreates == 1 && rastersAlive == 1 && renderFasts == 2);
    BlurFX::DrawFrozen(); CHECK(drawCalls == 7);

    // the screen size changes (rotation / display change): rebuilt at the new size, old one released
    struct S { int w, h; };
    BlurFX::InvalidateFrozen(); BlurFX::Shutdown(); CHECK(rastersAlive == 0 && texturesAlive == 0 && spritesAlive == 0);   // everything released
    CHECK(BlurFX::CaptureFrozen() && rastersAlive == 1 && texturesAlive == 1 && spritesAlive == 1);                          // and it can start again after a Shutdown

    // failure paths: the live scene is left alone and nothing is retried every frame
    BlurFX::Shutdown(); BlurFX::InvalidateFrozen(); int c0 = rasterCreates; failRaster = true;
    CHECK(!BlurFX::CaptureFrozen() && !BlurFX::HasFrozen()); CHECK(rasterCreates == c0 + 1);
    for (int i = 0; i < 20; ++i) BlurFX::CaptureFrozen(); CHECK(rasterCreates == c0 + 1);   // one attempt per pause, not one per frame
    BlurFX::DrawFrozen(); CHECK(drawCalls == 7);
    failRaster = false; BlurFX::InvalidateFrozen(); CHECK(BlurFX::CaptureFrozen() && BlurFX::HasFrozen());   // the next pause tries again, and works
    BlurFX::Shutdown(); BlurFX::InvalidateFrozen(); failTexture = true; CHECK(!BlurFX::CaptureFrozen() && rastersAlive == 0 && texturesAlive == 0); failTexture = false;   // half-built: raster released
    BlurFX::InvalidateFrozen();
    { void* none = nullptr; auto sv = Sym::CPostEffects_pRasterFrontBuffer; Sym::CPostEffects_pRasterFrontBuffer = &none; CHECK(!BlurFX::CaptureFrozen() && !BlurFX::HasFrozen()); Sym::CPostEffects_pRasterFrontBuffer = sv; }   // no front buffer yet
    BlurFX::InvalidateFrozen();
    { auto sv = Sym::RwRasterRenderFast; Sym::RwRasterRenderFast = nullptr; CHECK(!BlurFX::CaptureFrozen()); Sym::RwRasterRenderFast = sv; }                                                                            // symbol missing
    BlurFX::InvalidateFrozen(); { auto sv = Sym::CSprite2d_Draw; Sym::CSprite2d_Draw = nullptr; CHECK(!BlurFX::CaptureFrozen()); Sym::CSprite2d_Draw = sv; }
    BlurFX::InvalidateFrozen(); CHECK(BlurFX::CaptureFrozen());
    BlurFX::Shutdown(); CHECK(rastersAlive == 0 && texturesAlive == 0 && spritesAlive == 0);                                // no leaks

    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}

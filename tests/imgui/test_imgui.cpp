// Drives the REAL UI.cpp + ImGuiRW.cpp + Dear ImGui 1.89.7 with simulated fingers.
//
// The game's RenderWare 2D renderer is replaced by a small software rasteriser (textured / vertex-coloured triangles,
// src-alpha blending, scissor) that implements exactly the entry points ImGuiRW.cpp calls. So the pixels checked here are
// what the real code submits to RwIm2DRenderIndexedPrimitive: font atlas upload, glyph quads, clipping, render states.
// What this can NOT prove: how the phone's own RenderWare/GPU turns those triangles into pixels.
#include <mod/logger.h>
#include <mod/amlmod.h>
FakeLogger fl; FakeLogger* logger = &fl; FakeAML fa; FakeAML* aml = &fa;
#include "GameSymbols.h"
#include "CutsceneCtrl.h"
#include "UI.h"
#include "ImGuiRW.h"
#include "imgui/imgui_internal.h"
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include "GameTypes.h"
#include <algorithm>

// ---- stand-ins for the parts of CutsceneCtrl the UI calls -------------------------------------------------------
namespace CutsceneCtrl {
    Settings g_Settings; bool g_bCutscenePaused = false; bool g_bFreeCamActive = false; bool g_bFastForwarding = false;
    int toggles = 0, changed = 0, saves = 0, resets = 0, presets = 0; const void* lastField = nullptr;
    bool TogglePause() { ++toggles; g_bCutscenePaused = !g_bCutscenePaused; return true; }
    void SettingChanged(const void* f) { ++changed; lastField = f; }
    void SaveConfigNow() { ++saves; }
    void ResetAllSettings() { ++resets; g_Settings = Settings(); }
    void ApplyPreset(int p) { ++presets; (void)p; }
}
using namespace CutsceneCtrl;

#define CHECK(c) do{ if(!(c)){ printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails; } else ++passes; }while(0)
static int fails = 0, passes = 0;

// ---- the software RenderWare ---------------------------------------------------------------------------------
struct FakeRaster { int w = 0, h = 0; std::vector<uint32_t> px; };
struct FakeImage  { int32_t flags, width, height, depth, stride; uint8_t* cpPixels; void* palette; };
static const int kStridePad = 16;                                       // real RwImages may have padded rows: the backend must honour stride

static int W = 1600, H = 720;
static std::vector<float> fb;                                           // premultiplied RGBA, 0..255
static FakeRaster* curTex = nullptr;
static bool rasterOn = true;                                            // quiet frames skip the (slow) software rasteriser
static float scis[4] = {};
static int imagesAlive = 0, rastersCreated = 0, rastersAlive = 0, draws = 0, drawSeq = 0, trianglesDrawn = 0, lastTexState = -1;
static float firstZ = -1, firstRhw = -1; static bool sawFirst = false;
static bool badState = false;                                           // an unexpected render state value was set

static void FbClear() { fb.assign((size_t)W * H * 4, 0.0f); }
struct Px { float r, g, b, a; };
static Px At(int x, int y) { if (x < 0 || y < 0 || x >= W || y >= H) return {0,0,0,0}; const float* p = &fb[((size_t)y * W + x) * 4]; return { p[0], p[1], p[2], p[3] }; }

static void* f_ImageCreate(int32_t w, int32_t h, int32_t d) { ++imagesAlive; FakeImage* i = new FakeImage{ 0, w, h, d, w * 4 + kStridePad, nullptr, nullptr }; return i; }
static int32_t f_ImageDestroy(void* p) { FakeImage* i = (FakeImage*)p; delete[] i->cpPixels; delete i; --imagesAlive; return 1; }
static void* f_ImageAlloc(void* p) { FakeImage* i = (FakeImage*)p; i->cpPixels = new uint8_t[(size_t)i->stride * i->height]; memset(i->cpPixels, 0xCD, (size_t)i->stride * i->height); return i; }
static void* f_ImageFindFmt(void* p, int32_t, int32_t* w, int32_t* h, int32_t* d, int32_t* f) { FakeImage* i = (FakeImage*)p; *w = i->width; *h = i->height; *d = 32; *f = 0x500; return i; }
static void* f_RasterCreate(int32_t w, int32_t h, int32_t, int32_t) { ++rastersCreated; ++rastersAlive; FakeRaster* r = new FakeRaster; r->w = w; r->h = h; r->px.assign((size_t)w * h, 0); return r; }
static bool failSetFromImage = false;
static void* f_RasterSetFromImage(void* rp, void* ip)
{
    if (failSetFromImage) return nullptr;
    FakeRaster* r = (FakeRaster*)rp; FakeImage* i = (FakeImage*)ip;
    for (int y = 0; y < r->h; ++y) for (int x = 0; x < r->w; ++x) { uint32_t v; memcpy(&v, i->cpPixels + (size_t)y * i->stride + x * 4, 4); r->px[(size_t)y * r->w + x] = v; }
    return r;
}
static void f_RasterDestroy(void* p) { delete (FakeRaster*)p; --rastersAlive; }
static int32_t f_RenderStateSet(int32_t st, void* v)
{
    if (st == 1) { curTex = (FakeRaster*)v; lastTexState = v ? 1 : 0; }
    if (st == 6 && v != nullptr) badState = true;        // z-test must be off
    if (st == 8 && v != nullptr) badState = true;        // z-write must be off
    return 1;
}
static void f_Scissor(float* r) { memcpy(scis, r, sizeof(scis)); }

static Px Tex(const FakeRaster* t, float u, float v)    // bilinear, clamped
{
    float x = u * t->w - 0.5f, y = v * t->h - 0.5f;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y); float fx = x - x0, fy = y - y0;
    auto g = [&](int xx, int yy) { xx = std::max(0, std::min(xx, t->w - 1)); yy = std::max(0, std::min(yy, t->h - 1)); uint32_t c = t->px[(size_t)yy * t->w + xx];
                                   return Px{ (float)(c & 255), (float)((c >> 8) & 255), (float)((c >> 16) & 255), (float)(c >> 24) }; };
    Px a = g(x0, y0), b = g(x0 + 1, y0), c = g(x0, y0 + 1), d = g(x0 + 1, y0 + 1);
    auto L = [&](float p, float q, float r, float s) { return (p * (1 - fx) + q * fx) * (1 - fy) + (r * (1 - fx) + s * fx) * fy; };
    return { L(a.r, b.r, c.r, d.r), L(a.g, b.g, c.g, d.g), L(a.b, b.b, c.b, d.b), L(a.a, b.a, c.a, d.a) };
}

struct V { float x, y, z, rhw; uint32_t col; float u, v; };
static int32_t f_DrawIndexed(int32_t prim, void* verts, int32_t nv, uint16_t* idx, int32_t ni)
{
    ++draws; ++drawSeq; if (prim != 3) badState = true;
    if (!rasterOn) return 1;
    const V* vs = (const V*)verts;
    if (!sawFirst && nv > 0) { sawFirst = true; firstZ = vs[0].z; firstRhw = vs[0].rhw; }
    const bool clip = !(scis[0] == 0 && scis[1] == 0 && scis[2] == 0 && scis[3] == 0);
    const float cl = scis[0], cr = scis[2], ct = scis[3], cb = scis[1];   // CRect: left, bottom, right, top
    for (int t = 0; t + 2 < ni; t += 3)
    {
        if (idx[t] >= nv || idx[t + 1] >= nv || idx[t + 2] >= nv) { badState = true; continue; }
        const V &a = vs[idx[t]], &b = vs[idx[t + 1]], &c = vs[idx[t + 2]];
        ++trianglesDrawn;
        float minx = std::min({a.x, b.x, c.x}), maxx = std::max({a.x, b.x, c.x}), miny = std::min({a.y, b.y, c.y}), maxy = std::max({a.y, b.y, c.y});
        int x0 = std::max(0, (int)std::floor(minx)), x1 = std::min(W - 1, (int)std::ceil(maxx)), y0 = std::max(0, (int)std::floor(miny)), y1 = std::min(H - 1, (int)std::ceil(maxy));
        float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-6f) continue;
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x)
        {
            float px = x + 0.5f, py = y + 0.5f;
            if (clip && (px < cl || px >= cr || py < ct || py >= cb)) continue;
            float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
            auto mix = [&](uint32_t sh) { return w0 * ((a.col >> sh) & 255) + w1 * ((b.col >> sh) & 255) + w2 * ((c.col >> sh) & 255); };
            float cr_ = mix(0), cg = mix(8), cb_ = mix(16), ca = mix(24);
            if (curTex)
            {
                Px t4 = Tex(curTex, w0 * a.u + w1 * b.u + w2 * c.u, w0 * a.v + w1 * b.v + w2 * c.v);
                cr_ = cr_ * t4.r / 255.0f; cg = cg * t4.g / 255.0f; cb_ = cb_ * t4.b / 255.0f; ca = ca * t4.a / 255.0f;
            }
            float sa = ca / 255.0f; float* d = &fb[((size_t)y * W + x) * 4];
            d[0] = cr_ * sa + d[0] * (1 - sa); d[1] = cg * sa + d[1] * (1 - sa); d[2] = cb_ * sa + d[2] * (1 - sa); d[3] = ca + d[3] * (1 - sa);
        }
    }
    return 1;
}

// ---- helpers ------------------------------------------------------------------------------------------------
static Sym::RsGlobalLite rsg = { "test", 1600, 720 };
static float nearZ = 0.25f, recipZ = 4.0f;
static void SetupSyms()
{
    Sym::RsGlobal = &rsg; Sym::RwImageCreate = f_ImageCreate; Sym::RwImageDestroy = f_ImageDestroy; Sym::RwImageAllocatePixels = f_ImageAlloc;
    Sym::RwImageFindRasterFormat = f_ImageFindFmt; Sym::RwRasterSetFromImage = f_RasterSetFromImage; Sym::RwRasterCreate = f_RasterCreate;
    Sym::RwRasterDestroy = f_RasterDestroy; Sym::RwRenderStateSet = f_RenderStateSet; Sym::RwIm2DRenderIndexedPrimitive = f_DrawIndexed;
    Sym::CWidget_SetScissor = f_Scissor; Sym::CSprite2d_NearScreenZ = &nearZ; Sym::CSprite2d_RecipNearClip = &recipZ; Sym::g_bImGuiBackendResolved = true;
}
static void SetScreen(int w, int h) { W = w; H = h; rsg.maximumWidth = w; rsg.maximumHeight = h; }

// ---- a fake CFont: a simple proportional model so the layout maths can be checked ---------------------------------------
static uint8_t fkDet[64], fkRS[48];
static float fkScale = 1; static int fkStyle = 0; static CRGBA fkColor;
struct Printed { std::string text; int style; float x, y, lineH, w; CRGBA col; };
static std::vector<Printed> printedAll, pending;
static int flushes = 0, flushDrawSeq = 0; static bool fkWidthZero = false;
static float StyleW(int st) { return 6.0f + 1.5f * st; }                   // per-character width at scale 1: wider fonts for higher ids
static void f_fStyle(uint8_t st) { fkStyle = st; fkDet[0x34] = st; }
static void f_fScale(float sc) { fkScale = sc; memcpy(fkDet + 8, &sc, 4); }
static void f_fColor(const CRGBA& c) { fkColor = c; memcpy(fkDet + 0x18, &c, 4); }
static void f_fOrient(uint8_t o) { fkDet[0x2a] = o; }
static void f_fProp(uint8_t o) { fkDet[0x29] = o; }
static void f_fBack(uint8_t a, uint8_t b) { fkDet[0x1b] = a; fkDet[0x1c] = b; }
static void f_fJust(uint8_t o) { fkDet[0x28] = o; }
static void f_fWrap(float w) { memcpy(fkDet + 0x20, &w, 4); }
static void f_fDrop(int8_t d) { fkDet[0x2b] = (uint8_t)d; }
static float f_fHeight(uint8_t) { return 18.0f * fkScale; }
static float f_fWidth(unsigned short* t, uint8_t, uint8_t) { if (fkWidthZero) return 0.0f; int n = 0; while (t[n]) ++n; return n * StyleW(fkStyle) * fkScale; }
static void f_fPrint(float x, float y, unsigned short* t)
{
    Printed p; for (int i = 0; t[i]; ++i) p.text += (char)t[i];
    p.style = fkStyle; p.x = x; p.y = y; p.lineH = 18.0f * fkScale; p.w = (float)p.text.size() * StyleW(fkStyle) * fkScale; p.col = fkColor; pending.push_back(p);
    memset(fkRS, 0x5A, sizeof(fkRS));                                       // the real one dirties its render state too
}
static void f_fFlush()
{
    ++flushes; flushDrawSeq = drawSeq;
    for (const Printed& p : pending)
    {
        printedAll.push_back(p);
        if (!rasterOn) continue;
        const bool black = (p.col.r == 0 && p.col.g == 0 && p.col.b == 0);
        for (int y = std::max(0, (int)p.y); y < std::min(H, (int)(p.y + p.lineH)); ++y) for (int x = std::max(0, (int)p.x); x < std::min(W, (int)(p.x + p.w)); ++x)
        {   // a flat pink box (never "bright"), so ImGui's white Arial pixels can be told apart from the game text
            float sa = p.col.a / 255.0f; float* d = &fb[((size_t)y * W + x) * 4];
            d[0] = (black ? 0.0f : 255.0f) * sa + d[0] * (1 - sa); d[1] = 0.0f + d[1] * (1 - sa); d[2] = (black ? 0.0f : 255.0f) * sa + d[2] * (1 - sa); d[3] = p.col.a + d[3] * (1 - sa);
        }
    }
    pending.clear();
}
static void ClearPrinted() { printedAll.clear(); pending.clear(); flushes = 0; }
static const Printed* FindPrint(const char* text)                            // the coloured print (not the black shadow) of `text`
{ for (const Printed& p : printedAll) if (p.text == text && !(p.col.r == 0 && p.col.g == 0 && p.col.b == 0)) return &p; return nullptr; }
static const Printed* FindShadow(const char* text)
{ for (const Printed& p : printedAll) if (p.text == text && p.col.r == 0 && p.col.g == 0 && p.col.b == 0) return &p; return nullptr; }
static void SetupFont()
{
    Sym::CFont_SetFontStyle = f_fStyle; Sym::CFont_SetScale = f_fScale; Sym::CFont_SetColor = f_fColor; Sym::CFont_SetOrientation = f_fOrient;
    Sym::CFont_SetProportional = f_fProp; Sym::CFont_SetBackground = f_fBack; Sym::CFont_SetJustify = f_fJust; Sym::CFont_SetWrapx = f_fWrap;
    Sym::CFont_SetDropShadowPosition = f_fDrop; Sym::CFont_GetHeight = f_fHeight; Sym::CFont_GetStringWidth = f_fWidth; Sym::CFont_PrintString = f_fPrint;
    Sym::CFont_RenderFontBuffer = f_fFlush; Sym::CFont_Details = fkDet; Sym::CFont_RenderState = fkRS;
}

static bool g_inCutscene = true, g_drawUI = true, g_vignette = false, g_ffButton = false;
static void Frame(bool paused = false, const char* banner = nullptr, const char* info = nullptr)
{
    FbClear();
    UI::FrameInfo fi; fi.inCutscene = g_inCutscene; fi.drawUI = g_drawUI; fi.vignette = g_vignette; fi.ffButton = g_ffButton; fi.paused = paused; fi.banner = banner; fi.info = info;
    UI::Frame(fi);
}
static void FrameQ(bool paused = false, const char* banner = nullptr) { rasterOn = false; UI::FrameInfo fi; fi.inCutscene = g_inCutscene; fi.drawUI = g_drawUI; fi.vignette = g_vignette; fi.ffButton = g_ffButton; fi.paused = paused; fi.banner = banner; UI::Frame(fi); rasterOn = true; }
static void Idle(int n = 14, bool paused = false, const char* banner = nullptr) { for (int i = 0; i < n; ++i) FrameQ(paused, banner); }   // 14 frames @ 1/30 s = 0.47 s > ImGui's double-click time

struct R4 { float l = 0, t = 0, r = 0, b = 0; float cx() const { return (l + r) / 2; } float cy() const { return (t + b) / 2; } float w() const { return r - l; } float h() const { return b - t; }
            bool has(float x, float y) const { return x >= l && x < r && y >= t && y < b; } };
static R4 Rc(const float* a) { return { a[0], a[1], a[2], a[3] }; }
static bool Down(int id, float x, float y) { return UI::PushTouchEvent(2, id, (int)x, (int)y); }
static bool Move(int id, float x, float y) { return UI::PushTouchEvent(3, id, (int)x, (int)y); }
static bool Up(int id, float x, float y)   { return UI::PushTouchEvent(1, id, (int)x, (int)y); }
static bool Tap(float x, float y, bool paused = false)
{
    bool a = Down(0, x, y); FrameQ(paused); FrameQ(paused); bool b = Up(0, x, y);
    for (int i = 0; i < 4; ++i) FrameQ(paused);
    Idle(14, paused);
    return a && b;
}
static void Drag(float x0, float y0, float x1, float y1, int steps = 8)
{
    Down(0, x0, y0); FrameQ(); FrameQ();
    for (int i = 1; i <= steps; ++i) { Move(0, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps); FrameQ(); }
    Up(0, x1, y1); for (int i = 0; i < 4; ++i) FrameQ();
    Idle(14);
}
static const char* OutDir() { const char* t = getenv("CUTSCENE_TEST_OUT"); return t ? t : "/tmp"; }
static void Save(const char* name) { std::string full = std::string(OutDir()) + "/" + (strrchr(name, '/') ? strrchr(name, '/') + 1 : name); FILE* f = fopen(full.c_str(), "wb"); std::vector<uint8_t> o((size_t)W * H * 4); for (size_t i = 0; i < o.size(); ++i) o[i] = (uint8_t)std::max(0.0f, std::min(255.0f, fb[i])); fwrite(o.data(), 1, o.size(), f); fclose(f); }

// bright (text) pixels inside a rect, ignoring `inset` px at the rect border (where the button outline lives)
struct Bright { int n = 0; float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f; };
static Bright BrightIn(R4 rc, float inset, float thr = 190.0f)
{
    Bright o;
    for (int y = (int)(rc.t + inset); y < (int)(rc.b - inset); ++y) for (int x = (int)(rc.l + inset); x < (int)(rc.r - inset); ++x)
    { Px p = At(x, y); if ((p.r + p.g + p.b) / 3 >= thr) { ++o.n; o.l = std::min(o.l, (float)x); o.r = std::max(o.r, (float)x + 1); o.t = std::min(o.t, (float)y); o.b = std::max(o.b, (float)y + 1); } }
    return o;
}
static int CoveredPixels() { int n = 0; for (size_t i = 3; i < fb.size(); i += 4) if (fb[i] > 0.5f) ++n; return n; }
static int PixelsOutside(const std::vector<R4>& keep, float slack = 2.0f)
{
    int n = 0;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
    {
        if (fb[((size_t)y * W + x) * 4 + 3] <= 0.5f) continue;
        bool in = false; for (const R4& k : keep) if (x >= k.l - slack && x < k.r + slack && y >= k.t - slack && y < k.b + slack) { in = true; break; }
        if (!in) ++n;
    }
    return n;
}
static UI::DebugInfo D() { return UI::GetDebug(); }
static int Find(const char* label) { for (int i = 0; i < UI::DebugItemCount(); ++i) if (!strcmp(UI::DebugItemLabel(i), label)) return i; return -1; }
static bool Item(const char* label, R4& r) { float o[4]; int i = Find(label); if (i < 0 || !UI::DebugItemRect(i, o)) return false; r = Rc(o); return true; }
static ImGuiWindow* PopupWindow() { ImGuiContext& g = *ImGui::GetCurrentContext(); for (ImGuiWindow* w : g.Windows) if ((w->Flags & ImGuiWindowFlags_Popup) && w->Active) return w; return nullptr; }
static void OpenWindow() { if (!D().winOpen) { R4 s = Rc(D().set); Tap(s.cx(), s.cy()); } }
static void CloseWindowViaX()
{
    R4 w = Rc(D().win); ImGuiWindow* iw = ImGui::FindWindowByName("Cutscene Control Settings###ccwin"); float tb = iw ? iw->TitleBarHeight() : 30;
    Tap(w.r - tb * 0.5f, w.t + tb * 0.5f);
}
static void ScrollTo(float y)                     // by real finger swipes inside the content
{
    for (int guard = 0; guard < 80; ++guard)
    {
        UI::DebugInfo d = D(); float diff = y - d.scrollY; if (std::fabs(diff) < 2.0f) break;
        R4 c = Rc(d.content); float cx = c.l + c.w() * 0.93f, y0 = diff > 0 ? c.b - 10 : c.t + 10, span = std::min(std::fabs(diff), c.h() - 24); float y1 = diff > 0 ? y0 - span : y0 + span;
        Drag(cx, y0, cx, y1, 4);
    }
}

static bool Reach(const char* label, R4& r)      // scroll (by finger) until the row is fully inside the window body
{
    for (int g = 0; g < 16; ++g)
    {
        R4 cb = Rc(D().content);
        if (Item(label, r) && r.t >= cb.t + 4 && r.b <= cb.b - 4) return true;
        float next = D().scrollY + 140; if (D().scrollY >= D().scrollMax - 4.0f) next = 0; else next = std::min(next, D().scrollMax);
        ScrollTo(next);
    }
    return false;
}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    SetupSyms(); SetupFont(); UI::DebugSetFixedDt(1.0f / 30.0f);
    g_Settings.labelAlpha = 255; g_Settings.outlineAlpha = 255;   // sections A-E check solid text / outline; F-H cover the see-through defaults
    printf("== A. backend: font upload, render states, vertex setup ==\n");
    UI::Init(); UI::SetActive(true);
    CHECK(!Down(0, 800, 360));                        // before any frame no rectangle is published: nothing is captured
    Up(0, 800, 360);
    Frame();
    CHECK(ImGuiRW::IsReady());
    CHECK(rastersCreated == 1 && rastersAlive == 1);  // exactly one font raster
    CHECK(imagesAlive == 0);                          // the temporary RwImage was destroyed (no leak)
    CHECK(draws > 0 && trianglesDrawn > 0);
    CHECK(sawFirst && firstZ == nearZ && firstRhw == recipZ);          // vertices carry the game's near-Z / 1/near-clip
    CHECK(!badState);
    CHECK(lastTexState == 0);                                          // texture raster state reset after drawing
    CHECK(scis[0] == 0 && scis[1] == 0 && scis[2] == 0 && scis[3] == 0);   // scissor cleared again
    { UI::DebugInfo d = D(); CHECK(d.uiReady); CHECK(d.pauseShown && d.setShown); }
    { int n = 0; for (int i = 0; i < 5; ++i) Frame(); n = rastersCreated; CHECK(n == 1); }   // not re-uploaded every frame

    printf("== B. PAUSE / RESUME button (the reported bug) ==\n");
    Idle();
    {
        Frame(false); UI::DebugInfo d = D(); R4 p = Rc(d.pause), s = Rc(d.set);
        CHECK(d.pauseShown && d.setShown);
        CHECK(!strcmp(d.pauseLabel, "PAUSE"));
        Bright b = BrightIn(p, 5);
        printf("    PAUSE button %.0fx%.0f at (%.0f,%.0f); text pixels=%d bbox %.0f..%.0f x %.0f..%.0f\n", p.w(), p.h(), p.l, p.t, b.n, b.l, b.r, b.t, b.b);
        CHECK(b.n > 250);                                              // real, substantial text
        CHECK(b.l > p.l && b.r < p.r && b.t > p.t && b.b < p.b);       // inside the plate
        CHECK(std::fabs((b.l + b.r) / 2 - p.cx()) < p.w() * 0.06f);    // centred horizontally
        CHECK(std::fabs((b.t + b.b) / 2 - p.cy()) < p.h() * 0.10f);    // and vertically
        CHECK(b.r - b.l > p.w() * 0.45f);                              // PAUSE is the shorter label, but still fills a good part
        CHECK(std::fabs(p.r - (W - 0.025f * W)) < 3 && std::fabs(p.b - (H - 0.06f * H)) < 3);   // bottom-right with the .ini margins
        CHECK(p.w() > 0.10f * W && p.w() < 0.20f * W);                 // ~13% of the screen width, as documented
        CHECK(s.r < p.l && std::fabs(s.cy() - p.cy()) < 3);            // SET sits to the left, centred on the big one
        CHECK(BrightIn(s, 3).n > 60);                                  // and "SET" is readable too
        Save("/tmp/ui_unpaused.rgba");
        // border colour of the unpaused button is whitish, plate is dark
        Px edge = At((int)p.cx(), (int)p.t); CHECK(edge.r > 100 && edge.b > 100);
        float pw = p.w(), ph = p.h();

        Frame(true, "PAUSED"); d = D(); R4 q = Rc(d.pause);
        CHECK(!strcmp(d.pauseLabel, "RESUME"));
        Bright b2 = BrightIn(q, 5);
        printf("    RESUME text pixels=%d bbox %.0f..%.0f\n", b2.n, b2.l, b2.r);
        CHECK(b2.n > 250 && b2.n > b.n);                               // more letters => more ink than PAUSE
        CHECK(b2.r - b2.l > b.r - b.l);                                // and wider
        CHECK(b2.l > q.l && b2.r < q.r);
        CHECK(std::fabs(q.w() - pw) < 1.5f && std::fabs(q.h() - ph) < 1.5f);   // the button does not change size when the label flips
        CHECK(std::fabs(q.l - p.l) < 1.5f && std::fabs(q.t - p.t) < 1.5f);
        Px gold = At((int)q.cx(), (int)q.t); CHECK(gold.r > 180 && gold.g > 120 && gold.b < 120);   // gold outline while paused
        // banner
        R4 band = { 0.2f * W, 0.08f * H, 0.8f * W, 0.45f * H };
        Bright bb = BrightIn(band, 0, 220);
        printf("    PAUSED banner pixels=%d bbox %.0f..%.0f x %.0f..%.0f\n", bb.n, bb.l, bb.r, bb.t, bb.b);
        CHECK(bb.n > 1500);
        CHECK(std::fabs((bb.l + bb.r) / 2 - W / 2.0f) < 0.03f * W);
        CHECK(bb.r - bb.l > 0.18f * W);
        Save("/tmp/ui_paused.rgba");
        Frame(false, "PAUSED"); CHECK(BrightIn(band, 0, 220).n == 0);  // banner only while paused
        Frame(true, nullptr); CHECK(BrightIn(band, 0, 220).n == 0);    // and only when asked for
        Frame(true, "PAUSED", "Free camera   speed 0.30");
        CHECK(BrightIn({ 0.0f, 0.86f * H, 0.5f * W, (float)H }, 0, 200).n > 200);   // info line bottom-left
    }
    printf("   custom / empty labels\n");
    {
        strcpy(g_Settings.pauseButtonLabel, ""); strcpy(g_Settings.resumeButtonLabel, "");
        Frame(false); CHECK(!strcmp(D().pauseLabel, "PAUSE")); CHECK(BrightIn(Rc(D().pause), 5).n > 250);       // blank .ini value -> default text, still drawn
        Frame(true);  CHECK(!strcmp(D().pauseLabel, "RESUME")); CHECK(BrightIn(Rc(D().pause), 5).n > 250);
        strcpy(g_Settings.pauseButtonLabel, "PAUSE"); strcpy(g_Settings.resumeButtonLabel, "CONTINUE THE SCENE");
        Frame(true); R4 q = Rc(D().pause); Bright b = BrightIn(q, 4);
        CHECK(!strcmp(D().pauseLabel, "CONTINUE THE SCENE")); CHECK(b.l > q.l && b.r < q.r && q.r <= W && q.l >= 0);   // long label: button grows, text never overflows
        Frame(false); R4 p = Rc(D().pause); CHECK(std::fabs(p.w() - q.w()) < 1.5f);                               // width is the longer of the two labels
        strcpy(g_Settings.resumeButtonLabel, "RESUME");
        Frame(false); strcpy(g_Settings.pauseButtonLabel, "STOP"); Frame(false); CHECK(!strcmp(D().pauseLabel, "STOP")); CHECK(BrightIn(Rc(D().pause), 5).n > 150);
        strcpy(g_Settings.pauseButtonLabel, "PAUSE");
    }
    printf("   corners, scale, opacity, visibility\n");
    {
        for (int c = 0; c < 4; ++c)
        {
            g_Settings.buttonCorner = c; Frame(false); UI::DebugInfo d = D(); R4 p = Rc(d.pause), s = Rc(d.set);
            bool right = (c == 0 || c == 2), bottom = (c == 0 || c == 1);
            CHECK(p.l >= 0 && p.r <= W && p.t >= 0 && p.b <= H && s.l >= 0 && s.r <= W && s.t >= 0 && s.b <= H);
            CHECK((p.cx() > W / 2.0f) == right); CHECK((p.cy() > H / 2.0f) == bottom);
            CHECK(right ? (s.r < p.l) : (s.l > p.r));                  // SET is on the inboard side of the big button
            CHECK(BrightIn(p, 5).n > 250);
        }
        g_Settings.buttonCorner = 0;
        g_Settings.buttonScale = 0.5f; Frame(false); float h05 = D().pause[3] - D().pause[1]; CHECK(BrightIn(Rc(D().pause), 3).n > 40);
        g_Settings.buttonScale = 2.0f; Frame(false); float h20 = D().pause[3] - D().pause[1]; CHECK(BrightIn(Rc(D().pause), 5).n > 900);
        CHECK(std::fabs(h20 / h05 - 4.0f) < 0.3f); CHECK(Rc(D().pause).l >= 0 && Rc(D().set).l >= 0);
        g_Settings.buttonScale = 1.0f;
        g_Settings.buttonPlateAlpha = 0; Frame(false); { R4 p = Rc(D().pause); CHECK(At((int)(p.l + 8), (int)(p.t + 8)).a < 1.0f); CHECK(BrightIn(p, 5).n > 250); }   // no plate, text still there
        g_Settings.buttonPlateAlpha = 255; Frame(false); { R4 p = Rc(D().pause); CHECK(At((int)(p.l + 8), (int)(p.t + 8)).a > 250.0f); }
        g_Settings.buttonPlateAlpha = 120;
        g_Settings.showPauseButton = false; Frame(false); CHECK(!D().pauseShown && D().setShown);
        { R4 s = Rc(D().set); CHECK(std::fabs(s.r - (W - 0.025f * W)) < 3); }
        g_Settings.showPauseButton = true; g_Settings.showSettingsButton = false; Frame(false); CHECK(D().pauseShown && !D().setShown);
        g_Settings.showSettingsButton = true; Frame(false);
    }
    printf("   other screen sizes\n");
    {
        int sizes[][2] = { { 800, 480 }, { 1280, 720 }, { 2400, 1080 }, { 1920, 1080 } };
        for (auto& sz : sizes)
        {
            SetScreen(sz[0], sz[1]); Frame(false); UI::DebugInfo d = D(); R4 p = Rc(d.pause); Bright b = BrightIn(p, 4);   // fonts were built for 720p: the UI follows the screen height
            CHECK(p.l >= 0 && p.r <= W && p.b <= H); CHECK(b.n > 80); CHECK(std::fabs(p.w() / W - 0.13f) < 0.05f);
        }
        SetScreen(1600, 720);
        UI::Shutdown(); rastersCreated = 0; rsg.maximumHeight = 1080; rsg.maximumWidth = 2400; W = 2400; H = 1080;      // a fresh start on a hi-res phone: fonts rasterised for 1080
        Frame(false); { UI::DebugInfo d = D(); R4 p = Rc(d.pause); CHECK(d.uiReady && rastersCreated == 1 && BrightIn(p, 5).n > 600); CHECK(std::fabs(p.w() / W - 0.13f) < 0.03f); }
        SetScreen(1600, 720); UI::Shutdown(); rastersCreated = 0; Frame(false); CHECK(D().uiReady && rastersCreated == 1);
    }

    printf("== C. touch: capture, taps, cancel ==\n");
    Idle();
    {
        UI::DebugInfo d = D(); R4 p = Rc(d.pause), s = Rc(d.set);
        toggles = 0;
        CHECK(!Down(0, 400, 300)); Frame(); CHECK(!Up(0, 400, 300)); Idle(); CHECK(toggles == 0);                // empty screen: the game gets it
        CHECK(Tap(p.cx(), p.cy())); CHECK(toggles == 1);                                                           // tap on PAUSE: swallowed, toggles once
        g_bCutscenePaused = false;
        CHECK(!Down(1, p.cx(), p.t - 90)); for (int i = 0; i < 5; ++i) Move(1, p.cx(), p.t - 90 + 18 * (i + 1)); Frame(); CHECK(!Up(1, p.cx(), p.cy())); Idle();   // slid in from outside: never ours
        CHECK(toggles == 1);
        CHECK(Down(0, p.cx(), p.cy())); FrameQ(); FrameQ(); Move(0, p.cx(), p.t - 120); FrameQ(); FrameQ(); CHECK(Up(0, p.cx(), p.t - 120)); Idle();       // pressed, dragged off, released: cancelled
        CHECK(toggles == 1);
        CHECK(Down(0, p.cx(), p.cy())); Frame(); CHECK(Down(1, 200, 200) == false); Frame(); CHECK(Up(0, p.cx(), p.cy())); Idle();                      // 2nd finger on the game doesn't disturb
        CHECK(toggles == 2); g_bCutscenePaused = false;
        UI::SetActive(false); CHECK(!Down(0, p.cx(), p.cy())); Up(0, p.cx(), p.cy()); UI::SetActive(true); Frame(); CHECK(toggles == 2);               // not active: nothing is captured
        CHECK(!D().winOpen);
    }
    printf("   touch coordinates scale from window pixels to render pixels\n");
    {
        static int ww = 800, wh = 360; Sym::OS_ScreenGetWidth = []() -> int32_t { return ww; }; Sym::OS_ScreenGetHeight = []() -> int32_t { return wh; };
        Frame(); R4 p = Rc(D().pause); toggles = 0;
        CHECK(Tap(p.cx() / 2, p.cy() / 2)); CHECK(toggles == 1); g_bCutscenePaused = false;
        CHECK(!Down(0, p.cx(), p.cy() / 2) || true); Up(0, p.cx(), p.cy() / 2); Idle();
        Sym::OS_ScreenGetWidth = nullptr; Sym::OS_ScreenGetHeight = nullptr; Frame(); Idle();
    }

    printf("== D. settings window ==\n");
    {
        UI::DebugInfo d0 = D(); R4 s = Rc(d0.set); CHECK(!d0.winOpen);
        CHECK(Tap(s.cx(), s.cy())); UI::DebugInfo d = D(); CHECK(d.winOpen && !d.winCollapsed);
        R4 w = Rc(d.win); printf("    window %.0f,%.0f  %.0fx%.0f  scroll max %.0f\n", w.l, w.t, w.w(), w.h(), d.scrollMax);
        CHECK(w.l >= 0 && w.t >= 0 && w.r <= W && w.b <= H); CHECK(w.w() > 0.3f * W && w.w() < 0.5f * W);
        CHECK(d.scrollMax > 100);                                                        // content is longer than the window: it scrolls
        CHECK(UI::DebugItemCount() >= 28);
        { UI::FrameInfo fi; Frame(); Bright tb = BrightIn({ w.l + 28, w.t + 1, w.r - 30, w.t + 28 }, 0, 200); CHECK(tb.n > 200); }   // title text
        Save("/tmp/ui_window_top.rgba");
        // taps inside the window are ours, outside are the game's
        CHECK(Down(0, w.cx(), w.t + 18)); Frame(); Up(0, w.cx(), w.t + 18); Idle();                      // (on the title bar: harmless)
        CHECK(!Down(0, w.r + 30, w.cy())); Frame(); Up(0, w.r + 30, w.cy()); Idle();
        // only our pixels are on screen: window + the two buttons
        Frame(); CHECK(PixelsOutside({ w, Rc(D().pause), Rc(D().set) }) == 0);
    }
    printf("   checkboxes\n");
    {
        struct CB { const char* label; bool* f; } cbs[] = {
            { "Show pause button", &g_Settings.showPauseButton }, { "PAUSED banner", &g_Settings.showPauseText }, { "Background blur", &g_Settings.showBlur },
            { "Real Gaussian blur", &g_Settings.useGaussianShader }, { "Show subtitles", &g_Settings.showSubtitles }, { "Show mission name", &g_Settings.showMissionName },
            { "Silence game audio", &g_Settings.pauseGameAudio }, { "Skip while paused", &g_Settings.skipInPause }, { "Use game skip keys", &g_Settings.useSkipGameKeys },
            { "Only during missions", &g_Settings.pauseOnlyDuringMissions }, { "Show speed text", &g_Settings.showCamSpeedText }, { "Pause on app return", &g_Settings.fixAudioDesync } };
        int ok = 0, total = 0;
        for (auto& c : cbs)
        {
            // scroll until the row is on screen
            // a person scrolls until the row is comfortably inside the window body (not half hidden under the edge / grip)
            R4 r; bool vis = false;
            for (int g = 0; g < 14; ++g)
            {
                R4 cb = Rc(D().content);
                if (Item(c.label, r) && r.t >= cb.t + 4 && r.b <= cb.b - 4) { vis = true; break; }
                float next = D().scrollY + 140; if (D().scrollY >= D().scrollMax - 4.0f) next = 0; else next = std::min(next, D().scrollMax);   // to the end, then wrap to the top
                ScrollTo(next);
            }
            ++total; if (!vis) { printf("    row not reachable: %s\n", c.label); continue; }
            bool before = *c.f; int ch = changed; changed = ch;
            Tap(r.l + 20, r.cy());                                                         // tap on the box
            bool flipped = (*c.f != before) && lastField == (const void*)c.f;
            Tap(r.l + 20, r.cy());                                                         // and back
            if (flipped && *c.f == before) ++ok; else printf("    checkbox misbehaved: %s (before=%d now=%d)\n", c.label, before, *c.f);
        }
        printf("    %d/%d checkboxes toggle on tap and back\n", ok, total); CHECK(ok == total);
        // tapping the *label* works too
        ScrollTo(0); Frame(); R4 r; Item("Show pause button", r); bool b0 = g_Settings.showPauseButton; Tap(r.l + r.w() * 0.45f, r.cy()); CHECK(g_Settings.showPauseButton != b0); Tap(r.l + r.w() * 0.45f, r.cy()); CHECK(g_Settings.showPauseButton == b0);
        // the effect is visible immediately
        Tap(r.l + 20, r.cy()); Frame(); CHECK(!D().pauseShown); Tap(r.l + 20, r.cy()); Frame(); CHECK(D().pauseShown);
        // the blue check mark is drawn when on (compare a pixel region before/after)
        Frame(); float on = 0, off = 0; Item("Show pause button", r);
        for (int y = (int)r.t; y < (int)r.b; ++y) for (int x = (int)r.l; x < (int)(r.l + r.h()); ++x) { Px p = At(x, y); on += p.r + p.g + p.b; }
        Tap(r.l + 20, r.cy()); Frame(); Item("Show pause button", r);
        for (int y = (int)r.t; y < (int)r.b; ++y) for (int x = (int)r.l; x < (int)(r.l + r.h()); ++x) { Px p = At(x, y); off += p.r + p.g + p.b; }
        CHECK(on > off * 1.05f); Tap(r.l + 20, r.cy());
    }
    printf("   sliders: tap jumps, horizontal drag slides, vertical swipe scrolls\n");
    {
        ScrollTo(0); Frame(); R4 r;
        CHECK(Item("Button opacity", r));
        Tap(r.l + r.w() * 0.80f, r.cy()); printf("    opacity after tap@80%% = %d\n", g_Settings.buttonPlateAlpha); CHECK(g_Settings.buttonPlateAlpha > 160 && g_Settings.buttonPlateAlpha < 235);
        Drag(r.l + r.w() * 0.80f, r.cy(), r.l + r.w() * 0.12f, r.cy()); printf("    opacity after drag->12%% = %d\n", g_Settings.buttonPlateAlpha); CHECK(g_Settings.buttonPlateAlpha < 70);
        Drag(r.l + r.w() * 0.12f, r.cy(), r.l + r.w() * 3.0f, r.cy()); CHECK(g_Settings.buttonPlateAlpha == 255);                                  // dragging far past the end clamps
        CHECK(Item("Button size", r)); Tap(r.l + r.w() * 0.5f, r.cy()); printf("    size after tap@50%% = %.2f\n", g_Settings.buttonScale); CHECK(g_Settings.buttonScale > 1.1f && g_Settings.buttonScale < 1.4f);
        float sc = g_Settings.buttonScale; (void)sc;
        // a vertical swipe that starts ON a slider scrolls the window and does not touch the value
        int before = g_Settings.buttonPlateAlpha; float scBefore = g_Settings.buttonScale; float y0 = D().scrollY; int chg = changed;
        Item("Button size", r); Drag(r.l + r.w() * 0.4f, r.cy(), r.l + r.w() * 0.4f, r.cy() - 150);
        CHECK(D().scrollY > y0 + 60); CHECK(g_Settings.buttonScale == scBefore && g_Settings.buttonPlateAlpha == before); CHECK(changed == chg);
        // and a swipe on a checkbox row
        ScrollTo(0); Frame(); bool pb = g_Settings.showPauseButton; chg = changed; Item("Show pause button", r); y0 = D().scrollY; Drag(r.l + 20, r.cy(), r.l + 20, r.cy() - 140);
        CHECK(D().scrollY > y0 + 50); CHECK(g_Settings.showPauseButton == pb); CHECK(changed == chg);
        g_Settings.buttonPlateAlpha = 120; g_Settings.buttonScale = 1.0f;
    }
    printf("   combo boxes\n");
    {
        ScrollTo(0); Frame(); R4 r; CHECK(Item("Button corner", r));
        Tap(r.cx() - 40, r.cy()); CHECK(D().popupOpen); ImGuiWindow* pop = PopupWindow(); CHECK(pop != nullptr);
        if (pop)
        {
            ImGuiStyle& st = ImGui::GetStyle(); float fh = ImGui::GetFontSize(), pitch = fh + st.ItemSpacing.y; Frame();
            CHECK(pop->Pos.x >= 0 && pop->Pos.y >= 0 && pop->Pos.x + pop->Size.x <= W + 1 && pop->Pos.y + pop->Size.y <= H + 1);
            CHECK(Down(0, 5, 5));                                                                                 // while a list is open every touch is ours (so tapping outside only closes it)
            Frame(); CHECK(Up(0, 5, 5)); Idle(); CHECK(!D().popupOpen);                                            // tap outside closes
            CHECK(g_Settings.buttonCorner == 0);
            Tap(r.cx() - 40, r.cy()); pop = PopupWindow(); CHECK(pop != nullptr);
            int target = 3; float y = pop->Pos.y + st.WindowPadding.y + target * pitch + fh * 0.5f; int chg = changed;
            Save("/tmp/ui_combo_open.rgba");
            Tap(pop->Pos.x + pop->Size.x * 0.5f, y);
            printf("    corner after picking 'Top left' = %d\n", g_Settings.buttonCorner);
            CHECK(g_Settings.buttonCorner == 3); CHECK(!D().popupOpen); CHECK(changed > chg && lastField == (const void*)&g_Settings.buttonCorner);
            Frame(); CHECK(D().pause[1] < H / 2.0f && D().pause[0] < W / 2.0f);                                    // the real button moved to the top-left
            g_Settings.buttonCorner = 0; Frame();
        }
        // gamepad combos map to the right enum values
        ScrollTo(0); for (int g = 0; g < 8; ++g) { if (Item("Gamepad pause", r)) break; ScrollTo(std::min(D().scrollMax, D().scrollY + 140)); }
        if (Item("Gamepad pause", r))
        {
            Tap(r.cx() - 40, r.cy()); ImGuiWindow* p2 = PopupWindow(); CHECK(p2 != nullptr);
            if (p2) { ImGuiStyle& st = ImGui::GetStyle(); float fh = ImGui::GetFontSize(), pitch = fh + st.ItemSpacing.y; Tap(p2->Pos.x + p2->Size.x * 0.5f, p2->Pos.y + st.WindowPadding.y + 9 * pitch + fh * 0.5f); }
            printf("    Gamepad pause -> %d (expect R2 = %d)\n", (int)g_Settings.buttonPause, (int)GamepadButton::R2); CHECK(g_Settings.buttonPause == GamepadButton::R2);
            g_Settings.buttonPause = GamepadButton::Start;
        }
    }
    printf("   scrolling reaches everything, buttons run actions\n");
    {
        ScrollTo(D().scrollMax + 500); UI::DebugInfo d = D(); printf("    scroll %.0f / %.0f\n", d.scrollY, d.scrollMax); CHECK(d.scrollY >= d.scrollMax - 2);
        Frame(); Save("/tmp/ui_window_bottom.rgba");
        R4 r; CHECK(Item("Minimal preset (no blur / banner)", r)); presets = 0; Tap(r.cx(), r.cy()); CHECK(presets == 1);
        CHECK(Item("Reset ALL settings", r)); g_Settings.blurAlpha = 33; resets = 0; Tap(r.cx(), r.cy()); CHECK(resets == 1 && g_Settings.blurAlpha == 170);
        CHECK(Item("Menu size", r)); ScrollTo(D().scrollMax + 50);
        ScrollTo(0); CHECK(D().scrollY < 2);
    }
    printf("   menu size\n");
    {
        Frame(); R4 r; Item("Show pause button", r); float h12 = r.h(); R4 w12 = Rc(D().win); (void)w12;
        g_Settings.menuScale = 2.0f; Idle(2); Item("Show pause button", r); float h20 = r.h(); printf("    row height %.1f -> %.1f\n", h12, h20); CHECK(h20 > h12 * 1.45f);
        { Bright b = BrightIn(r, 0, 200); CHECK(b.n > 100); }
        g_Settings.menuScale = 0.7f; Idle(2); Item("Show pause button", r); CHECK(r.h() < h12 * 0.7f);
        g_Settings.menuScale = 1.2f; Idle(2);
        // the PAUSE button does NOT change with Menu size
        R4 p0 = Rc(D().pause); g_Settings.menuScale = 2.5f; Idle(2); R4 p1 = Rc(D().pause); CHECK(std::fabs(p1.w() - p0.w()) < 1.5f); g_Settings.menuScale = 1.2f; Idle(2);
    }
    printf("   move, collapse, resize, close\n");
    {
        Frame(); R4 w = Rc(D().win); ImGuiWindow* iw = ImGui::FindWindowByName("Cutscene Control Settings###ccwin"); float tb = iw->TitleBarHeight();
        Drag(w.cx(), w.t + tb * 0.5f, w.cx() + 120, w.t + tb * 0.5f + 70);
        R4 w2 = Rc(D().win); printf("    moved by (%.0f,%.0f)\n", w2.l - w.l, w2.t - w.t); CHECK(std::fabs((w2.l - w.l) - 120) < 4 && std::fabs((w2.t - w.t) - 70) < 4); CHECK(std::fabs(w2.w() - w.w()) < 1);
        CHECK(Down(0, w2.cx(), w2.cy())); Up(0, w2.cx(), w2.cy()); Idle();
        Drag(w2.cx(), w2.t + tb * 0.5f, w2.cx() - 1500, w2.t + tb * 0.5f - 600);                                  // fling it off the top-left: some of it stays reachable
        R4 w3 = Rc(D().win); CHECK(w3.r > 20 && w3.b > 20 && w3.l < W - 20 && w3.t < H - 20); CHECK(w3.t >= -1);
        CHECK(w3.r - w3.l > 120); Drag(w3.r - 90, w3.t + tb * 0.5f, (w3.r - 90) + (0.05f * W - w3.l), 0.04f * H + tb * 0.5f);                // grab the bit that stayed on screen and put it back near the top-left
        R4 w4 = Rc(D().win);
        // resize via the corner grip
        float gx = w4.r - 8, gy = w4.b - 8; Drag(gx, gy, gx + 140, std::min(gy + 60, H - 12.0f));
        R4 w5 = Rc(D().win); printf("    resized %.0fx%.0f -> %.0fx%.0f\n", w4.w(), w4.h(), w5.w(), w5.h()); CHECK(w5.w() > w4.w() + 100 && w5.h() >= w4.h()); CHECK(w5.b <= H + 1);
        gx = w5.r - 8; gy = w5.b - 8; Drag(gx, gy, gx - 800, gy - 600); R4 w6 = Rc(D().win);
        CHECK(w6.w() >= 0.2f * W && w6.h() >= 0.18f * H); CHECK(w6.w() < w5.w());                                   // can't be shrunk to nothing
        gx = w6.r - 8; gy = w6.b - 8; Drag(gx, gy, gx + 300, gy + 300);
        // collapse: tap the arrow
        Frame(); R4 w7 = Rc(D().win); CHECK(w7.l >= 0 && w7.t >= 0); Tap(w7.l + tb * 0.5f, w7.t + tb * 0.5f); UI::DebugInfo d = D(); CHECK(d.winCollapsed); R4 wc = Rc(d.win);
        printf("    collapsed to %.0fx%.0f\n", wc.w(), wc.h()); CHECK(wc.h() < tb * 1.3f);
        Save("/tmp/ui_window_collapsed.rgba");
        CHECK(!Down(0, wc.cx(), wc.b + 60)); Frame(); Up(0, wc.cx(), wc.b + 60); Idle();                          // below the title the game gets the touch again
        CHECK(Down(0, wc.cx(), wc.cy())); Frame(); Up(0, wc.cx(), wc.cy()); Idle();
        Tap(wc.l + tb * 0.5f, wc.t + tb * 0.5f); CHECK(!D().winCollapsed);                                         // expand again
        // close with the X
        saves = 0; CloseWindowViaX(); CHECK(!D().winOpen); CHECK(saves >= 1);                                       // settings are written when the window closes
        R4 wo = w7; CHECK(!Down(0, wo.cx(), wo.cy()) || Rc(D().pause).has(wo.cx(), wo.cy()) || Rc(D().set).has(wo.cx(), wo.cy())); Up(0, wo.cx(), wo.cy()); Idle();
        // SET toggles it open/closed again, and a window that was moved stays where it was
        OpenWindow(); CHECK(D().winOpen); { R4 s = Rc(D().set); saves = 0; Tap(s.cx(), s.cy()); CHECK(!D().winOpen); CHECK(saves >= 1); }
    }
    printf("   cutscene ends while the window is open\n");
    {
        OpenWindow(); CHECK(D().winOpen); R4 w = Rc(D().win); Down(0, w.cx(), w.cy()); Frame(); saves = 0;
        UI::OnCutsceneEnded(); Frame(); CHECK(!D().winOpen); CHECK(saves >= 1);
        UI::SetActive(false); CHECK(!Down(0, w.cx(), w.cy())); UI::SetActive(true); Idle(4);                        // the finger that was down never gets stuck
        // inactive -> active again: hit-rects are only valid after one frame
        Frame(); CHECK(D().pauseShown);
    }


    printf("== F. see-through PAUSE / RESUME / SET labels ==\n");
    {
        g_inCutscene = true; g_bCutscenePaused = false; g_Settings = Settings(); UI::OnCutsceneEnded(); Idle(2);
        CHECK(g_Settings.buttonPlateAlpha == 0 && g_Settings.labelAlpha == 170);                       // the defaults: no plate, translucent text
        struct St { float maxR = 0, sumR = 0; int vis = 0; };
        auto stat = [&](R4 rc, float inset) { St o; for (int y = (int)(rc.t + inset); y < (int)(rc.b - inset); ++y) for (int x = (int)(rc.l + inset); x < (int)(rc.r - inset); ++x)
                                                  { Px q = At(x, y); o.maxR = std::max(o.maxR, q.r); o.sumR += q.r; if (q.r > 100) ++o.vis; } return o; };
        Frame(false); R4 p = Rc(D().pause); R4 s = Rc(D().set);
        St a170 = stat(p, 5);
        printf("    A=170: brightest label pixel r=%.0f  pixels>100: %d  plate corner alpha=%.0f\n", a170.maxR, a170.vis, At((int)p.l + 8, (int)p.t + 8).a);
        CHECK(At((int)p.l + 8, (int)p.t + 8).a == 0 && At((int)p.r - 8, (int)p.b - 8).a == 0);        // plate fully transparent
        CHECK(a170.maxR > 160 && a170.maxR < 182);                                                     // text is ~67% white, not 100%
        CHECK(a170.vis > 200);                                                                         // ...but still plainly readable
        CHECK(stat(s, 3).maxR > 160 && stat(s, 3).maxR < 182);                                         // SET follows the same setting
        Frame(true, "PAUSED"); St r170 = stat(Rc(D().pause), 5); CHECK(r170.maxR > 160 && r170.maxR < 182 && r170.vis > 250);   // RESUME too
        { R4 q = Rc(D().pause); Px g = At((int)q.cx(), (int)q.t); printf("    paused outline premult rgba = %.0f %.0f %.0f %.0f\n", g.r, g.g, g.b, g.a); CHECK(g.r > 100 && g.r > g.g * 1.1f && g.g > g.b * 2.0f); }   // gold, translucent
        g_Settings.labelAlpha = 255; Frame(false); St a255 = stat(p, 5); CHECK(a255.maxR > 245);
        g_Settings.labelAlpha = 85;  Frame(false); St a85  = stat(p, 5); CHECK(a85.maxR > 75 && a85.maxR < 100 && a85.vis == 0);
        CHECK(a85.sumR < a170.sumR && a170.sumR < a255.sumR);                                           // strictly follows the slider
        g_Settings.labelAlpha = 0; Frame(false); St a0 = stat(p, 5);
        CHECK(a0.maxR == 0);                                                                           // no label at all
        CHECK(At((int)p.cx(), (int)p.t).a > 20);                                                       // ...but the outline keeps the button findable
        { bool hit = Down(0, p.cx(), p.cy()); Frame(); Up(0, p.cx(), p.cy()); Idle(8); CHECK(hit); CHECK(g_bCutscenePaused); g_bCutscenePaused = false; }   // and it is still tappable
        g_Settings.labelAlpha = 170; Frame(false);
        // the plate slider still works on top of a transparent label
        g_Settings.buttonPlateAlpha = 200; Frame(false); CHECK(At((int)p.l + 8, (int)p.t + 8).a > 190); g_Settings.buttonPlateAlpha = 0;
        // the in-window slider
        OpenWindow(); ScrollTo(0); Frame(); R4 r; CHECK(Item("Label opacity", r));
        Tap(r.l + r.w() * 0.50f, r.cy()); printf("    label opacity after tap@50%% = %d\n", g_Settings.labelAlpha); CHECK(g_Settings.labelAlpha > 105 && g_Settings.labelAlpha < 150);
        Frame(false); { St m = stat(Rc(D().pause), 5); CHECK(std::fabs(m.maxR - g_Settings.labelAlpha) < 14); }                  // the real button follows at once
        Drag(r.l + r.w() * 0.5f, r.cy(), r.l + r.w() * 5.0f, r.cy()); CHECK(g_Settings.labelAlpha == 255);
        g_Settings.labelAlpha = 170;
        // a long custom label keeps the same transparency and stays inside the plate
        strcpy(g_Settings.resumeButtonLabel, "CONTINUE THE SCENE"); Frame(true); { R4 q = Rc(D().pause); Bright b = BrightIn(q, 4, 100); CHECK(b.n > 300 && b.l > q.l && b.r < q.r); } strcpy(g_Settings.resumeButtonLabel, "RESUME");
        { R4 w = Rc(D().win); (void)w; CloseWindowViaX(); CHECK(!D().winOpen); }
        Save("/tmp/ui_transparent.rgba"); Frame(true, "PAUSED"); Save("/tmp/ui_transparent_paused.rgba");
    }

    printf("== G. \"Cutscene Controller Settings\" button outside cutscenes ==\n");
    {
        g_inCutscene = false; g_bCutscenePaused = false; g_Settings = Settings(); g_Settings.labelAlpha = 255; UI::OnCutsceneEnded(); Idle(3);
        Frame(); UI::DebugInfo d = D(); R4 s = Rc(d.set);
        printf("    gameplay button %.0fx%.0f at (%.0f,%.0f)\n", s.w(), s.h(), s.l, s.t);
        CHECK(d.gameplay && d.setShown && !d.pauseShown && !d.winOpen);                               // no PAUSE button when not in a cutscene
        CHECK(D().pauseLabel[0] == 0);
        CHECK(std::fabs(s.cx() - 0.50f * W) < 2 && std::fabs(s.cy() - 0.045f * H) < 2 && s.t >= 0);       // default: top-centre (clear of the radar, the timer / money block and the right-hand buttons)
        CHECK(s.l > 0.17f * W && s.r < 0.80f * W);
        CHECK(s.w() > 0.10f * W && s.w() < 0.30f * W && s.h() > 0.03f * H && s.h() < 0.09f * H);
        Bright b = BrightIn(s, 4); printf("    label pixels=%d bbox %.0f..%.0f\n", b.n, b.l, b.r);
        CHECK(b.n > 400); CHECK(b.l > s.l && b.r < s.r && b.t > s.t && b.b < s.b);                       // 28 characters, all inside the button
        CHECK(std::fabs((b.l + b.r) / 2 - s.cx()) < s.w() * 0.04f);                                    // centred
        CHECK(b.r - b.l > s.w() * 0.80f);                                                               // the full name, not "SET"
        Save("/tmp/ui_gameplay_button.rgba");
        // X / Y = where the CENTRE of the button goes, in percent of the screen
        {
            struct XY { float x, y; } pts[] = { { 10, 50 }, { 90, 80 }, { 25.5f, 12.25f }, { 70, 3 } };
            for (const XY& q : pts)
            {
                g_Settings.gameplayButtonX = q.x; g_Settings.gameplayButtonY = q.y; Frame(); R4 r = Rc(D().set);
                printf("    X=%.1f Y=%.1f -> centre (%.0f,%.0f)\n", q.x, q.y, r.cx(), r.cy());
                CHECK(std::fabs(r.cx() - q.x * 0.01f * W) < 2 && std::fabs(r.cy() - q.y * 0.01f * H) < 2); CHECK(BrightIn(r, 4).n > 400);
            }
            // moving the button moves where touches are taken: the old spot is the game's again
            g_Settings.gameplayButtonX = 50; g_Settings.gameplayButtonY = 4.5f; Frame(); R4 old = Rc(D().set);
            g_Settings.gameplayButtonX = 10; g_Settings.gameplayButtonY = 50; Idle(3); R4 nw = Rc(D().set);
            CHECK(!Down(0, old.cx(), old.cy())); Frame(); Up(0, old.cx(), old.cy()); Idle(3); CHECK(!D().winOpen);
            CHECK(Tap(nw.cx(), nw.cy())); CHECK(D().winOpen); CloseWindowViaX(); CHECK(!D().winOpen);
            // the edges: the button is pulled back inside the screen, never cut off
            struct XY edges[] = { { 0, 0 }, { 100, 0 }, { 0, 100 }, { 100, 100 }, { -20, 150 } };
            for (const XY& q : edges)
            { g_Settings.gameplayButtonX = q.x; g_Settings.gameplayButtonY = q.y; Frame(); R4 r = Rc(D().set); CHECK(r.l >= 0 && r.t >= 0 && r.r <= W && r.b <= H); CHECK(BrightIn(r, 4).n > 400); }
            g_Settings.gameplayButtonX = 50; g_Settings.gameplayButtonY = 4.5f; Frame();
        }
        // labels
        strcpy(g_Settings.settingsButtonLabel, "CC Settings"); Frame(); { R4 q = Rc(D().set); CHECK(q.w() < s.w() * 0.6f); CHECK(BrightIn(q, 4).n > 150); }
        strcpy(g_Settings.settingsButtonLabel, ""); Frame(); { R4 q = Rc(D().set); CHECK(std::fabs(q.w() - s.w()) < 1.5f); CHECK(BrightIn(q, 4).n > 400); }   // blank .ini value -> default text
        strcpy(g_Settings.settingsButtonLabel, "Cutscene Controller Settings");
        // size follows "Button size"
        g_Settings.buttonScale = 2.0f; Frame(); CHECK(Rc(D().set).w() > s.w() * 1.9f); g_Settings.buttonScale = 0.5f; Frame(); CHECK(Rc(D().set).w() < s.w() * 0.6f); g_Settings.buttonScale = 1.0f;
        // touch: only the button is ours
        Frame(); s = Rc(D().set); toggles = 0;
        CHECK(!Down(0, W / 2.0f, H / 2.0f)); Frame(); Up(0, W / 2.0f, H / 2.0f); Idle(); CHECK(!D().winOpen);
        CHECK(Tap(s.cx(), s.cy())); d = D(); CHECK(d.winOpen && !d.winCollapsed); CHECK(toggles == 0);       // opens the settings window; never touches pause
        { Frame(); R4 w = Rc(D().win); CHECK(w.l >= 0 && w.t >= 0 && w.r <= W && w.b <= H); CHECK(BrightIn({ w.l + 28, w.t + 1, w.r - 30, w.t + 28 }, 0, 200).n > 200); }
        // the window opens beside / below the button, never on top of it (so the button can still close it again)
        { R4 w = Rc(d.win); R4 sb = Rc(D().set); CHECK(w.t >= sb.b || w.l >= sb.r || w.r <= sb.l); printf("    window %.0f,%.0f..%.0f,%.0f  button bottom %.0f\n", w.l, w.t, w.r, w.b, sb.b); CHECK(w.b <= H + 1);
          CHECK(Tap(sb.cx(), sb.cy())); CHECK(!D().winOpen); CHECK(Tap(sb.cx(), sb.cy())); CHECK(D().winOpen); }               // the button itself toggles the window
        {
            struct XY { float x, y; } pts[] = { { 50, 4.5f }, { 5, 5 }, { 95, 5 }, { 50, 95 }, { 5, 95 }, { 95, 95 }, { 50, 50 }, { 20, 50 } };
            for (const XY& q : pts)
            {
                g_Settings.gameplayButtonX = q.x; g_Settings.gameplayButtonY = q.y; CloseWindowViaX(); Frame(); R4 sb = Rc(D().set); OpenWindow(); R4 w = Rc(D().win);
                bool apart = w.t >= sb.b - 0.5f || w.b <= sb.t + 0.5f || w.l >= sb.r - 0.5f || w.r <= sb.l + 0.5f;
                printf("    button at %.1f,%.1f: %.0f,%.0f..%.0f,%.0f  window %.0f,%.0f..%.0f,%.0f\n", q.x, q.y, sb.l, sb.t, sb.r, sb.b, w.l, w.t, w.r, w.b);
                CHECK(apart); CHECK(w.t >= 0 && w.b <= H + 1 && w.h() >= 0.2f * H);
            }
        }
        g_Settings.gameplayButtonX = 50; g_Settings.gameplayButtonY = 4.5f; CloseWindowViaX(); Frame(); OpenWindow();
        // the window works exactly as during a cutscene
        { ScrollTo(0); Frame(); R4 r; bool b0 = g_Settings.showSubtitles; bool vis = false;
          for (int g = 0; g < 10 && !vis; ++g) { R4 cb = Rc(D().content); vis = Item("Show subtitles", r) && r.t >= cb.t + 4 && r.b <= cb.b - 4; if (!vis) ScrollTo(std::min(D().scrollMax, D().scrollY + 120)); }
          CHECK(vis); Tap(r.l + 20, r.cy()); CHECK(g_Settings.showSubtitles != b0); Tap(r.l + 20, r.cy()); CHECK(g_Settings.showSubtitles == b0); ScrollTo(0); }
        Frame(); Save("/tmp/ui_gameplay_window.rgba");
        // the "Show outside cutscenes" checkbox: switching it off hides the button but never the open window
        { R4 r; CHECK(Reach("Show outside cutscenes", r)); Tap(r.l + 20, r.cy()); CHECK(!g_Settings.showGameplayButton); Frame(); CHECK(!D().setShown && D().winOpen && D().win[2] > 0);
          CHECK(Reach("Show outside cutscenes", r)); Tap(r.l + 20, r.cy()); CHECK(g_Settings.showGameplayButton); Frame(); CHECK(D().setShown); }
        { R4 r; CHECK(Reach("X position", r)); CHECK(Reach("Y position", r)); }
        // close with the X, then switch the button off from the .ini: nothing at all is drawn
        saves = 0; CloseWindowViaX(); CHECK(!D().winOpen && saves >= 1);
        g_Settings.showGameplayButton = false; Frame(); CHECK(!D().setShown && CoveredPixels() == 0);
        CHECK(!Down(0, s.cx(), s.cy())); Up(0, s.cx(), s.cy()); Idle(3);                                // nothing there to catch the touch
        g_Settings.showGameplayButton = true; Frame();
        // cutscene starts: the same slot turns into PAUSE + SET; ends: back to the long button
        g_inCutscene = true; Frame(); d = D(); CHECK(!d.gameplay && d.pauseShown && d.setShown); CHECK(!strcmp(d.pauseLabel, "PAUSE")); CHECK(Rc(d.set).w() < 0.06f * W);
        g_inCutscene = false; Frame(); d = D(); CHECK(d.gameplay && !d.pauseShown && Rc(d.set).w() > 0.10f * W);
        // a window opened in gameplay is closed (and saved) when a cutscene ends
        OpenWindow(); CHECK(D().winOpen); saves = 0; UI::OnCutsceneEnded(); Frame(); CHECK(!D().winOpen && saves >= 1);
        g_inCutscene = true;
    }

    printf("== H. outline opacity ==\n");
    {
        g_inCutscene = true; g_drawUI = true; g_vignette = false; g_bCutscenePaused = false; g_Settings = Settings(); UI::OnCutsceneEnded(); Idle(2);
        CHECK(g_Settings.outlineAlpha == 170 && g_Settings.labelAlpha == 170 && g_Settings.buttonPlateAlpha == 0);
        auto edgeA = [&](R4 r) { return At((int)r.cx(), (int)r.t).a; };                             // top edge of the outline, away from the label
        Frame(false); float p170 = edgeA(Rc(D().pause)), s170 = edgeA(Rc(D().set));
        printf("    outline alpha @170: PAUSE %.1f  SET %.1f\n", p170, s170);
        CHECK(std::fabs(p170 - 93.5f) < 4 && std::fabs(s170 - 93.5f) < 4);                          // white 0.55 x 170/255
        g_Settings.outlineAlpha = 255; Frame(false); float p255 = edgeA(Rc(D().pause)), s255 = edgeA(Rc(D().set)); CHECK(std::fabs(p255 - 140.0f) < 4 && std::fabs(s255 - 140.0f) < 4);
        g_Settings.outlineAlpha = 85;  Frame(false); float p85 = edgeA(Rc(D().pause)); CHECK(p85 > 20 && p85 < p170 && p170 < p255);
        g_Settings.outlineAlpha = 0;   Frame(false); CHECK(edgeA(Rc(D().pause)) == 0 && edgeA(Rc(D().set)) == 0);   // no outline at all
        { R4 p = Rc(D().pause); int n = 0; for (int x = (int)p.l; x < (int)p.r; ++x) for (int y : { (int)p.t, (int)p.t + 1, (int)p.b - 1, (int)p.b - 2 }) if (At(x, y).a > 0.5f) ++n; CHECK(n == 0); }  // not even a corner pixel
        CHECK(BrightIn(Rc(D().pause), 5, 100).n > 200);                                              // the label is not touched by the outline slider
        // paused = gold
        g_Settings.outlineAlpha = 255; Frame(true, "PAUSED"); { R4 q = Rc(D().pause); Px g = At((int)q.cx(), (int)q.t); printf("    gold @255 alpha %.0f\n", g.a); CHECK(std::fabs(g.a - 161.5f) < 4 + 85 || g.a > 200); CHECK(g.r > g.g && g.g > g.b * 2.0f); }
        g_Settings.outlineAlpha = 0; Frame(true, "PAUSED"); CHECK(edgeA(Rc(D().pause)) == 0);
        // a transparent outline does not make the button untappable
        { R4 q = Rc(D().pause); g_bCutscenePaused = false; toggles = 0; CHECK(Tap(q.cx(), q.cy())); CHECK(toggles == 1); g_bCutscenePaused = false; }
        // lock-out guard: label, outline AND plate all at 0 would leave nothing to see or find; a faint outline stays
        g_Settings.labelAlpha = 0; g_Settings.outlineAlpha = 0; g_Settings.buttonPlateAlpha = 0; Frame(false);
        { float a = edgeA(Rc(D().pause)); printf("    all-zero guard outline alpha %.1f\n", a); CHECK(a > 25 && a < 45); CHECK(edgeA(Rc(D().set)) > 25); }
        g_Settings.buttonPlateAlpha = 100; Frame(false); CHECK(std::fabs(edgeA(Rc(D().pause)) - 100.0f) < 2);   // with a plate there is something to see: the zero is honoured (only the plate shows)
        g_Settings.buttonPlateAlpha = 0; g_Settings.labelAlpha = 170; g_Settings.outlineAlpha = 0; Frame(false); CHECK(edgeA(Rc(D().pause)) == 0);
        // the gameplay button follows it too
        g_inCutscene = false; g_Settings.outlineAlpha = 255; Frame(); { R4 r = Rc(D().set); CHECK(std::fabs(edgeA(r) - 140.0f) < 4); }
        g_Settings.outlineAlpha = 0; Frame(); CHECK(edgeA(Rc(D().set)) == 0 && BrightIn(Rc(D().set), 4, 100).n > 300);
        // the slider in the window
        g_inCutscene = true; g_Settings.outlineAlpha = 170; Frame(false); OpenWindow(); R4 r; CHECK(Reach("Outline opacity", r));
        Tap(r.l + r.w() * 0.25f, r.cy()); printf("    outline opacity after tap@25%% = %d\n", g_Settings.outlineAlpha); CHECK(g_Settings.outlineAlpha > 50 && g_Settings.outlineAlpha < 80);
        Frame(false); CHECK(std::fabs(edgeA(Rc(D().pause)) - 0.55f * g_Settings.outlineAlpha * 1.0f) < 5);
        Drag(r.l + r.w() * 0.25f, r.cy(), r.l - 900, r.cy()); CHECK(g_Settings.outlineAlpha == 0);
        g_Settings.outlineAlpha = 170; CloseWindowViaX();
    }

    printf("== I. game fonts (CFont) ==\n");
    {
        g_inCutscene = true; g_drawUI = true; g_vignette = false; g_bCutscenePaused = false; g_Settings = Settings(); g_Settings.labelAlpha = 255; UI::OnCutsceneEnded(); Idle(2);
        CHECK(g_Settings.fontStyle == -1);
        ClearPrinted(); Frame(false); CHECK(printedAll.empty() && flushes == 0);                        // default: Arial, CFont is never touched
        CHECK(BrightIn(Rc(D().pause), 5).n > 250);
        float widths[4] = {};
        for (int st = 0; st < 4; ++st)
        {
            g_Settings.fontStyle = st; ClearPrinted(); Frame(false);
            R4 p = Rc(D().pause), s = Rc(D().set); const Printed* pp = FindPrint("PAUSE"); const Printed* ps = FindPrint("SET");
            CHECK(pp && ps); if (!pp || !ps) continue;
            printf("    style %d: PAUSE box %.0fx%.0f text at (%.1f,%.1f) lineH %.1f w %.1f\n", st, p.w(), p.h(), pp->x, pp->y, pp->lineH, pp->w);
            CHECK(pp->style == st && ps->style == st);                                                  // the right game font
            CHECK(std::fabs(pp->x + pp->w / 2 - p.cx()) < 1.5f && std::fabs(pp->y + pp->lineH / 2 - p.cy()) < 1.5f);   // centred in the button
            CHECK(std::fabs(ps->x + ps->w / 2 - s.cx()) < 1.5f && std::fabs(ps->y + ps->lineH / 2 - s.cy()) < 1.5f);
            CHECK(std::fabs(pp->lineH - p.h() / 1.4f) < 1.5f && std::fabs(ps->lineH - pp->lineH * 0.5f) < 1.0f);   // same size relation as the Arial label
            CHECK(pp->col.a == 255 && pp->col.r == 255 && pp->col.g == 255 && pp->col.b == 255);
            const Printed* sh = FindShadow("PAUSE"); CHECK(sh && sh->col.a == 150 && sh->x > pp->x && sh->y > pp->y);   // drop shadow printed first, behind
            CHECK(printedAll.size() == 4 && flushes == 2);                                              // 2 labels x (shadow + text), one flush per label
            CHECK(BrightIn(p, 5).n < 20);                                                               // and no Arial label on top of it
            CHECK(pp->x > p.l && pp->x + pp->w < p.r);                                                  // fits in the plate
            widths[st] = p.w();
        }
        CHECK(widths[0] < widths[1] && widths[1] < widths[2] && widths[2] < widths[3]);                 // the button is as wide as that font's label needs
        g_Settings.fontStyle = 1;
        // RESUME / banner / info
        ClearPrinted(); Frame(true, "PAUSED", "Free camera   speed 0.30");
        { const Printed* r = FindPrint("RESUME"); const Printed* b = FindPrint("PAUSED"); const Printed* i = FindPrint("Free camera   speed 0.30"); R4 p = Rc(D().pause);
          CHECK(r && b && i); if (r && b && i)
          { CHECK(std::fabs(r->x + r->w / 2 - p.cx()) < 1.5f && std::fabs(r->y + r->lineH / 2 - p.cy()) < 1.5f);
            CHECK(std::fabs(b->x + b->w / 2 - W / 2.0f) < 2.0f && std::fabs(b->y - 0.15f * H) < 1.5f && b->lineH > 70 && b->style == 1);
            CHECK(std::fabs(i->x - 0.02f * W) < 1.5f && std::fabs(i->y - 0.90f * H) < 1.5f && i->style == 1);
            CHECK(FindShadow("PAUSED") && FindShadow("PAUSED")->col.a == 215); } }
        CHECK(BrightIn({ 0.2f * W, 0.08f * H, 0.8f * W, 0.45f * H }, 0, 200).n == 0);                    // no Arial banner either
        // translucent
        g_Settings.labelAlpha = 170; ClearPrinted(); Frame(false);
        { const Printed* pp = FindPrint("PAUSE"); CHECK(pp && pp->col.a == 170); const Printed* sh = FindShadow("PAUSE"); CHECK(sh && sh->col.a == 100); }
        g_Settings.labelAlpha = 255;
        // vertical nudge
        ClearPrinted(); Frame(false); float y0 = FindPrint("PAUSE")->y, lh = FindPrint("PAUSE")->lineH;
        g_Settings.fontYOffset = 20; ClearPrinted(); Frame(false); CHECK(std::fabs(FindPrint("PAUSE")->y - (y0 + 0.2f * lh)) < 0.6f);
        g_Settings.fontYOffset = -30; ClearPrinted(); Frame(false); CHECK(std::fabs(FindPrint("PAUSE")->y - (y0 - 0.3f * lh)) < 0.6f);
        g_Settings.fontYOffset = 0;
        // custom label, size, scale
        strcpy(g_Settings.pauseButtonLabel, "FREEZE"); ClearPrinted(); Frame(false); CHECK(FindPrint("FREEZE") != nullptr); strcpy(g_Settings.pauseButtonLabel, "PAUSE");
        g_Settings.buttonScale = 2.0f; ClearPrinted(); Frame(false); CHECK(std::fabs(FindPrint("PAUSE")->lineH - 2.0f * lh) < 1.5f); g_Settings.buttonScale = 1.0f;
        // '~' would start a control token in the game's text: never forwarded
        strcpy(g_Settings.pauseButtonLabel, "A~r~B"); ClearPrinted(); Frame(false); CHECK(FindPrint("ArB") != nullptr); strcpy(g_Settings.pauseButtonLabel, "PAUSE");
        // CFont's global state is exactly as the game left it
        memset(fkDet, 0xAB, sizeof(fkDet)); memset(fkRS, 0xCD, sizeof(fkRS));
        ClearPrinted(); Frame(true, "PAUSED", "info");
        { bool det = true, rs = true; for (uint8_t b : fkDet) det &= (b == 0xAB); for (uint8_t b : fkRS) rs &= (b == 0xCD); CHECK(printedAll.size() > 4); CHECK(det && rs); }
        // draw order: the callbacks sit inside each button's own draw list, so a window opened later still paints over them
        OpenWindow(); ClearPrinted(); Frame(false); { printf("    draw calls after the last text flush: %d\n", drawSeq - flushDrawSeq); CHECK(flushes >= 2 && drawSeq - flushDrawSeq > 0); }
        // the settings window itself keeps ImGui's Arial
        { R4 w = Rc(D().win); CHECK(BrightIn({ w.l + 28, w.t + 1, w.r - 30, w.t + 30 }, 0, 200).n > 200); R4 r; ScrollTo(0); Frame(false); CHECK(Item("Show pause button", r)); CHECK(BrightIn(r, 0, 200).n > 100); }
        // the combo in the window
        { R4 r; CHECK(Reach("Font style", r)); Tap(r.cx() - 40, r.cy()); ImGuiWindow* pop = PopupWindow(); CHECK(pop != nullptr);
          if (pop) { ImGuiStyle& st = ImGui::GetStyle(); float fh = ImGui::GetFontSize(), pitch = fh + st.ItemSpacing.y;
                     Tap(pop->Pos.x + pop->Size.x * 0.5f, pop->Pos.y + st.WindowPadding.y + 4 * pitch + fh * 0.5f);
                     printf("    Font style after picking the 5th entry = %d\n", g_Settings.fontStyle); CHECK(g_Settings.fontStyle == 3);
                     Frame(false); ClearPrinted(); Frame(false); CHECK(FindPrint("PAUSE") && FindPrint("PAUSE")->style == 3);
                     CHECK(Reach("Font style", r)); Tap(r.cx() - 40, r.cy()); pop = PopupWindow(); CHECK(pop != nullptr);
                     if (pop) Tap(pop->Pos.x + pop->Size.x * 0.5f, pop->Pos.y + st.WindowPadding.y + 0 * pitch + fh * 0.5f);
                     CHECK(g_Settings.fontStyle == -1); ClearPrinted(); Frame(false); CHECK(printedAll.empty() && BrightIn(Rc(D().pause), 5).n > 250); }
          g_Settings.fontStyle = 2; CHECK(Reach("Font vertical nudge", r)); Tap(r.l + r.w() * 0.75f, r.cy());
          printf("    nudge after tap@75%% = %d\n", g_Settings.fontYOffset); CHECK(g_Settings.fontYOffset > 14 && g_Settings.fontYOffset < 26); g_Settings.fontYOffset = 0; }
        CloseWindowViaX();
        // gameplay button
        g_inCutscene = false; g_Settings.fontStyle = 3; ClearPrinted(); Frame();
        { const Printed* g = FindPrint("Cutscene Controller Settings"); R4 s = Rc(D().set); CHECK(g != nullptr); if (g) { CHECK(g->style == 3 && std::fabs(g->x + g->w / 2 - s.cx()) < 1.5f && std::fabs(g->y + g->lineH / 2 - s.cy()) < 1.5f); CHECK(g->x >= s.l && g->x + g->w <= s.r); }
          CHECK(!D().pauseShown); CHECK(BrightIn(s, 4).n < 20); }
        g_inCutscene = true;
        // fall-backs: the UI must never end up without a visible label
        g_Settings.labelAlpha = 255; g_Settings.fontStyle = 2; fkWidthZero = true; ClearPrinted(); Frame(false); CHECK(printedAll.empty() && BrightIn(Rc(D().pause), 5).n > 250); fkWidthZero = false;      // CFont measures 0 -> Arial
        { auto sv = Sym::CFont_PrintString; Sym::CFont_PrintString = nullptr; ClearPrinted(); Frame(false); CHECK(printedAll.empty() && BrightIn(Rc(D().pause), 5).n > 250); Sym::CFont_PrintString = sv; }   // symbol missing -> Arial
        { auto sv = Sym::CFont_Details; Sym::CFont_Details = nullptr; ClearPrinted(); Frame(false); CHECK(printedAll.empty() && BrightIn(Rc(D().pause), 5).n > 250); Sym::CFont_Details = sv; }
        ClearPrinted(); Frame(false); CHECK(FindPrint("PAUSE") != nullptr);                              // and it comes back
        g_Settings.fontStyle = -1; Frame(false);
        Save("/tmp/ui_font.rgba");
    }

    printf("== J. vignette ==\n");
    {
        g_inCutscene = true; g_drawUI = false; g_vignette = true; g_bCutscenePaused = false; g_Settings = Settings(); g_Settings.labelAlpha = 255; UI::OnCutsceneEnded(); Idle(2);
        CHECK(g_Settings.showVignette && g_Settings.vignetteStrength == 160 && g_Settings.vignetteSize == 50);
        Frame(false); CHECK(!D().pauseShown && !D().setShown);                                           // vignette only, no buttons
        auto sumA = [&]() { double t = 0; for (size_t i = 3; i < fb.size(); i += 4) t += fb[i]; return t; };
        printf("    centre %.0f  left-middle %.0f  corner %.0f\n", At(W / 2, H / 2).a, At(0, H / 2).a, At(2, 2).a);
        CHECK(At(W / 2, H / 2).a == 0 && At(W / 2, H / 4).a < 1.0f);                                      // the middle of the picture is untouched
        CHECK(At(2, 2).a > 145 && At(2, 2).a < 162 && At(W - 3, H - 3).a > 145 && At(2, H - 3).a > 145 && At(W - 3, 2).a > 145);   // all four corners
        CHECK(At(0, H / 2).a > 48 && At(0, H / 2).a < 62 && At(W / 2, 0).a > 30);                          // edge middles: lighter
        { Px c = At(2, 2); CHECK(c.r == 0 && c.g == 0 && c.b == 0); Px m = At(0, H / 2); CHECK(m.r == 0 && m.g == 0 && m.b == 0); }   // pure black, no colour cast
        { float prev = -1, jump = 0; for (int x = 0; x < W / 2; ++x) { float a = At(x, H / 2).a; if (prev >= 0) jump = std::max(jump, std::fabs(a - prev)); CHECK(a <= prev + 0.01f || prev < 0); prev = a; } printf("    biggest step between neighbouring pixels: %.1f\n", jump); CHECK(jump < 4.0f); }   // smooth fall-off, no banding / edge
        { bool sym = true; for (int i = 0; i < 40; ++i) { int x = 37 * i + 5, y = 17 * i % H; sym &= std::fabs(At(x, y).a - At(W - 1 - x, H - 1 - y).a) < 2.5f; } CHECK(sym); }   // symmetric
        // strength
        g_Settings.vignetteStrength = 255; Frame(false); CHECK(At(2, 2).a > 235); double s255 = sumA();
        g_Settings.vignetteStrength = 80;  Frame(false); CHECK(At(2, 2).a > 70 && At(2, 2).a < 84); double s80 = sumA();
        g_Settings.vignetteStrength = 160; Frame(false); double s160 = sumA(); CHECK(s80 < s160 && s160 < s255);
        g_Settings.vignetteStrength = 0;   Frame(false); CHECK(CoveredPixels() == 0);
        g_Settings.vignetteStrength = 160;
        // size
        g_Settings.vignetteSize = 0;   Frame(false); double z0 = sumA(); float lm0 = At(0, H / 2).a, c0 = At(2, 2).a; CHECK(lm0 == 0 && c0 > 115 && c0 < 145);
        g_Settings.vignetteSize = 100; Frame(false); double z100 = sumA(); float lm100 = At(0, H / 2).a, q = At(3 * W / 4, H / 2).a; CHECK(lm100 > 95 && q > 10 && At(W / 2, H / 2).a == 0);
        g_Settings.vignetteSize = 50;  Frame(false); double z50 = sumA(); printf("    size 0/50/100 total alpha: %.0f %.0f %.0f\n", z0, z50, z100); CHECK(z0 < z50 && z50 < z100);
        // vignette + UI together: the buttons and their text are on top of it, and the vignette adds no touch area
        g_drawUI = true; Frame(false); { UI::DebugInfo d = D(); CHECK(d.pauseShown && d.setShown); R4 p = Rc(d.pause); CHECK(BrightIn(p, 5).n > 250); CHECK(At(2, 2).a > 145); toggles = 0; CHECK(!Down(0, W / 2.0f, H / 2.0f)); Frame(); Up(0, W / 2.0f, H / 2.0f); Idle(3); CHECK(toggles == 0 && !D().winOpen); CHECK(!Down(1, 3, 3)); Up(1, 3, 3); Idle(2); }
        // vignette off in this frame: nothing but the UI
        g_vignette = false; Frame(false); CHECK(At(2, 2).a == 0 && At(0, H / 2).a == 0 && At(W - 3, H - 3).a == 0);
        // other screen sizes: scales with the screen
        g_vignette = true; g_drawUI = false;
        for (auto& sz : std::vector<std::pair<int, int>>{ { 800, 480 }, { 2400, 1080 }, { 1280, 720 } }) { SetScreen(sz.first, sz.second); Frame(false); CHECK(At(W / 2, H / 2).a == 0 && At(2, 2).a > 140 && At(W - 3, H - 3).a > 140 && At(0, H / 2).a > 45); }
        SetScreen(1600, 720); Frame(false);
        // the settings in the window
        g_drawUI = true; g_vignette = true; Frame(false); OpenWindow(); R4 r;
        CHECK(Reach("Vignette strength", r)); Tap(r.l + r.w() * 0.50f, r.cy()); printf("    strength after tap@50%% = %d\n", g_Settings.vignetteStrength); CHECK(g_Settings.vignetteStrength > 112 && g_Settings.vignetteStrength < 145);
        Frame(false); CHECK(std::fabs(At(2, 2).a - 0.96f * g_Settings.vignetteStrength) < 8);
        CHECK(Reach("Vignette size", r)); Tap(r.l + r.w() * 0.20f, r.cy()); printf("    size after tap@20%% = %d\n", g_Settings.vignetteSize); CHECK(g_Settings.vignetteSize > 12 && g_Settings.vignetteSize < 28);
        { R4 c; bool v0 = g_Settings.showVignette; CHECK(Reach("Vignette in cutscenes", c)); Tap(c.l + 20, c.cy()); CHECK(g_Settings.showVignette != v0); Tap(c.l + 20, c.cy()); CHECK(g_Settings.showVignette == v0); }
        CloseWindowViaX(); g_drawUI = true; g_vignette = true; g_Settings.vignetteStrength = 160; g_Settings.vignetteSize = 50; g_Settings.labelAlpha = 170;
        Frame(false); Save("/tmp/ui_vignette.rgba"); Frame(true, "PAUSED"); Save("/tmp/ui_vignette_paused.rgba"); g_vignette = false;
    }

    printf("== K. hold-for-fast-forward button + freeze-frame row ==\n");
    {
        g_inCutscene = true; g_drawUI = true; g_vignette = false; g_ffButton = false; g_bCutscenePaused = false; g_bFastForwarding = false;
        g_Settings = Settings(); g_Settings.labelAlpha = 255; g_Settings.outlineAlpha = 255; UI::OnCutsceneEnded(); Idle(2);
        CHECK(!g_Settings.ffEnabled && g_Settings.ffScreenButton && g_Settings.ffSpeed == 2.0f && !g_Settings.freezeFrame);
        Frame(false); CHECK(!D().ffShown && !UI::FastForwardHeld());                                   // not offered: nothing drawn
        g_ffButton = true; Frame(false); UI::DebugInfo d = D(); R4 p = Rc(d.pause), s = Rc(d.set), f = Rc(d.ff);
        printf("    PAUSE %.0f..%.0f  SET %.0f..%.0f  2x %.0f..%.0f (y %.0f..%.0f)\n", p.l, p.r, s.l, s.r, f.l, f.r, f.t, f.b);
        CHECK(d.ffShown && d.pauseShown && d.setShown);
        CHECK(f.r < s.l && s.r < p.l && std::fabs((s.l - f.r) - (p.l - s.r)) < 1.5f);                  // [2x] [SET] [PAUSE], same gaps
        CHECK(std::fabs(f.cy() - s.cy()) < 1.5f && std::fabs(f.h() - s.h()) < 1.5f);                  // same size and line as SET
        CHECK(f.l >= 0 && f.r <= W);
        Bright b = BrightIn(f, 3); CHECK(b.n > 60 && b.l > f.l && b.r < f.r);                           // "2x" is readable and inside
        printf("    2x label pixels %d\n", b.n);
        // label follows the speed
        g_Settings.ffSpeed = 2.5f; Frame(false); R4 f25 = Rc(D().ff); CHECK(f25.w() > f.w());          // "2.5x" is longer than "2x"
        g_Settings.ffSpeed = 4.0f; Frame(false); CHECK(BrightIn(Rc(D().ff), 3).n > 60); g_Settings.ffSpeed = 2.0f; Frame(false);
        // hold semantics: held while the finger is down, released when it lifts - and it never toggles pause
        toggles = 0; f = Rc(D().ff);
        CHECK(!UI::FastForwardHeld());
        CHECK(Down(0, f.cx(), f.cy())); Frame(false); Frame(false); CHECK(UI::FastForwardHeld());      // swallowed from the game, held
        Frame(false); Frame(false); CHECK(UI::FastForwardHeld());                                      // and it stays held for as long as the finger stays
        Move(0, f.cx() + 30, f.cy() - 25); Frame(false); CHECK(UI::FastForwardHeld());                // a little wobble does not drop it
        CHECK(Up(0, f.cx() + 30, f.cy() - 25)); Frame(false); Frame(false); CHECK(!UI::FastForwardHeld());
        Idle(8); CHECK(toggles == 0 && !g_bCutscenePaused && !D().winOpen);                              // a hold is not a tap on PAUSE / SET
        // a tap holds for the frames it lasts and ends
        CHECK(Down(0, f.cx(), f.cy())); Frame(false); CHECK(UI::FastForwardHeld()); Up(0, f.cx(), f.cy()); Frame(false); Frame(false); CHECK(!UI::FastForwardHeld()); Idle(8);
        // highlight (gold) while the game is really fast-forwarding
        g_bFastForwarding = true; Frame(false); { R4 q = Rc(D().ff); Px g = At((int)q.cx(), (int)q.t); CHECK(g.r > g.g && g.g > g.b * 2.0f); } g_bFastForwarding = false; Frame(false);
        { R4 q = Rc(D().ff); Px g = At((int)q.cx(), (int)q.t); CHECK(std::fabs(g.r - g.g) < 6); }      // white when idle
        // the other buttons still work next to it
        { R4 q = Rc(D().pause); CHECK(Tap(q.cx(), q.cy())); CHECK(toggles == 1); g_bCutscenePaused = false; R4 w = Rc(D().set); CHECK(Tap(w.cx(), w.cy())); CHECK(D().winOpen); CloseWindowViaX(); }
        // hold released by every way the button can go away
        Frame(false); f = Rc(D().ff);
        Down(0, f.cx(), f.cy()); Frame(false); CHECK(UI::FastForwardHeld()); g_ffButton = false; Frame(false); CHECK(!UI::FastForwardHeld() && !D().ffShown); g_ffButton = true;     // no longer offered
        Up(0, f.cx(), f.cy()); Idle(6);
        Frame(false); f = Rc(D().ff); Down(0, f.cx(), f.cy()); Frame(false); CHECK(UI::FastForwardHeld()); Frame(true, "PAUSED"); CHECK(!UI::FastForwardHeld() && !D().ffShown); Up(0, f.cx(), f.cy()); Idle(6);  // paused: gone
        Frame(false); f = Rc(D().ff); Down(0, f.cx(), f.cy()); Frame(false); CHECK(UI::FastForwardHeld()); UI::SetActive(false); CHECK(!UI::FastForwardHeld()); UI::SetActive(true); Idle(3); Frame(false); { R4 q = Rc(D().set); CHECK(Tap(q.cx(), q.cy())); CHECK(D().winOpen); CloseWindowViaX(); }        // UI switched off with the finger still down: it comes back fully usable
        Frame(false); f = Rc(D().ff); Down(0, f.cx(), f.cy()); Frame(false); CHECK(UI::FastForwardHeld()); UI::OnCutsceneEnded(); CHECK(!UI::FastForwardHeld()); Idle(3); { R4 q = Rc(D().set); CHECK(Tap(q.cx(), q.cy())); CHECK(D().winOpen); CloseWindowViaX(); }                         // cutscene over
        Frame(false); f = Rc(D().ff); Down(0, f.cx(), f.cy()); Frame(false); CHECK(UI::FastForwardHeld()); g_inCutscene = false; Frame(); CHECK(!UI::FastForwardHeld() && !D().ffShown); Up(0, f.cx(), f.cy()); Idle(6); g_inCutscene = true; Idle(3);   // gameplay
        // each corner, the buttons stay in a row inside the screen
        for (int c = 0; c < 4; ++c)
        {
            g_Settings.buttonCorner = c; Frame(false); UI::DebugInfo e = D(); R4 pp = Rc(e.pause), ss = Rc(e.set), ff = Rc(e.ff); bool right = (c == 0 || c == 2);
            CHECK(e.ffShown && ff.l >= 0 && ff.r <= W && ff.t >= 0 && ff.b <= H);
            CHECK(right ? (ff.r < ss.l && ss.r < pp.l) : (ff.l > ss.r && ss.l > pp.r));
            CHECK(std::fabs(ff.cy() - ss.cy()) < 1.5f);
        }
        g_Settings.buttonCorner = 0;
        // without the PAUSE button the 2x button slots in beside SET; without SET beside PAUSE
        g_Settings.showPauseButton = false; Frame(false); { UI::DebugInfo e = D(); CHECK(e.ffShown && !e.pauseShown && Rc(e.ff).r < Rc(e.set).l); } g_Settings.showPauseButton = true;
        g_Settings.showSettingsButton = false; Frame(false); { UI::DebugInfo e = D(); CHECK(e.ffShown && !e.setShown && Rc(e.ff).r < Rc(e.pause).l); } g_Settings.showSettingsButton = true;
        // game font: "2x" goes through CFont like the other labels
        g_Settings.fontStyle = 3; ClearPrinted(); Frame(false); { const Printed* t = FindPrint("2x"); R4 q = Rc(D().ff); CHECK(t && t->style == 3 && std::fabs(t->x + t->w / 2 - q.cx()) < 1.5f && std::fabs(t->y + t->lineH / 2 - q.cy()) < 1.5f); } g_Settings.fontStyle = -1;
        // transparency settings apply to it too
        g_Settings.outlineAlpha = 0; Frame(false); { R4 q = Rc(D().ff); CHECK(At((int)q.cx(), (int)q.t).a == 0); } g_Settings.outlineAlpha = 255;
        g_Settings.labelAlpha = 85; Frame(false); { R4 q = Rc(D().ff); float mx = 0; for (int y = (int)q.t + 3; y < (int)q.b - 3; ++y) for (int x = (int)q.l + 3; x < (int)q.r - 3; ++x) mx = std::max(mx, At(x, y).r); CHECK(mx > 75 && mx < 100); } g_Settings.labelAlpha = 255;
        Save("/tmp/ui_ff.rgba");
        // the window rows
        Frame(false); OpenWindow(); R4 r;
        { bool v = g_Settings.freezeFrame; CHECK(Reach("Freeze frame (hides drift)", r)); Tap(r.l + 20, r.cy()); CHECK(g_Settings.freezeFrame != v); Tap(r.l + 20, r.cy()); CHECK(g_Settings.freezeFrame == v); }
        { bool v = g_Settings.ffEnabled; CHECK(Reach("Hold for fast forward", r)); Tap(r.l + 20, r.cy()); CHECK(g_Settings.ffEnabled != v); Tap(r.l + 20, r.cy()); CHECK(g_Settings.ffEnabled == v); }
        CHECK(Reach("Speed", r)); Tap(r.l + r.w() * 0.5f, r.cy()); printf("    speed after tap@50%% = %.2f\n", g_Settings.ffSpeed); CHECK(g_Settings.ffSpeed > 2.2f && g_Settings.ffSpeed < 2.9f);
        Drag(r.l + r.w() * 0.5f, r.cy(), r.l - 800, r.cy()); CHECK(g_Settings.ffSpeed == 1.25f); Drag(r.l + r.w() * 0.1f, r.cy(), r.l + r.w() * 6, r.cy()); CHECK(g_Settings.ffSpeed == 4.0f); g_Settings.ffSpeed = 2.0f;
        { bool v = g_Settings.ffScreenButton; CHECK(Reach("On-screen hold button", r)); Tap(r.l + 20, r.cy()); CHECK(g_Settings.ffScreenButton != v); Tap(r.l + 20, r.cy()); CHECK(g_Settings.ffScreenButton == v); }
        { bool v = g_Settings.ffMuteAudio; CHECK(Reach("Silence audio while held", r)); Tap(r.l + 20, r.cy()); CHECK(g_Settings.ffMuteAudio != v); Tap(r.l + 20, r.cy()); CHECK(g_Settings.ffMuteAudio == v); }
        { bool v = g_Settings.ffScripted; CHECK(Reach("Also in scripted scenes", r)); Tap(r.l + 20, r.cy()); CHECK(g_Settings.ffScripted != v); Tap(r.l + 20, r.cy()); CHECK(g_Settings.ffScripted == v); }
        { CHECK(Reach("Gamepad fast-forward", r)); Tap(r.cx() - 40, r.cy()); ImGuiWindow* pop = PopupWindow(); CHECK(pop != nullptr);
          if (pop) { ImGuiStyle& st = ImGui::GetStyle(); float fh = ImGui::GetFontSize(), pitch = fh + st.ItemSpacing.y; Tap(pop->Pos.x + pop->Size.x * 0.5f, pop->Pos.y + st.WindowPadding.y + 8 * pitch + fh * 0.5f);   // "R1" is the 9th entry
                     printf("    Gamepad fast-forward -> %d (R1 = %d)\n", (int)g_Settings.ffButton, (int)GamepadButton::R1); CHECK(g_Settings.ffButton == GamepadButton::R1); }
          g_Settings.ffButton = GamepadButton::R2; }
        CloseWindowViaX();
        g_ffButton = false;
    }

    printf("== E. failure paths never crash the game ==\n");
    {
        UI::Shutdown(); ImGuiRW::DebugClearFailure(); failSetFromImage = true; int alive = rastersAlive; FbClear();
        UI::FrameInfo fi; UI::Frame(fi); CHECK(!ImGuiRW::IsReady()); UI::Frame(fi); UI::Frame(fi); CHECK(!ImGuiRW::IsReady()); CHECK(CoveredPixels() == 0);   // raster upload failed: UI off, no crash, no retry storm
        CHECK(rastersAlive == alive);                                                                                                                  // nothing leaked
        failSetFromImage = false; ImGuiRW::DebugClearFailure();
        UI::Shutdown(); Sym::g_bImGuiBackendResolved = false; UI::Frame(fi); CHECK(!ImGuiRW::IsReady()); UI::Frame(fi);                                // symbols missing: same
        Sym::g_bImGuiBackendResolved = true; ImGuiRW::DebugClearFailure(); UI::Frame(fi); CHECK(ImGuiRW::IsReady());                                  // and it recovers when they are there
        Sym::RsGlobal = nullptr; ImGuiRW::DebugClearFailure(); UI::Shutdown(); UI::Frame(fi); CHECK(!ImGuiRW::IsReady());
        Sym::RsGlobal = &rsg; ImGuiRW::DebugClearFailure(); UI::Frame(fi); CHECK(ImGuiRW::IsReady());
    }
    CHECK(!badState);
    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}

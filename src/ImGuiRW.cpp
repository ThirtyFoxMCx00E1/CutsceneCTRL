// ImGuiRW.cpp -- RenderWare backend for Dear ImGui.
//
// This is CLEO ImGui's backends/imgui_impl_renderware.cpp (MatiDragon), kept structurally identical because
// that one is proven to work on the target game, with these changes:
//   * RenderWare functions come from our own Sym:: table (GameSymbols.cpp), resolved by name for BOTH
//     armeabi-v7a and arm64-v8a -- the plugin hard-codes armeabi-v7a PLT offsets.
//   * the font atlas upload honours the real RwImage stride and checks every step, so a failure is logged and
//     the UI turns itself off instead of crashing the game.
//   * the vertex buffer is freed with delete[] (it is allocated with new[]).
//   * fonts: the same Arial TTF as CLEO ImGui, plus a large heavy cut for the PAUSE / RESUME button.
#include "ImGuiRW.h"
#include "GameSymbols.h"
#include <mod/logger.h>
#include <cstring>
#include <cstdint>
#include <cfloat>
#include <algorithm>
#include "ArialFont.h"

namespace
{
    // RenderWare render states / values (numeric, same as CLEO ImGui's rwlpcore.h)
    enum
    {
        rwRENDERSTATETEXTURERASTER      = 1,
        rwRENDERSTATETEXTUREADDRESS     = 2,
        rwRENDERSTATEZTESTENABLE        = 6,
        rwRENDERSTATEZWRITEENABLE       = 8,
        rwRENDERSTATETEXTUREFILTER      = 9,
        rwRENDERSTATESRCBLEND           = 10,
        rwRENDERSTATEDESTBLEND          = 11,
        rwRENDERSTATEVERTEXALPHAENABLE  = 12,
        rwRENDERSTATEBORDERCOLOR        = 13,
        rwRENDERSTATEFOGENABLE          = 14,
        rwRENDERSTATECULLMODE           = 20,
        rwRENDERSTATEALPHATESTFUNCTION  = 29,
        rwRENDERSTATEALPHATESTFUNCTIONREF = 30,
        rwPRIMTYPETRILIST               = 3,
        rwRASTERTYPETEXTURE             = 4,
    };

    // The game's vertex for RwIm2DRenderIndexedPrimitive (RwOpenGLVertex): 28 bytes on both ABIs.
    struct RwIm2DVertex
    {
        float    x, y, z, rhw;
        uint32_t emissiveColor;
        float    u, v;
    };
    static_assert(sizeof(RwIm2DVertex) == 28, "RwIm2DVertex must match the game's vertex layout");

    // The leading fields of RwImage (natural layout, identical for the 32 and 64 bit game builds).
    struct RwImageLite
    {
        int32_t  flags, width, height, depth, stride;
        uint8_t* cpPixels;
        void*    palette;
    };

    bool        s_ready = false;
    bool        s_failed = false;
    void*       s_fontRaster = nullptr;
    RwIm2DVertex* s_vb = nullptr;
    int         s_vbCap = 0;
    ImFont*     s_fontUI = nullptr;
    ImFont*     s_fontBig = nullptr;
    float       s_buildH = 720.0f;

    // Same glyph ranges CLEO ImGui uses (Latin, Latin-1, Cyrillic, punctuation, currency).
    const ImWchar kRangesFull[] = {
        0x0020, 0x0080,  0x00A0, 0x00C0,  0x0400, 0x0460,  0x0490, 0x04A0,
        0x2010, 0x2040,  0x20A0, 0x20B0,  0x2110, 0x2130,  0 };
    // The big font only ever renders the button / banner text.
    const ImWchar kRangesBig[] = { 0x0020, 0x0100, 0x0400, 0x0460, 0 };

    bool BackendSymbolsOk()
    {
        return Sym::g_bImGuiBackendResolved && Sym::RsGlobal && Sym::CSprite2d_NearScreenZ && Sym::CSprite2d_RecipNearClip;
    }

    bool CreateFontRaster()
    {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* px = nullptr; int w = 0, h = 0, bpp = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h, &bpp);
        if (!px || w <= 0 || h <= 0) { logger->Info("[CutsceneCtrl] ImGui: font atlas build failed"); return false; }
        logger->Info("[CutsceneCtrl] ImGui: font atlas %dx%d", w, h);

        void* img = Sym::RwImageCreate(w, h, 32);
        if (!img) { logger->Info("[CutsceneCtrl] ImGui: RwImageCreate failed"); return false; }
        Sym::RwImageAllocatePixels(img);
        RwImageLite* im = (RwImageLite*)img;
        if (!im->cpPixels || im->width != w || im->height != h)
        {
            logger->Info("[CutsceneCtrl] ImGui: RwImage has no pixel storage (%p, %dx%d)", (void*)im->cpPixels, im->width, im->height);
            Sym::RwImageDestroy(img);
            return false;
        }
        const int srcStride = w * 4;
        const int rowBytes = std::min(srcStride, im->stride > 0 ? im->stride : srcStride);
        for (int y = 0; y < h; ++y)
            memcpy(im->cpPixels + (size_t)y * (im->stride > 0 ? im->stride : srcStride), px + (size_t)y * srcStride, rowBytes);

        int rw = 0, rh = 0, rd = 0, rf = 0;
        Sym::RwImageFindRasterFormat(img, rwRASTERTYPETEXTURE, &rw, &rh, &rd, &rf);
        void* created = Sym::RwRasterCreate(rw, rh, rd, rf);
        void* raster = created ? Sym::RwRasterSetFromImage(created, img) : nullptr;
        Sym::RwImageDestroy(img);
        if (!raster)
        {
            if (created) Sym::RwRasterDestroy(created);                 // don't leak the half-built raster
            logger->Info("[CutsceneCtrl] ImGui: could not create the font raster (%dx%d depth %d flags 0x%x)", rw, rh, rd, rf);
            return false;
        }

        s_fontRaster = raster;
        io.Fonts->SetTexID((ImTextureID)raster);
        io.Fonts->ClearTexData();            // the pixels now live in the raster
        logger->Info("[CutsceneCtrl] ImGui: font raster ready");
        return true;
    }
}

namespace ImGuiRW
{
    bool IsReady() { return s_ready; }
    ImFont* FontUI()  { return s_fontUI; }
    ImFont* FontBig() { return s_fontBig; }
    float   BuildHeight() { return s_buildH; }
    void    DebugClearFailure() { s_failed = false; }

    bool Init(float displayH)
    {
        if (s_ready) return true;
        if (s_failed) return false;
        if (!BackendSymbolsOk()) { s_failed = true; logger->Info("[CutsceneCtrl] ImGui: backend symbols missing -- UI disabled"); return false; }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;                                   // the window never persists to imgui.ini
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen | ImGuiConfigFlags_NoMouseCursorChange;
        io.MouseDrawCursor = false;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        io.ConfigInputTrickleEventQueue = true;                     // a tap (down+up in one frame) still registers as a click

        const float H = displayH > 0 ? displayH : 720.0f;
        s_buildH = H;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;                           // arialData is static; ImGui must never free it
        cfg.OversampleH = 2; cfg.OversampleV = 1; cfg.PixelSnapH = false;
        cfg.GlyphRanges = kRangesFull;
        s_fontUI = io.Fonts->AddFontFromMemoryTTF((void*)arialData, (int)sizeof(arialData), std::max(12.0f, 0.0295f * H), &cfg, kRangesFull);

        ImFontConfig bigCfg;
        bigCfg.FontDataOwnedByAtlas = false;
        bigCfg.OversampleH = 2; bigCfg.OversampleV = 2;
        bigCfg.RasterizerMultiply = 1.7f;                           // heavier strokes: the reference button is a bold face
        s_fontBig = io.Fonts->AddFontFromMemoryTTF((void*)arialData, (int)sizeof(arialData), std::max(20.0f, 0.058f * H), &bigCfg, kRangesBig);

        if (!s_fontUI || !s_fontBig)
        {
            logger->Info("[CutsceneCtrl] ImGui: could not load the TTF font");
            ImGui::DestroyContext(); s_fontUI = s_fontBig = nullptr; s_failed = true; return false;
        }
        io.FontDefault = s_fontUI;
        s_ready = true;
        logger->Info("[CutsceneCtrl] ImGui %s ready (RenderWare backend)", ImGui::GetVersion());
        return true;
    }

    bool NewFrame(float w, float h, float dt)
    {
        if (!s_ready) return false;
        if (!s_fontRaster)
        {
            if (!CreateFontRaster()) { Shutdown(); s_failed = true; return false; }
        }
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(w, h);
        io.DeltaTime = dt > 1e-5f ? dt : 1.0f / 60.0f;
        return true;
    }

    void RenderDrawData(ImDrawData* dd)
    {
        if (!s_ready || !dd || dd->CmdListsCount == 0 || !BackendSymbolsOk()) return;

        if (!s_vb || s_vbCap < dd->TotalVtxCount)
        {
            delete[] s_vb;
            s_vbCap = dd->TotalVtxCount + 5000;
            s_vb = new (std::nothrow) RwIm2DVertex[s_vbCap];
            if (!s_vb) { s_vbCap = 0; logger->Info("[CutsceneCtrl] ImGui: vertex buffer allocation failed"); return; }
        }

        const float nearZ = *Sym::CSprite2d_NearScreenZ, recipZ = *Sym::CSprite2d_RecipNearClip;
        RwIm2DVertex* dst = s_vb;
        for (int n = 0; n < dd->CmdListsCount; ++n)
        {
            const ImDrawList* cl = dd->CmdLists[n];
            const ImDrawVert* src = cl->VtxBuffer.Data;
            for (int i = 0; i < cl->VtxBuffer.Size; ++i, ++dst, ++src)
            {
                dst->x = src->pos.x; dst->y = src->pos.y; dst->z = nearZ; dst->rhw = recipZ;
                dst->emissiveColor = src->col;
                dst->u = src->uv.x; dst->v = src->uv.y;
            }
        }

        int vtxOffset = 0;
        for (int n = 0; n < dd->CmdListsCount; ++n)
        {
            const ImDrawList* cl = dd->CmdLists[n];
            const ImDrawIdx* idx = cl->IdxBuffer.Data;
            for (int c = 0; c < cl->CmdBuffer.Size; ++c)
            {
                const ImDrawCmd* pcmd = &cl->CmdBuffer[c];
                if (pcmd->UserCallback)
                {
                    if (pcmd->UserCallback != ImDrawCallback_ResetRenderState)       // (a magic value, not a function; our states are set per draw anyway)
                        pcmd->UserCallback(cl, pcmd);
                }
                else if (pcmd->ElemCount > 0)
                {
                    float scissor[4];                               // CRect: left, bottom, right, top
                    if (Sym::CWidget_SetScissor)
                    {
                        scissor[0] = pcmd->ClipRect.x; scissor[1] = pcmd->ClipRect.w; scissor[2] = pcmd->ClipRect.z; scissor[3] = pcmd->ClipRect.y;
                        Sym::CWidget_SetScissor(scissor);
                    }
                    Sym::RwRenderStateSet(rwRENDERSTATEZTESTENABLE,        (void*)0);
                    Sym::RwRenderStateSet(rwRENDERSTATEZWRITEENABLE,       (void*)0);
                    Sym::RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE,  (void*)1);
                    Sym::RwRenderStateSet(rwRENDERSTATESRCBLEND,           (void*)5);   // src alpha
                    Sym::RwRenderStateSet(rwRENDERSTATEDESTBLEND,          (void*)6);   // inverse src alpha
                    Sym::RwRenderStateSet(rwRENDERSTATEFOGENABLE,          (void*)0);
                    Sym::RwRenderStateSet(rwRENDERSTATECULLMODE,           (void*)1);   // none
                    Sym::RwRenderStateSet(rwRENDERSTATEBORDERCOLOR,        (void*)0);
                    Sym::RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION,  (void*)5);
                    Sym::RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)2);
                    Sym::RwRenderStateSet(rwRENDERSTATETEXTUREFILTER,      (void*)2);   // linear
                    Sym::RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS,     (void*)3);   // clamp
                    Sym::RwRenderStateSet(rwRENDERSTATETEXTURERASTER,      (void*)pcmd->GetTexID());
                    Sym::RwIm2DRenderIndexedPrimitive(rwPRIMTYPETRILIST, &s_vb[vtxOffset], cl->VtxBuffer.Size,
                                                      (uint16_t*)idx, (int32_t)pcmd->ElemCount);
                    Sym::RwRenderStateSet(rwRENDERSTATETEXTURERASTER,      (void*)0);
                    if (Sym::CWidget_SetScissor)
                    {
                        scissor[0] = scissor[1] = scissor[2] = scissor[3] = 0;               // clear the scissor again
                        Sym::CWidget_SetScissor(scissor);
                    }
                }
                idx += pcmd->ElemCount;
            }
            vtxOffset += cl->VtxBuffer.Size;
        }
    }

    void Shutdown()
    {
        if (s_fontRaster && Sym::RwRasterDestroy) Sym::RwRasterDestroy(s_fontRaster);
        s_fontRaster = nullptr;
        delete[] s_vb; s_vb = nullptr; s_vbCap = 0;
        if (s_ready) ImGui::DestroyContext();
        s_ready = false; s_fontUI = s_fontBig = nullptr;
    }
}

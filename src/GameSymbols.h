// GameSymbols.h
//
// Every symbol name in GameSymbols.cpp was verified to exist in the
// v2.00 libGTASA.so the mod was built against (both armeabi-v7a's
// .dynsym and arm64-v8a's .symtab/.dynsym were checked with readelf --
// run tools/verify_symbols.sh to re-check). They are resolved at runtime
// with aml->GetSym() rather than hardcoded as absolute addresses.
//
// If a symbol is missing, ResolveAll() logs exactly which one failed
// (logcat / AML log, tag "CutsceneCtrl") instead of crashing. "Core"
// symbols gate the whole mod; "optional" ones only disable the feature
// that needs them (e.g. no touch symbol -> no on-screen button).
//
// CALLING-CONVENTION NOTE: every function taking a struct/class parameter
// below takes it by pointer/reference. That is deliberate. Several game
// functions (CFont::SetColor & friends) are mangled as taking a CRGBA *by
// value* yet physically receive a hidden pointer (Itanium ABI "non-trivial
// for calls" classification by the compiler that built the game) -- passing
// such a struct by value from our modern toolchain crashes. See README
// v1.0.4 for the disassembly that proved it.
#pragma once
#include "GameTypes.h"

namespace Sym
{
    // Set once by ResolveAll(). False means "don't touch the game".
    extern bool g_bCoreResolved;

    // ---- globals -------------------------------------------------------
    extern bool*     CTimer_m_UserPause;
    extern bool*     CTimer_m_CodePause;
    extern uint8_t*  TheCamera;            // raw CCamera bytes, use CCameraOffsets::*

    extern bool*     CCutsceneMgr_ms_running;
    extern float*    CCutsceneMgr_ms_cutsceneTimer;
    extern void**    CPostEffects_pRasterFrontBuffer;
    extern float*    CTimer_ms_fTimeStep;
    extern float*    CTimer_ms_fTimeStepNonClipped;
    extern float*    CTimer_ms_fTimeScale;            // multiplies CTimer::Update's frame time (both timesteps + game ms); used by the 2x hold

    // ---- functions -------------------------------------------------------
    typedef CPad*   (*Fn_CPad_GetPad)(int32_t padNumber);
    typedef void    (*Fn_CCamera_Process)(void* self);
    typedef void    (*Fn_CCamera_SetCamPositionForFixedMode)(void* self, const CVector& vec, const CVector& source);
    typedef void    (*Fn_CCamera_TakeControlNoEntity)(void* self, const CVector& pos, int16_t switchType, int32_t camMode);
    typedef void    (*Fn_CCamera_TakeControlWithSpline)(void* self, int16_t switchType);
    typedef uint32_t(*Fn_CCamera_GetCutSceneFinishTime)(void* self);
    // Returns 0 = no fade, 1 = mid-fade, 2 = fully faded to black (alpha==255).
    // Verified by disassembly of the real function (see README v1.1.0).
    typedef int32_t (*Fn_CCamera_GetScreenFadeStatus)(void* self);
    typedef void    (*Fn_CCamera_RestoreWithJumpCut)(void* self);
    typedef bool    (*Fn_IsWideScreen)();

    typedef void    (*Fn_CCutsceneMgr_SkipCutscene)();
    typedef bool    (*Fn_CCutsceneMgr_IsCutsceneSkipButtonBeingPressed)();

    typedef void    (*Fn_CTheScripts_Process)();
    typedef bool    (*Fn_CTheScripts_IsPlayerOnAMission)();

    typedef unsigned short* (*Fn_AsciiToGxtChar)(const char* ascii, unsigned short* out);
    typedef void    (*Fn_CFont_SetScale)(float scale);
    typedef void    (*Fn_CFont_SetColor)(const CRGBA& color);
    typedef void    (*Fn_CFont_SetJustify)(uint8_t justify);              // text JUSTIFICATION on/off -- NOT alignment
    typedef void    (*Fn_CFont_SetProportional)(uint8_t on);
    typedef void    (*Fn_CFont_SetOrientation)(uint8_t orient);           // alignment: 0=centre 1=left 2=right (verified)
    typedef void    (*Fn_CFont_SetBackgroundColor)(const CRGBA& color);
    typedef void    (*Fn_CFont_SetBackground)(uint8_t enabled, uint8_t textured);
    typedef void    (*Fn_CFont_SetDropColor)(const CRGBA& color);
    typedef void    (*Fn_CFont_SetDropShadowPosition)(int8_t pos);
    typedef void    (*Fn_CFont_SetFontStyle)(uint8_t style);              // 1 = subtitles, 3 = Pricedown (verified)
    typedef void    (*Fn_CFont_SetWrapx)(float wrapx);
    typedef void    (*Fn_CFont_PrintString)(float x, float y, unsigned short* text);
    typedef float   (*Fn_CFont_GetStringWidth)(unsigned short* text, uint8_t spaces, uint8_t scriptText);
    typedef float   (*Fn_CFont_GetHeight)(uint8_t unk);
    typedef void    (*Fn_CFont_RenderFontBuffer)();

    typedef void*   (*Fn_RwRasterCreate)(int32_t width, int32_t height, int32_t depth, int32_t flags);
    typedef void    (*Fn_RwRasterDestroy)(void* raster);
    typedef void    (*Fn_RwRasterPushContext)(void* raster);
    typedef void    (*Fn_RwRasterPopContext)();
    typedef void    (*Fn_RwRasterRenderFast)(void* raster, int32_t x, int32_t y);
    typedef void*   (*Fn_RwTextureCreate)(void* raster);
    typedef void    (*Fn_RwTextureDestroy)(void* texture);

    typedef void    (*Fn_CSprite2d_Ctor)(void* self);
    typedef void    (*Fn_CSprite2d_Dtor)(void* self);
    typedef void    (*Fn_CSprite2d_Draw)(void* self, const CRect& rect, const CRGBA& color);
    // Static, untextured solid-colour quad; enables vertex alpha itself when a<255 (verified).
    typedef void    (*Fn_CSprite2d_DrawRect)(const CRect& rect, const CRGBA& color);

    // Both ignore `this` (verified by disassembly: they load a global and tail-call), take no args.
    typedef void    (*Fn_CAudioEngine_Void)(void* self);

    typedef int32_t (*Fn_OS_ScreenGet)();

    // ---- RenderWare 2D immediate mode, used by the Dear ImGui backend (ImGuiRW.cpp) ----------------
    // Mangled exactly as in CLEO ImGui's RenderWare.cpp; all verified present in the v2.00 armeabi-v7a
    // AND arm64-v8a libGTASA.so (tools/verify_symbols.sh).
    struct RsGlobalLite { const char* appName; int32_t maximumWidth; int32_t maximumHeight; };
    typedef void*   (*Fn_RwImageCreate)(int32_t width, int32_t height, int32_t depth);
    typedef int32_t (*Fn_RwImageDestroy)(void* image);
    typedef void*   (*Fn_RwImageAllocatePixels)(void* image);
    typedef void*   (*Fn_RwImageFindRasterFormat)(void* image, int32_t rasterType, int32_t* w, int32_t* h, int32_t* d, int32_t* flags);
    typedef void*   (*Fn_RwRasterSetFromImage)(void* raster, void* image);
    typedef int32_t (*Fn_RwRenderStateSet)(int32_t state, void* value);
    typedef int32_t (*Fn_RwIm2DRenderIndexedPrimitive)(int32_t primType, void* vertices, int32_t numVertices, uint16_t* indices, int32_t numIndices);
    typedef void    (*Fn_CWidget_SetScissor)(float* rect4);      // CRect = {left, bottom, right, top}

    extern Fn_CPad_GetPad                                  CPad_GetPad;
    extern Fn_CCamera_Process                              CCamera_Process;
    extern Fn_CCamera_SetCamPositionForFixedMode           CCamera_SetCamPositionForFixedMode;
    extern Fn_CCamera_TakeControlNoEntity                  CCamera_TakeControlNoEntity;
    extern Fn_CCamera_TakeControlWithSpline                CCamera_TakeControlWithSpline;
    extern Fn_CCamera_GetCutSceneFinishTime                CCamera_GetCutSceneFinishTime;
    extern Fn_CCamera_GetScreenFadeStatus                  CCamera_GetScreenFadeStatus;
    extern Fn_CCamera_RestoreWithJumpCut                   CCamera_RestoreWithJumpCut;
    extern Fn_IsWideScreen                                 IsWideScreen;

    extern Fn_CCutsceneMgr_SkipCutscene                    CCutsceneMgr_SkipCutscene;
    extern Fn_CCutsceneMgr_IsCutsceneSkipButtonBeingPressed CCutsceneMgr_IsCutsceneSkipButtonBeingPressed;

    extern Fn_CTheScripts_Process                          CTheScripts_Process;
    extern Fn_CTheScripts_IsPlayerOnAMission                CTheScripts_IsPlayerOnAMission;

    extern Fn_AsciiToGxtChar                               AsciiToGxtChar;
    extern Fn_CFont_SetScale                               CFont_SetScale;
    extern Fn_CFont_SetColor                               CFont_SetColor;
    extern Fn_CFont_SetJustify                             CFont_SetJustify;
    extern Fn_CFont_SetProportional                        CFont_SetProportional;
    extern Fn_CFont_SetOrientation                         CFont_SetOrientation;
    extern Fn_CFont_SetBackgroundColor                     CFont_SetBackgroundColor;
    extern Fn_CFont_SetBackground                          CFont_SetBackground;
    extern Fn_CFont_SetDropColor                           CFont_SetDropColor;
    extern Fn_CFont_SetDropShadowPosition                  CFont_SetDropShadowPosition;
    extern Fn_CFont_SetFontStyle                           CFont_SetFontStyle;
    extern Fn_CFont_SetWrapx                               CFont_SetWrapx;
    extern Fn_CFont_PrintString                            CFont_PrintString;
    extern Fn_CFont_GetStringWidth                         CFont_GetStringWidth;
    extern Fn_CFont_GetHeight                              CFont_GetHeight;
    extern Fn_CFont_RenderFontBuffer                       CFont_RenderFontBuffer;

    extern Fn_RwRasterCreate                               RwRasterCreate;
    extern Fn_RwRasterDestroy                               RwRasterDestroy;
    extern Fn_RwRasterPushContext                          RwRasterPushContext;
    extern Fn_RwRasterPopContext                           RwRasterPopContext;
    extern Fn_RwRasterRenderFast                           RwRasterRenderFast;
    extern Fn_RwTextureCreate                               RwTextureCreate;
    extern Fn_RwTextureDestroy                              RwTextureDestroy;

    extern Fn_CSprite2d_Ctor                                CSprite2d_Ctor;
    extern Fn_CSprite2d_Dtor                                CSprite2d_Dtor;
    extern Fn_CSprite2d_Draw                                CSprite2d_Draw;
    extern Fn_CSprite2d_DrawRect                            CSprite2d_DrawRect;

    extern Fn_CAudioEngine_Void                             CAudioEngine_PauseAllSounds;
    extern Fn_CAudioEngine_Void                             CAudioEngine_ResumeAllSounds;
    extern void*                                            AudioEngine;   // address of the global CAudioEngine

    extern Fn_OS_ScreenGet                                  OS_ScreenGetWidth;
    extern Fn_OS_ScreenGet                                  OS_ScreenGetHeight;

    extern RsGlobalLite*                                    RsGlobal;
    extern Fn_RwImageCreate                                 RwImageCreate;
    extern Fn_RwImageDestroy                                RwImageDestroy;
    extern Fn_RwImageAllocatePixels                         RwImageAllocatePixels;
    extern Fn_RwImageFindRasterFormat                       RwImageFindRasterFormat;
    extern Fn_RwRasterSetFromImage                          RwRasterSetFromImage;
    extern Fn_RwRenderStateSet                              RwRenderStateSet;
    extern Fn_RwIm2DRenderIndexedPrimitive                  RwIm2DRenderIndexedPrimitive;
    extern Fn_CWidget_SetScissor                            CWidget_SetScissor;
    extern float*                                           CSprite2d_NearScreenZ;
    extern float*                                           CSprite2d_RecipNearClip;
    // True when every RenderWare symbol the ImGui backend needs resolved.
    extern bool                                             g_bImGuiBackendResolved;
    // CFont's own state blocks (64 and 48 bytes on both ABIs, checked in libGTASA's symbol table). We snapshot and restore
    // them around our text so the game's HUD never sees anything we changed.
    extern uint8_t*                                         CFont_Details;
    extern uint8_t*                                         CFont_RenderState;

    // Raw addresses (passed to HOOK(), which patches them):
    extern uintptr_t addr_CCutsceneMgr_Update;
    extern uintptr_t addr_CHud_DrawMissionTitle;
    extern uintptr_t addr_CHud_DrawSubtitles;
    extern uintptr_t addr_COnscreenTimerEntry_Process;
    extern uintptr_t addr_CCutsceneMgr_IsCutsceneSkipButtonBeingPressed;
    extern uintptr_t addr_RenderEffects;
    // The game's 2D pass (HUD, radar, menus). The Dear ImGui overlay is drawn right after it -- the same
    // moment the CLEO ImGui plugin draws, so it always lands on top of the HUD.
    extern uintptr_t addr_Render2dStuff;
    // Low-level touch entry point: AND_TouchEvent(int type, int pointerId, int x, int y).
    // Decoded from disassembly: type 2 = press, 1/4 = release, anything else = move.
    extern uintptr_t addr_AND_TouchEvent;
    extern uintptr_t addr_CTimer_Update;

    // Resolves every symbol above. Returns true only if every symbol the
    // mod's CORE features need resolved; UI/blur symbols are best-effort
    // and individually gated at the call site.
    bool ResolveAll();
}

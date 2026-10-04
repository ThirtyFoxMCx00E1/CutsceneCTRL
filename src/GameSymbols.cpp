#include "GameSymbols.h"
#include <mod/amlmod.h>
#include <mod/logger.h>

namespace Sym
{
    bool g_bCoreResolved = false;

    bool*     CTimer_m_UserPause = nullptr;
    bool*     CTimer_m_CodePause = nullptr;
    uint8_t*  TheCamera = nullptr;
    bool*     CCutsceneMgr_ms_running = nullptr;
    float*    CCutsceneMgr_ms_cutsceneTimer = nullptr;
    void**    CPostEffects_pRasterFrontBuffer = nullptr;
    float*    CTimer_ms_fTimeStep = nullptr;
    float*    CTimer_ms_fTimeStepNonClipped = nullptr;
    float*    CTimer_ms_fTimeScale = nullptr;

    Fn_CPad_GetPad                                   CPad_GetPad = nullptr;
    Fn_CCamera_Process                               CCamera_Process = nullptr;
    Fn_CCamera_SetCamPositionForFixedMode            CCamera_SetCamPositionForFixedMode = nullptr;
    Fn_CCamera_TakeControlNoEntity                   CCamera_TakeControlNoEntity = nullptr;
    Fn_CCamera_TakeControlWithSpline                 CCamera_TakeControlWithSpline = nullptr;
    Fn_CCamera_GetCutSceneFinishTime                 CCamera_GetCutSceneFinishTime = nullptr;
    Fn_CCamera_GetScreenFadeStatus                   CCamera_GetScreenFadeStatus = nullptr;
    Fn_CCamera_RestoreWithJumpCut                    CCamera_RestoreWithJumpCut = nullptr;
    Fn_IsWideScreen                                  IsWideScreen = nullptr;

    Fn_CCutsceneMgr_SkipCutscene                     CCutsceneMgr_SkipCutscene = nullptr;
    Fn_CCutsceneMgr_IsCutsceneSkipButtonBeingPressed CCutsceneMgr_IsCutsceneSkipButtonBeingPressed = nullptr;

    Fn_CTheScripts_Process                           CTheScripts_Process = nullptr;
    Fn_CTheScripts_IsPlayerOnAMission                CTheScripts_IsPlayerOnAMission = nullptr;

    Fn_AsciiToGxtChar                                AsciiToGxtChar = nullptr;
    Fn_CFont_SetScale                                CFont_SetScale = nullptr;
    Fn_CFont_SetColor                                CFont_SetColor = nullptr;
    Fn_CFont_SetJustify                              CFont_SetJustify = nullptr;
    Fn_CFont_SetProportional                         CFont_SetProportional = nullptr;
    Fn_CFont_SetOrientation                          CFont_SetOrientation = nullptr;
    Fn_CFont_SetBackgroundColor                      CFont_SetBackgroundColor = nullptr;
    Fn_CFont_SetBackground                           CFont_SetBackground = nullptr;
    Fn_CFont_SetDropColor                            CFont_SetDropColor = nullptr;
    Fn_CFont_SetDropShadowPosition                   CFont_SetDropShadowPosition = nullptr;
    Fn_CFont_SetFontStyle                            CFont_SetFontStyle = nullptr;
    Fn_CFont_SetWrapx                                CFont_SetWrapx = nullptr;
    Fn_CFont_PrintString                             CFont_PrintString = nullptr;
    Fn_CFont_GetStringWidth                          CFont_GetStringWidth = nullptr;
    Fn_CFont_GetHeight                               CFont_GetHeight = nullptr;
    Fn_CFont_RenderFontBuffer                        CFont_RenderFontBuffer = nullptr;

    Fn_RwRasterCreate                                RwRasterCreate = nullptr;
    Fn_RwRasterDestroy                               RwRasterDestroy = nullptr;
    Fn_RwRasterPushContext                           RwRasterPushContext = nullptr;
    Fn_RwRasterPopContext                            RwRasterPopContext = nullptr;
    Fn_RwRasterRenderFast                            RwRasterRenderFast = nullptr;
    Fn_RwTextureCreate                               RwTextureCreate = nullptr;
    Fn_RwTextureDestroy                              RwTextureDestroy = nullptr;

    Fn_CSprite2d_Ctor                                CSprite2d_Ctor = nullptr;
    Fn_CSprite2d_Dtor                                CSprite2d_Dtor = nullptr;
    Fn_CSprite2d_Draw                                CSprite2d_Draw = nullptr;
    Fn_CSprite2d_DrawRect                            CSprite2d_DrawRect = nullptr;

    Fn_CAudioEngine_Void                             CAudioEngine_PauseAllSounds = nullptr;
    Fn_CAudioEngine_Void                             CAudioEngine_ResumeAllSounds = nullptr;
    void*                                            AudioEngine = nullptr;

    Fn_OS_ScreenGet                                  OS_ScreenGetWidth = nullptr;
    Fn_OS_ScreenGet                                  OS_ScreenGetHeight = nullptr;

    RsGlobalLite*                                    RsGlobal = nullptr;
    Fn_RwImageCreate                                 RwImageCreate = nullptr;
    Fn_RwImageDestroy                                RwImageDestroy = nullptr;
    Fn_RwImageAllocatePixels                         RwImageAllocatePixels = nullptr;
    Fn_RwImageFindRasterFormat                       RwImageFindRasterFormat = nullptr;
    Fn_RwRasterSetFromImage                          RwRasterSetFromImage = nullptr;
    Fn_RwRenderStateSet                              RwRenderStateSet = nullptr;
    Fn_RwIm2DRenderIndexedPrimitive                  RwIm2DRenderIndexedPrimitive = nullptr;
    Fn_CWidget_SetScissor                            CWidget_SetScissor = nullptr;
    float*                                           CSprite2d_NearScreenZ = nullptr;
    float*                                           CSprite2d_RecipNearClip = nullptr;
    bool                                             g_bImGuiBackendResolved = false;
    uint8_t*                                         CFont_Details = nullptr;
    uint8_t*                                         CFont_RenderState = nullptr;

    uintptr_t addr_CCutsceneMgr_Update = 0;
    uintptr_t addr_CHud_DrawMissionTitle = 0;
    uintptr_t addr_CHud_DrawSubtitles = 0;
    uintptr_t addr_COnscreenTimerEntry_Process = 0;
    uintptr_t addr_CCutsceneMgr_IsCutsceneSkipButtonBeingPressed = 0;
    uintptr_t addr_RenderEffects = 0;
    uintptr_t addr_Render2dStuff = 0;
    uintptr_t addr_AND_TouchEvent = 0;
    uintptr_t addr_CTimer_Update = 0;

    // Resolves one symbol into *out (as a raw address), logging a failure.
    static bool Resolve(void* lib, const char* mangled, const char* pretty, bool required, void* out)
    {
        uintptr_t addr = aml->GetSym(lib, mangled);
        *(uintptr_t*)out = addr;
        if (!addr)
            logger->Info("[CutsceneCtrl] %s symbol NOT FOUND: %s (%s)", required ? "REQUIRED" : "optional", pretty, mangled);
        return addr != 0;
    }

    // Each line: mangled name, pretty name (for the log), destination variable.
    #define R_REQ(mangled, pretty, var) (ok &= Resolve(lib, mangled, pretty, true,  &(var)))
    #define R_OPT(mangled, pretty, var) ((void)Resolve(lib, mangled, pretty, false, &(var)))

    bool ResolveAll()
    {
        void* lib = aml->GetLibHandle("libGTASA.so");
        if (!lib)
        {
            logger->Info("[CutsceneCtrl] libGTASA.so handle not found -- is the game loaded?");
            return false;
        }

        bool ok = true;

        // ---- core (the mod is inert without all of these) ----------------
        R_REQ("_ZN6CTimer11m_UserPauseE",                 "CTimer::m_UserPause",             CTimer_m_UserPause);
        R_REQ("_ZN6CTimer11m_CodePauseE",                 "CTimer::m_CodePause",             CTimer_m_CodePause);
        R_REQ("TheCamera",                                "TheCamera",                       TheCamera);
        R_REQ("_ZN12CCutsceneMgr10ms_runningE",           "CCutsceneMgr::ms_running",        CCutsceneMgr_ms_running);
        R_REQ("_ZN12CCutsceneMgr16ms_cutsceneTimerE",     "CCutsceneMgr::ms_cutsceneTimer",  CCutsceneMgr_ms_cutsceneTimer);
        R_REQ("_ZN4CPad6GetPadEi",                        "CPad::GetPad",                    CPad_GetPad);
        R_REQ("_ZN7CCamera7ProcessEv",                    "CCamera::Process",                CCamera_Process);
        R_REQ("_ZN7CCamera26SetCamPositionForFixedModeERK7CVectorS2_", "CCamera::SetCamPositionForFixedMode", CCamera_SetCamPositionForFixedMode);
        R_REQ("_ZN7CCamera19TakeControlNoEntityERK7CVectorsi", "CCamera::TakeControlNoEntity", CCamera_TakeControlNoEntity);
        R_REQ("_ZN7CCamera21TakeControlWithSplineEs",     "CCamera::TakeControlWithSpline",  CCamera_TakeControlWithSpline);
        R_REQ("_ZN7CCamera21GetCutSceneFinishTimeEv",     "CCamera::GetCutSceneFinishTime",  CCamera_GetCutSceneFinishTime);
        R_REQ("_ZN12CCutsceneMgr12SkipCutsceneEv",        "CCutsceneMgr::SkipCutscene",      CCutsceneMgr_SkipCutscene);
        R_REQ("_ZN12CCutsceneMgr32IsCutsceneSkipButtonBeingPressedEv", "CCutsceneMgr::IsCutsceneSkipButtonBeingPressed", addr_CCutsceneMgr_IsCutsceneSkipButtonBeingPressed);
        CCutsceneMgr_IsCutsceneSkipButtonBeingPressed = (Fn_CCutsceneMgr_IsCutsceneSkipButtonBeingPressed)addr_CCutsceneMgr_IsCutsceneSkipButtonBeingPressed;
        R_REQ("_ZN12CCutsceneMgr6UpdateEv",               "CCutsceneMgr::Update",            addr_CCutsceneMgr_Update);
        R_REQ("_ZN11CTheScripts7ProcessEv",               "CTheScripts::Process",            CTheScripts_Process);

        // ---- optional ----------------------------------------------------
        R_OPT("_ZN7CCamera18RestoreWithJumpCutEv",        "CCamera::RestoreWithJumpCut",     CCamera_RestoreWithJumpCut);
        R_OPT("_Z12IsWideScreenv",                        "IsWideScreen",                    IsWideScreen);
        R_OPT("_ZN11CTheScripts18IsPlayerOnAMissionEv",   "CTheScripts::IsPlayerOnAMission", CTheScripts_IsPlayerOnAMission);
        R_OPT("_ZN7CCamera19GetScreenFadeStatusEv",       "CCamera::GetScreenFadeStatus",    CCamera_GetScreenFadeStatus);
        R_OPT("_ZN12CPostEffects18pRasterFrontBufferE",   "CPostEffects::pRasterFrontBuffer",CPostEffects_pRasterFrontBuffer);

        R_OPT("_ZN4CHud16DrawMissionTitleEv",             "CHud::DrawMissionTitle",          addr_CHud_DrawMissionTitle);
        R_OPT("_ZN4CHud13DrawSubtitlesEv",                "CHud::DrawSubtitles",             addr_CHud_DrawSubtitles);
        R_OPT("_ZN19COnscreenTimerEntry7ProcessEv",       "COnscreenTimerEntry::Process",    addr_COnscreenTimerEntry_Process);
        R_OPT("_Z13RenderEffectsv",                       "RenderEffects",                   addr_RenderEffects);
        R_OPT("_ZN6CTimer6UpdateEv",                      "CTimer::Update",                  addr_CTimer_Update);
        R_OPT("_ZN6CTimer12ms_fTimeStepE",                "CTimer::ms_fTimeStep",            CTimer_ms_fTimeStep);
        R_OPT("_ZN6CTimer22ms_fTimeStepNonClippedE",      "CTimer::ms_fTimeStepNonClipped",  CTimer_ms_fTimeStepNonClipped);
        R_OPT("_ZN6CTimer13ms_fTimeScaleE",               "CTimer::ms_fTimeScale",           CTimer_ms_fTimeScale);
        R_OPT("_Z14AND_TouchEventiiii",                   "AND_TouchEvent",                  addr_AND_TouchEvent);

        R_OPT("_Z14AsciiToGxtCharPKcPt",                  "AsciiToGxtChar",                  AsciiToGxtChar);
        R_OPT("_ZN5CFont8SetScaleEf",                     "CFont::SetScale",                 CFont_SetScale);
        R_OPT("_ZN5CFont8SetColorE5CRGBA",                "CFont::SetColor",                 CFont_SetColor);
        R_OPT("_ZN5CFont10SetJustifyEh",                  "CFont::SetJustify",               CFont_SetJustify);
        R_OPT("_ZN5CFont15SetProportionalEh",             "CFont::SetProportional",          CFont_SetProportional);
        R_OPT("_ZN5CFont14SetOrientationEh",              "CFont::SetOrientation",           CFont_SetOrientation);
        R_OPT("_ZN5CFont18SetBackgroundColorE5CRGBA",     "CFont::SetBackgroundColor",       CFont_SetBackgroundColor);
        R_OPT("_ZN5CFont13SetBackgroundEhh",              "CFont::SetBackground",            CFont_SetBackground);
        R_OPT("_ZN5CFont12SetDropColorE5CRGBA",           "CFont::SetDropColor",             CFont_SetDropColor);
        R_OPT("_ZN5CFont21SetDropShadowPositionEa",       "CFont::SetDropShadowPosition",    CFont_SetDropShadowPosition);
        R_OPT("_ZN5CFont12SetFontStyleEh",                "CFont::SetFontStyle",             CFont_SetFontStyle);
        R_OPT("_ZN5CFont8SetWrapxEf",                     "CFont::SetWrapx",                 CFont_SetWrapx);
        R_OPT("_ZN5CFont11PrintStringEffPt",              "CFont::PrintString",              CFont_PrintString);
        R_OPT("_ZN5CFont14GetStringWidthEPthh",           "CFont::GetStringWidth",           CFont_GetStringWidth);
        R_OPT("_ZN5CFont9GetHeightEb",                    "CFont::GetHeight",                CFont_GetHeight);
        R_OPT("_ZN5CFont16RenderFontBufferEv",            "CFont::RenderFontBuffer",         CFont_RenderFontBuffer);

        R_OPT("_Z14RwRasterCreateiiii",                   "RwRasterCreate",                  RwRasterCreate);
        R_OPT("_Z15RwRasterDestroyP8RwRaster",            "RwRasterDestroy",                 RwRasterDestroy);
        R_OPT("_Z19RwRasterPushContextP8RwRaster",        "RwRasterPushContext",             RwRasterPushContext);
        R_OPT("_Z18RwRasterPopContextv",                  "RwRasterPopContext",              RwRasterPopContext);
        R_OPT("_Z18RwRasterRenderFastP8RwRasterii",       "RwRasterRenderFast",              RwRasterRenderFast);
        R_OPT("_Z15RwTextureCreateP8RwRaster",            "RwTextureCreate",                 RwTextureCreate);
        R_OPT("_Z16RwTextureDestroyP9RwTexture",          "RwTextureDestroy",                RwTextureDestroy);

        R_OPT("_ZN9CSprite2dC1Ev",                        "CSprite2d::CSprite2d",            CSprite2d_Ctor);
        R_OPT("_ZN9CSprite2dD1Ev",                        "CSprite2d::~CSprite2d",           CSprite2d_Dtor);
        R_OPT("_ZN9CSprite2d4DrawERK5CRectRK5CRGBA",      "CSprite2d::Draw",                 CSprite2d_Draw);
        R_OPT("_ZN9CSprite2d8DrawRectERK5CRectRK5CRGBA",  "CSprite2d::DrawRect",             CSprite2d_DrawRect);

        R_OPT("_ZN12CAudioEngine14PauseAllSoundsEv",      "CAudioEngine::PauseAllSounds",    CAudioEngine_PauseAllSounds);
        R_OPT("_ZN12CAudioEngine15ResumeAllSoundsEv",     "CAudioEngine::ResumeAllSounds",   CAudioEngine_ResumeAllSounds);
        R_OPT("AudioEngine",                              "AudioEngine",                     AudioEngine);

        // ---- Dear ImGui / RenderWare backend (all required for the on-screen UI; the pause logic works without) ----
        {
            bool b = true;
            b &= Resolve(lib, "RsGlobal",                                            "RsGlobal",                       false, &RsGlobal);
            b &= Resolve(lib, "_Z13RwImageCreateiii",                                "RwImageCreate",                  false, &RwImageCreate);
            b &= Resolve(lib, "_Z14RwImageDestroyP7RwImage",                         "RwImageDestroy",                 false, &RwImageDestroy);
            b &= Resolve(lib, "_Z21RwImageAllocatePixelsP7RwImage",                  "RwImageAllocatePixels",          false, &RwImageAllocatePixels);
            b &= Resolve(lib, "_Z23RwImageFindRasterFormatP7RwImageiPiS1_S1_S1_",    "RwImageFindRasterFormat",        false, &RwImageFindRasterFormat);
            b &= Resolve(lib, "_Z20RwRasterSetFromImageP8RwRasterP7RwImage",         "RwRasterSetFromImage",           false, &RwRasterSetFromImage);
            b &= Resolve(lib, "_Z16RwRenderStateSet13RwRenderStatePv",               "RwRenderStateSet",               false, &RwRenderStateSet);
            b &= Resolve(lib, "_Z28RwIm2DRenderIndexedPrimitive15RwPrimitiveTypeP14RwOpenGLVertexiPti", "RwIm2DRenderIndexedPrimitive", false, &RwIm2DRenderIndexedPrimitive);
            b &= Resolve(lib, "_ZN9CSprite2d11NearScreenZE",                         "CSprite2d::NearScreenZ",         false, &CSprite2d_NearScreenZ);
            b &= Resolve(lib, "_ZN9CSprite2d13RecipNearClipE",                       "CSprite2d::RecipNearClip",       false, &CSprite2d_RecipNearClip);
            b &= (RwRasterCreate != nullptr);
            b &= (RwRasterDestroy != nullptr);
            (void)Resolve(lib, "_ZN7CWidget10SetScissorER5CRect",                     "CWidget::SetScissor",            false, &CWidget_SetScissor);   // optional: only clips the settings window precisely
            g_bImGuiBackendResolved = b;
            if (!b) logger->Info("[CutsceneCtrl] RenderWare/ImGui symbols incomplete -- the on-screen UI is disabled (pause logic still works).");
        }

        (void)Resolve(lib, "_ZN5CFont7DetailsE",     "CFont::Details",     false, &CFont_Details);
        (void)Resolve(lib, "_ZN5CFont11RenderStateE", "CFont::RenderState", false, &CFont_RenderState);
        R_OPT("_Z13Render2dStuffv",                       "Render2dStuff",                   addr_Render2dStuff);
        R_OPT("_Z17OS_ScreenGetWidthv",                   "OS_ScreenGetWidth",               OS_ScreenGetWidth);
        R_OPT("_Z18OS_ScreenGetHeightv",                  "OS_ScreenGetHeight",              OS_ScreenGetHeight);

        g_bCoreResolved = ok;
        logger->Info("[CutsceneCtrl] Symbol resolution %s (core features %s)",
                     ok ? "complete" : "INCOMPLETE", ok ? "enabled" : "DISABLED -- see errors above");
        return ok;
    }
}

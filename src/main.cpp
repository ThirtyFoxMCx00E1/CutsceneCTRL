#include <mod/amlmod.h>
#include <mod/iaml.h>
#include <mod/logger.h>
#include <mod/config.h>
#include "GameSymbols.h"
#include "CutsceneCtrl.h"
#include "BlurFX.h"
#include "PauseAudio.h"
#include "UI.h"

MYMODCFG(com.example.cutscenectrl, CutsceneCtrl, 1.0.3, YourName)
NEEDGAME(com.rockstargames.gtasa)

ON_MOD_PRELOAD()
{
    logger->Info("[CutsceneCtrl] PreLoad: v2.00 target, %s build",
#if defined(AML64)
                  "64-bit"
#else
                  "32-bit"
#endif
    );
}

ON_MOD_LOAD()
{
    if (!Sym::ResolveAll())
    {
        logger->Info("[CutsceneCtrl] One or more REQUIRED symbols were not found in libGTASA.so.");
        logger->Info("[CutsceneCtrl] This usually means the game was updated past v2.00 -- see");
        logger->Info("[CutsceneCtrl] README.md 'Verifying game symbols' for how to re-check them.");
        return; // stay loaded but inert rather than risk hooking bad addresses
    }

    CutsceneCtrl::LoadConfig();
    CutsceneCtrl::InstallHooks();
}

ON_MOD_UNLOAD()
{
    UI::Shutdown();
    BlurFX::Shutdown();
    PauseAudio::Shutdown();
}

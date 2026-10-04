#include "PauseAudio.h"
#include <mod/amlmod.h>
#include <mod/iaml.h>
#include <mod/logger.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <cstdio>
#include <cstring>

namespace PauseAudio
{
    static SLObjectItf s_engineObj = nullptr;
    static SLEngineItf s_engine = nullptr;
    static SLObjectItf s_outputMixObj = nullptr;
    static bool s_ready = false;

    // One-shot players are short-lived: created on PlayFile(), destroyed by
    // their own completion callback. kMaxConcurrent caps how many can be
    // in flight at once (pause/resume cues are single events, so this is
    // generous headroom, not a hard functional limit).
    constexpr int kMaxConcurrent = 4;
    static SLObjectItf s_players[kMaxConcurrent] = {};

    static void ReleaseSlot(int i)
    {
        if (s_players[i])
        {
            (*s_players[i])->Destroy(s_players[i]);
            s_players[i] = nullptr;
        }
    }

    static void SLAPIENTRY PlayCallback(SLPlayItf caller, void* context, SLuint32 event)
    {
        if (event != SL_PLAYEVENT_HEADATEND) return;
        int idx = (int)(intptr_t)context;
        if (idx >= 0 && idx < kMaxConcurrent)
            ReleaseSlot(idx); // safe: OpenSL ES allows Destroy() from within a callback per the spec's "synchronous" notification model used here
    }

    void Init()
    {
        if (s_ready) return;
        if (slCreateEngine(&s_engineObj, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
        if ((*s_engineObj)->Realize(s_engineObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;
        if ((*s_engineObj)->GetInterface(s_engineObj, SL_IID_ENGINE, &s_engine) != SL_RESULT_SUCCESS) return;

        if ((*s_engine)->CreateOutputMix(s_engine, &s_outputMixObj, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
        if ((*s_outputMixObj)->Realize(s_outputMixObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;

        s_ready = true;
    }

    void Shutdown()
    {
        for (int i = 0; i < kMaxConcurrent; i++) ReleaseSlot(i);
        if (s_outputMixObj) { (*s_outputMixObj)->Destroy(s_outputMixObj); s_outputMixObj = nullptr; }
        if (s_engineObj) { (*s_engineObj)->Destroy(s_engineObj); s_engineObj = nullptr; }
        s_ready = false;
    }

    void PlayFile(const char* filename)
    {
        if (!filename || !filename[0]) return;
        Init();
        if (!s_ready) return;

        char path[512];
        snprintf(path, sizeof(path), "%s/CutsceneCtrl/%s", aml->GetInternalModsPath(), filename);
        if (!aml->FileExists(path))
        {
            logger->Info("[CutsceneCtrl] PauseAudio: file not found: %s", path);
            return;
        }

        int slot = -1;
        for (int i = 0; i < kMaxConcurrent; i++) if (!s_players[i]) { slot = i; break; }
        if (slot < 0) { logger->Info("[CutsceneCtrl] PauseAudio: too many sounds in flight, dropping request"); return; }

        SLDataLocator_URI locUri = { SL_DATALOCATOR_URI, (SLchar*)path };
        SLDataFormat_MIME formatMime = { SL_DATAFORMAT_MIME, nullptr, SL_CONTAINERTYPE_UNSPECIFIED };
        SLDataSource audioSrc = { &locUri, &formatMime };

        SLDataLocator_OutputMix locOutmix = { SL_DATALOCATOR_OUTPUTMIX, s_outputMixObj };
        SLDataSink audioSnk = { &locOutmix, nullptr };

        const SLInterfaceID ids[1] = { SL_IID_PLAY };
        const SLboolean req[1] = { SL_BOOLEAN_TRUE };

        SLObjectItf player = nullptr;
        if ((*s_engine)->CreateAudioPlayer(s_engine, &player, &audioSrc, &audioSnk, 1, ids, req) != SL_RESULT_SUCCESS) return;
        if ((*player)->Realize(player, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) { (*player)->Destroy(player); return; }

        SLPlayItf playItf = nullptr;
        (*player)->GetInterface(player, SL_IID_PLAY, &playItf);
        if (playItf)
        {
            (*playItf)->RegisterCallback(playItf, PlayCallback, (void*)(intptr_t)slot);
            (*playItf)->SetCallbackEventsMask(playItf, SL_PLAYEVENT_HEADATEND);
            (*playItf)->SetPlayState(playItf, SL_PLAYSTATE_PLAYING);
        }

        s_players[slot] = player;
    }
}

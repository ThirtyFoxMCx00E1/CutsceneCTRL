#pragma once
// PauseAudio -- plays a short custom pause/resume sound effect via OpenSL ES.
// Deliberately independent of the game's own audio engine (no CutsceneCtrl
// game-symbol dependency here at all): the game's audio pipeline is CODE
// PAUSED along with everything else during a cutscene pause, so mixing our
// cue through it would be unreliable. OpenSL ES is the NDK's own low-level
// audio API and keeps working regardless of the game's pause state.
namespace PauseAudio
{
    void Init();
    void Shutdown();

    // filename is relative to <AML mods data folder>/CutsceneCtrl/ -- see
    // README.md "Custom pause/resume sounds". No-ops (and logs once) if the
    // file doesn't exist.
    void PlayFile(const char* filename);
}

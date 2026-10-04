#include <cmath>
#include <mod/amlmod.h>
#include <mod/iaml.h>
#include <mod/logger.h>
#include <mod/config.h>
#include "GameSymbols.h"
#include "CutsceneCtrl.h"
#include "BlurFX.h"
#include "PauseAudio.h"
#include "UI.h"
#include <cstring>
#include <unistd.h>
namespace CutsceneCtrl { extern bool (*CCutsceneMgr_IsCutsceneSkipButtonBeingPressed)(); extern void (*CTimer_Update)(); extern void (*CCutsceneMgr_Update)(); void HookOf_CTimer_Update(); void HookOf_CCutsceneMgr_Update();
  extern void (*RenderEffects)(); void HookOf_RenderEffects(); extern void (*Render2dStuff)(); void HookOf_Render2dStuff(); }
static int reCalls=0, r2dCalls=0;
FakeAML fa; FakeAML* aml=&fa; FakeLogger fl; FakeLogger* logger=&fl; Config cf; Config* cfg=&cf;
// UI is stubbed here (it is covered by tests/imgui): the logic test only needs to see WHEN the UI is asked to draw and with what.
namespace UI { bool ffHeld=false; bool FastForwardHeld(){return ffHeld;}
  int active=-1, frames=0; bool lastPaused=false, lastInCutscene=true, settingsOpen=false, lastDrawUI=true, lastVignette=false, lastFFButton=false; const char* lastBanner=nullptr; const char* lastInfo=nullptr; int ended=0;
  void Init(){} void Shutdown(){} bool PushTouchEvent(int,int,int,int){return false;} void SetActive(bool a){active=a;}
  bool IsSettingsOpen(){return settingsOpen;}
  void Frame(const FrameInfo& fi){ ++frames; lastPaused=fi.paused; lastInCutscene=fi.inCutscene; lastDrawUI=fi.drawUI; lastVignette=fi.vignette; lastFFButton=fi.ffButton; lastBanner=fi.banner; lastInfo=fi.info; } void OnCutsceneEnded(){++ended;} }
static int blurDraws=0, frzCaptures=0, frzDraws=0, frzInvalidates=0, orderSeq=0, frzDrawSeq=0, blurDrawSeq=0; static bool frzValid=false, frzCaptureOk=true;
namespace BlurFX { void Init(){} void Shutdown(){} int captures=0; void CaptureNow(bool){++captures;} void DrawFullscreen(uint8_t,bool){ ++blurDraws; blurDrawSeq=++orderSeq; }
  bool CaptureFrozen(){ ++frzCaptures; frzValid=frzCaptureOk; return frzCaptureOk; } void InvalidateFrozen(){ ++frzInvalidates; frzValid=false; } bool HasFrozen(){ return frzValid; } void DrawFrozen(){ ++frzDraws; frzDrawSeq=++orderSeq; } }
namespace PauseAudio { void Init(){} void Shutdown(){} int played=0; void PlayFile(const char*){++played;} }

// ---- fake game memory ---------------------------------------------------
static float stepClipped=0.5f, stepNonClipped=0.5f; static int cutsceneUpdates=0, timerUpdates=0;
static bool userPause=false, codePause=false, csRunning=false; static float csTimer=0; static void* frontBuf=nullptr;
static uint8_t cam[3504]; static CPad pad; static int fade=0; static int scriptsRun=0, skips=0, restores=0, audioPause=0, audioResume=0; static bool skipBtn=false;
static CPad* getPad(int){ return &pad; }
static float timeScale=1.0f, scaleSeenByUpdate=-1; static bool gameWritesScale=false;
static void fakeTimerUpdate(){ ++timerUpdates; scaleSeenByUpdate=timeScale; stepNonClipped=0.5f*timeScale; stepClipped=stepNonClipped>3.0f?3.0f:stepNonClipped; }          // the game recomputes both every frame, even when paused
static void fakeCutsceneUpdate(){ ++cutsceneUpdates; csTimer += stepNonClipped * 0.02f; }        // Update_overlay: timer += NonClipped-step * k
static void camProc(void*){} static void setFixed(void*,const CVector&,const CVector&){} static void takeNo(void*,const CVector&,int16_t,int32_t){} static void takeSpl(void*,int16_t){}
static uint32_t cutFin(void*){return 0;} static int32_t fadeSt(void*){return fade;} static void restoreJC(void*){++restores;}
static void doSkip(){++skips;} static bool skipPressed(){return skipBtn;} static void scriptsProc(){++scriptsRun;}
static void aPause(void*){++audioPause;} static void aResume(void*){++audioResume;}
static bool onMission(){return true;}
static void setWide(bool on){ cam[CCameraOffsets::m_WideScreenOn]=on?1:0; }
static int frame(int n=1,bool wide=true){ for(int i=0;i<n;++i){ setWide(wide); CutsceneCtrl::OnLogicFrame(); CutsceneCtrl::OnRenderFrame(); } return n; }
static void slp(int ms){ usleep(ms*1000); }
namespace CutsceneCtrl { void HookOf_Render2dStuff(); }
static void ui(bool wide=false){ UI::lastFFButton=false; frame(1,wide); CutsceneCtrl::HookOf_Render2dStuff(); }   // a frame INCLUDING the UI draw (Render2dStuff), so UI::lastFFButton is current

#define CHECK(c) do{ if(!(c)){ printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails; } else ++passes; }while(0)
static int fails=0, passes=0;
static void fresh(){   // reset the pieces of game state a scenario touches
  frame(4,false); slp(400); userPause=codePause=false; fade=0; csRunning=false; memset(&pad,0,sizeof(pad));
}
using namespace CutsceneCtrl;

int main(){
  Sym::CTimer_m_UserPause=&userPause; Sym::CTimer_m_CodePause=&codePause; Sym::TheCamera=cam; Sym::CCutsceneMgr_ms_running=&csRunning;
  Sym::CCutsceneMgr_ms_cutsceneTimer=&csTimer; Sym::CPostEffects_pRasterFrontBuffer=&frontBuf; Sym::CPad_GetPad=getPad; Sym::CCamera_Process=camProc;
  Sym::CCamera_SetCamPositionForFixedMode=setFixed; Sym::CCamera_TakeControlNoEntity=takeNo; Sym::CCamera_TakeControlWithSpline=takeSpl;
  Sym::CCamera_GetCutSceneFinishTime=cutFin; Sym::CCamera_GetScreenFadeStatus=fadeSt; Sym::CCamera_RestoreWithJumpCut=restoreJC;
  Sym::CCutsceneMgr_SkipCutscene=doSkip; Sym::CCutsceneMgr_IsCutsceneSkipButtonBeingPressed=skipPressed;
  Sym::CTheScripts_Process=scriptsProc; Sym::CTheScripts_IsPlayerOnAMission=onMission;
  Sym::CAudioEngine_PauseAllSounds=aPause; Sym::CAudioEngine_ResumeAllSounds=aResume; Sym::AudioEngine=cam;
  CutsceneCtrl::CCutsceneMgr_IsCutsceneSkipButtonBeingPressed = skipPressed;   // what HOOK() does for real
  CutsceneCtrl::CTimer_Update = fakeTimerUpdate; CutsceneCtrl::CCutsceneMgr_Update = fakeCutsceneUpdate;
  CutsceneCtrl::RenderEffects = [](){ ++reCalls; }; CutsceneCtrl::Render2dStuff = [](){ ++r2dCalls; }; Sym::addr_Render2dStuff = 1;
  Sym::CTimer_ms_fTimeScale = &timeScale; Sym::CTimer_ms_fTimeStep = &stepClipped; Sym::CTimer_ms_fTimeStepNonClipped = &stepNonClipped;
  Sym::g_bCoreResolved=true;
  LoadConfig(); printf("config loaded, saves=%d\n",cf.saves);

  printf("\n[1] flicker: widescreen on for only 2 frames must NOT count as a cutscene\n");
  frame(2,true); frame(1,false); CHECK(!IsOnAnyCutscene()); CHECK(TogglePause()==false); CHECK(!g_bCutscenePaused);
  slp(400);
  printf("[2] sustained widescreen => cutscene; pause works; timers + audio + scripts\n");
  frame(5,true); CHECK(IsOnAnyCutscene());
  CHECK(TogglePause()==true); CHECK(g_bCutscenePaused && userPause && codePause); CHECK(audioPause==1);
  scriptsRun=0; frame(3,true); CHECK(scriptsRun==3);
  CHECK(BlurFX::captures==1);
  printf("[3] *** THE REPORTED BUG ***: detection drops while paused (walking into a house)\n");
  frame(1,false);
  CHECK(!g_bCutscenePaused); CHECK(!userPause && !codePause); CHECK(audioResume==1);
  frame(10,false); CHECK(!userPause && !codePause);
  slp(400);
  printf("[4] fade starts while paused => forced resume\n");
  frame(5,true); CHECK(TogglePause()); CHECK(userPause);
  fade=1; frame(1,true); CHECK(!g_bCutscenePaused && !userPause && !codePause); fade=0; frame(1,true);
  slp(400);
  printf("[5] cannot START a pause during a fade\n");
  fade=2; frame(2,true); CHECK(TogglePause()==false); CHECK(!userPause); fade=0; frame(1,true);
  slp(400);
  printf("[6] free camera: only works while paused, and always hands the camera back\n");
  pad.OldState.Select=0; pad.NewState.Select=1; frame(1,true); CHECK(!g_bFreeCamActive);   // not paused: toggle ignored
  pad.NewState.Select=0; frame(1,true);
  CHECK(TogglePause()); frame(1,true);
  pad.OldState.Select=0; pad.NewState.Select=1; frame(1,true); CHECK(g_bFreeCamActive);
  pad.NewState.Select=0; slp(600);
  restores=0; frame(1,false);                              // cutscene ends while free-cam active
  CHECK(!g_bFreeCamActive); CHECK(restores==1); CHECK(!userPause && !codePause);
  slp(400);
  printf("[7] resuming with free cam active deactivates it\n");
  frame(5,true); CHECK(TogglePause()); pad.OldState.Select=0; pad.NewState.Select=1; frame(1,true); CHECK(g_bFreeCamActive);
  pad.NewState.Select=0; slp(400); CHECK(TogglePause()); CHECK(!g_bFreeCamActive); CHECK(!userPause);
  slp(400);
  printf("[8] gamepad Start toggles on the press edge only\n");
  pad.OldState.Start=0; pad.NewState.Start=1; frame(1,true); CHECK(g_bCutscenePaused);
  pad.OldState.Start=1; pad.NewState.Start=1; frame(3,true); CHECK(g_bCutscenePaused);      // held: no re-toggle
  pad.OldState.Start=0; pad.NewState.Start=0; slp(400);
  printf("[9] skip while paused unpauses then skips; SkipInPause=false blocks it\n");
  skipBtn=true; skips=0; frame(1,true); CHECK(skips==1); CHECK(!g_bCutscenePaused && !userPause); skipBtn=false; slp(400);
  frame(3,true); CHECK(TogglePause()); g_Settings.skipInPause=false; skipBtn=true; frame(2,true); CHECK(skips==1 && g_bCutscenePaused);
  g_Settings.skipInPause=true; skipBtn=false; slp(400); CHECK(TogglePause()); slp(400);
  printf("[10] real cutscene (CCutsceneMgr running) is detected instantly, no debounce needed\n");
  frame(3,false); csRunning=true; frame(1,false); CHECK(IsOnAnyCutscene()); CHECK(!IsOnScriptedCutscene());
  csRunning=false; frame(1,false); CHECK(!IsOnAnyCutscene());
  slp(400);
  printf("[11] PauseOnlyDuringMissions is honoured\n");
  g_Settings.pauseOnlyDuringMissions=true; Sym::CTheScripts_IsPlayerOnAMission=[]{return false;}; frame(5,true); CHECK(TogglePause()==false);
  Sym::CTheScripts_IsPlayerOnAMission=onMission; CHECK(TogglePause()==true); CHECK(TogglePause()==false /*350ms debounce*/); g_Settings.pauseOnlyDuringMissions=false; slp(400); CHECK(TogglePause()); slp(400);
  printf("[12] settings round-trip: change -> reset restores defaults\n");
  g_Settings.blurAlpha=33; SettingChanged(&g_Settings.blurAlpha); ResetAllSettings(); CHECK(g_Settings.blurAlpha==170); CHECK(g_Settings.buttonCorner==0);
  printf("[13] auto-pause after returning from background (armed only after the scene has run a while)\n");
  frame(3,false); slp(400); frame(5,true); slp(2700); frame(1,true);    // cutscene has been running >2.5s
  slp(3200); frame(1,true); CHECK(g_bCutscenePaused);                     // then a 3.2s gap => paused on return
  fresh();
  frame(5,true); slp(3200); frame(1,true); CHECK(!g_bCutscenePaused);     // big stall right at the START must not pause
  printf("[14] *** PAUSE FREEZES TIME ***: the cutscene clock and both timesteps stand still while paused\n");
  frame(5,false); slp(400); frame(5,true); CHECK(IsOnAnyCutscene());
  csTimer = 10.0f;
  CutsceneCtrl::HookOf_CCutsceneMgr_Update(); float runningTimer = csTimer; CHECK(runningTimer > 10.0f);     // unpaused: the clock advances
  CHECK(TogglePause()); CHECK(g_bCutscenePaused);
  float frozen = csTimer; int ok1=1, ok2=1;
  for(int i=0;i<30;++i){
      CutsceneCtrl::HookOf_CTimer_Update();                                   // frame start: game recomputes timesteps
      ok1 &= (stepClipped==0.0f && stepNonClipped==0.0f);
      CutsceneCtrl::HookOf_CCutsceneMgr_Update();                             // game's Update_overlay tries to advance the clock
      ok2 &= (csTimer==frozen);
  }
  CHECK(ok1); CHECK(ok2); printf("    clock held at %.3f across 30 paused frames\n", csTimer);
  slp(400); CHECK(TogglePause()); CHECK(!g_bCutscenePaused);
  CutsceneCtrl::HookOf_CTimer_Update(); CHECK(stepNonClipped==0.5f && stepClipped==0.5f);                      // unpaused: untouched
  float t0=csTimer; CutsceneCtrl::HookOf_CCutsceneMgr_Update(); CHECK(csTimer > t0);                          // and time flows again
  printf("[15] the freeze never applies outside a pause (no stray writes to the clock)\n");
  frame(3,false); float t1=csTimer; CutsceneCtrl::HookOf_CCutsceneMgr_Update(); CHECK(csTimer > t1);
  slp(400);
  printf("[16] settings include the new menu options and round-trip\n");
  CHECK(g_Settings.buttonPlateAlpha == 0 && g_Settings.labelAlpha == 170);                                   // see-through buttons by default
  CHECK(g_Settings.showGameplayButton && g_Settings.gameplayButtonX == 50.0f && g_Settings.gameplayButtonY == 4.5f && !strcmp(g_Settings.settingsButtonLabel, "Cutscene Controller Settings"));
  g_Settings.buttonPlateAlpha = 120; LoadConfig(); CHECK(g_Settings.buttonPlateAlpha == 0);                   // an old untouched plate (120) becomes transparent once
  g_Settings.buttonPlateAlpha = 200; LoadConfig(); CHECK(g_Settings.buttonPlateAlpha == 200);                 // a value the user chose is kept
  g_Settings.buttonPlateAlpha = 0;
  CHECK(g_Settings.menuScale == 1.0f); g_Settings.menuScale = 2.0f; SettingChanged(&g_Settings.menuScale); ResetAllSettings(); CHECK(g_Settings.menuScale == 1.0f);
  ApplyPreset(1); CHECK(!g_Settings.showBlur && !g_Settings.showPauseText && g_Settings.buttonScale < 1.0f); ApplyPreset(0); CHECK(g_Settings.showBlur && g_Settings.showPauseText);
  printf("[17] *** THE UI IS DRAWN AFTER THE GAME'S 2D PASS (Render2dStuff), once per frame, with the right state ***\n");
  fresh(); frame(5,false); slp(400); frame(5,true); CHECK(IsOnAnyCutscene());
  CutsceneCtrl::HookOf_Render2dStuff();                     // drain whatever the helper left pending
  UI::frames=0; reCalls=r2dCalls=0;
  CutsceneCtrl::HookOf_RenderEffects(); CHECK(reCalls==1);                           // the game's own RenderEffects still runs
  CHECK(UI::frames==0);                                                              // ...but the UI is NOT drawn yet (it would sit under the HUD)
  CutsceneCtrl::HookOf_Render2dStuff(); CHECK(r2dCalls==1 && UI::frames==1);         // drawn right after the HUD
  CHECK(UI::lastPaused==false && UI::lastBanner==nullptr);
  CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::frames==1);                        // never twice for one RenderEffects
  CHECK(UI::active==1);
  CHECK(TogglePause()); CHECK(g_bCutscenePaused);
  BlurFX::captures=0; CutsceneCtrl::HookOf_Render2dStuff(); UI::frames=0;
  CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff();
  CHECK(UI::frames==1 && UI::lastPaused && UI::lastBanner && !strcmp(UI::lastBanner,"PAUSED"));     // paused: banner text handed to the UI
  g_Settings.showPauseText=false; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::lastBanner==nullptr); g_Settings.showPauseText=true;
  slp(400); pad.OldState.Select=0; pad.NewState.Select=1; frame(1,true); pad.NewState.Select=0; CHECK(g_bFreeCamActive);
  CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::lastInfo && strstr(UI::lastInfo,"Free camera"));       // free-cam speed line is passed on
  slp(400); CHECK(TogglePause()); CHECK(!g_bCutscenePaused);
  g_Settings.showInterface=false; UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::active==0 && !(UI::frames==1 && UI::lastDrawUI)); g_Settings.showInterface=true;   // master switch: no buttons, no touches (only the vignette may still be drawn, see [18])
  Sym::addr_Render2dStuff=0; UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CHECK(UI::frames==1);        // fallback when Render2dStuff can't be hooked: drawn from RenderEffects
  CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::frames==1); Sym::addr_Render2dStuff=1;
  fresh(); frame(5,false); CutsceneCtrl::HookOf_Render2dStuff(); UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff();
  CHECK(UI::frames==1 && UI::active==1 && UI::lastInCutscene==false && !UI::lastPaused && !UI::lastBanner);   // no cutscene: only the gameplay settings button is drawn (never a banner / pause state)
  g_Settings.showGameplayButton=false; UI::frames=0; UI::settingsOpen=false;
  CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::frames==0 && UI::active==0);   // button switched off: no UI work at all
  UI::settingsOpen=true; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::frames==1 && UI::active==1);   // ...unless the window is open (it must not vanish under the finger)
  UI::settingsOpen=false; g_Settings.showGameplayButton=true;
  g_Settings.showInterface=false; UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::frames==0 && UI::active==0); g_Settings.showInterface=true;   // master switch also hides the gameplay button
  frame(5,true); slp(400); fresh(); int ended0=UI::ended; frame(5,true); frame(2,false); CHECK(UI::ended>ended0);   // cutscene end tells the UI to close its window
  printf("[18] *** VIGNETTE: only in cutscenes, independent of the buttons, never while faded or in gameplay ***\n");
  fresh(); frame(5,false); slp(400); frame(5,true); CHECK(IsOnAnyCutscene());
  CutsceneCtrl::HookOf_Render2dStuff(); UI::frames=0;
  CHECK(g_Settings.showVignette && g_Settings.vignetteStrength==160 && g_Settings.vignetteSize==50);
  CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff();
  CHECK(UI::frames==1 && UI::lastVignette && UI::lastDrawUI && UI::lastInCutscene);                       // cutscene: vignette + buttons
  g_Settings.showInterface=false; UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff();
  CHECK(UI::frames==1 && UI::lastVignette && !UI::lastDrawUI && UI::active==0);                          // buttons off: still the vignette, and no touches are taken
  g_Settings.showInterface=true;
  g_Settings.vignetteStrength=0; UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::frames==1 && !UI::lastVignette && UI::lastDrawUI); g_Settings.vignetteStrength=160;   // strength 0 = off
  g_Settings.showVignette=false; UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::frames==1 && !UI::lastVignette && UI::lastDrawUI);
  g_Settings.showInterface=false; UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::frames==0);          // nothing wanted at all: no UI work
  g_Settings.showInterface=true; g_Settings.showVignette=true;
  CHECK(TogglePause()); UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::frames==1 && UI::lastVignette && UI::lastPaused);   // paused: still there
  slp(400); CHECK(TogglePause());
  fresh(); frame(5,false); CutsceneCtrl::HookOf_Render2dStuff(); UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff();
  CHECK(UI::frames==1 && !UI::lastVignette && !UI::lastInCutscene);                                      // gameplay: never a vignette
  g_Settings.showGameplayButton=false; UI::frames=0; CutsceneCtrl::HookOf_RenderEffects(); CutsceneCtrl::HookOf_Render2dStuff(); CHECK(UI::frames==0); g_Settings.showGameplayButton=true;
  CHECK(g_Settings.fontStyle==-1 && g_Settings.outlineAlpha==170 && g_Settings.fontYOffset==0);
  CHECK(g_Settings.gameplayButtonX==50.0f && g_Settings.gameplayButtonY==4.5f);
  g_Settings.vignetteStrength=33; g_Settings.fontStyle=3; g_Settings.gameplayButtonX=12.5f; ResetAllSettings();
  CHECK(g_Settings.vignetteStrength==160 && g_Settings.fontStyle==-1 && g_Settings.gameplayButtonX==50.0f);   // "Reset ALL settings" covers the new keys
  printf("[19] *** HOLD-FOR-FAST-FORWARD (experimental): scales ms_fTimeScale around CTimer::Update only, strictly gated ***\n");
  fresh(); g_Settings.ffEnabled=false; timeScale=1.0f; csRunning=true; frame(3,false); CHECK(IsOnAnyCutscene() && !IsOnScriptedCutscene() && !g_bCutscenePaused);
  pad.NewState.RightShoulder2=1; pad.OldState.RightShoulder2=1; frame(2,false); CHECK(!g_bFastForwarding);                 // setting off: held button does nothing
  HookOf_CTimer_Update(); CHECK(scaleSeenByUpdate==1.0f && stepNonClipped==0.5f);
  ui(false); CHECK(!UI::lastFFButton);                                                                                                // ...and no on-screen button
  g_Settings.ffEnabled=true; audioPause=audioResume=0; ui(false); CHECK(g_bFastForwarding && UI::lastFFButton);
  HookOf_CTimer_Update();
  CHECK(scaleSeenByUpdate==2.0f && stepNonClipped==1.0f && stepClipped==1.0f);                                             // the game computed both timesteps with scale x2
  CHECK(timeScale==1.0f);                                                                                                  // ...and the value is put straight back
  timeScale=0.5f; HookOf_CTimer_Update(); CHECK(scaleSeenByUpdate==1.0f && timeScale==0.5f); timeScale=1.0f;                // the game's own slow-motion is multiplied, never replaced
  g_Settings.ffSpeed=4.0f; HookOf_CTimer_Update(); CHECK(scaleSeenByUpdate==4.0f && stepNonClipped==2.0f);
  timeScale=2.0f; HookOf_CTimer_Update(); CHECK(stepNonClipped==4.0f && stepClipped==3.0f); timeScale=1.0f;                // the game's own clip at 3.0 still applies
  g_Settings.ffSpeed=10.0f; HookOf_CTimer_Update(); CHECK(scaleSeenByUpdate==4.0f);                                         // out-of-range values are clamped
  g_Settings.ffSpeed=0.2f;  HookOf_CTimer_Update(); CHECK(scaleSeenByUpdate==1.0f);
  g_Settings.ffSpeed=2.0f;
  CHECK(audioPause==1 && audioResume==0);                                                                                  // audio silenced for the duration
  pad.NewState.RightShoulder2=0; frame(1,false); CHECK(!g_bFastForwarding && audioResume==1);                              // released: back to normal, audio given back
  HookOf_CTimer_Update(); CHECK(scaleSeenByUpdate==1.0f && stepNonClipped==0.5f);
  // the cutscene clock really runs 2x: that is what the animation / subtitle timing follows
  { float c0=csTimer; pad.NewState.RightShoulder2=1; frame(1,false); HookOf_CTimer_Update(); HookOf_CCutsceneMgr_Update(); float d2=csTimer-c0;
    pad.NewState.RightShoulder2=0; frame(1,false); c0=csTimer; HookOf_CTimer_Update(); HookOf_CCutsceneMgr_Update(); float d1=csTimer-c0; CHECK(d1>0 && std::fabs(d2/d1-2.0f)<0.01f); }
  // mute off: no audio calls at all
  g_Settings.ffMuteAudio=false; audioPause=audioResume=0; pad.NewState.RightShoulder2=1; frame(1,false); CHECK(g_bFastForwarding); pad.NewState.RightShoulder2=0; frame(1,false); CHECK(audioPause==0 && audioResume==0); g_Settings.ffMuteAudio=true;
  // the on-screen hold button
  UI::ffHeld=true; frame(1,false); CHECK(g_bFastForwarding); UI::ffHeld=false; frame(1,false); CHECK(!g_bFastForwarding);
  g_Settings.ffScreenButton=false; UI::ffHeld=true; ui(false); CHECK(!g_bFastForwarding && !UI::lastFFButton); UI::ffHeld=false; g_Settings.ffScreenButton=true;   // switched off: neither shown nor honoured
  g_Settings.ffButton=GamepadButton::None; pad.NewState.RightShoulder2=1; frame(1,false); CHECK(!g_bFastForwarding); g_Settings.ffButton=GamepadButton::R2;                 // "None" = no gamepad button
  // pausing ends it; holding while paused does nothing; releasing the pause with the button still down picks it up again
  pad.NewState.RightShoulder2=1; frame(1,false); CHECK(g_bFastForwarding); audioPause=audioResume=0;
  slp(400); CHECK(TogglePause()); CHECK(g_bCutscenePaused && !g_bFastForwarding && userPause);
  timeScale=1.0f; HookOf_CTimer_Update(); CHECK(scaleSeenByUpdate==1.0f && stepNonClipped==0.0f);                           // paused: time stands still, no scaling
  frame(1,true); ui(true); CHECK(!g_bFastForwarding && !UI::lastFFButton);
  slp(400); CHECK(TogglePause()); frame(1,false); CHECK(!g_bCutscenePaused && g_bFastForwarding); pad.NewState.RightShoulder2=0; frame(1,false); CHECK(!g_bFastForwarding);
  // gating
  pad.NewState.RightShoulder2=1;
  g_bFreeCamActive=true; ui(false); CHECK(!g_bFastForwarding && !UI::lastFFButton); g_bFreeCamActive=false;               // never in the free camera
  fade=1; frame(1,false); CHECK(!g_bFastForwarding); fade=0;                                                                  // never while the screen is faded
  frame(1,false); CHECK(g_bFastForwarding);
  { auto sv=Sym::CTimer_ms_fTimeScale; Sym::CTimer_ms_fTimeScale=nullptr; ui(false); CHECK(!g_bFastForwarding && !UI::lastFFButton); HookOf_CTimer_Update(); CHECK(scaleSeenByUpdate==1.0f); Sym::CTimer_ms_fTimeScale=sv; }   // symbol missing: feature off, no crash
  frame(1,false); CHECK(g_bFastForwarding);
  // cutscene ends while held: everything is put back
  audioPause=audioResume=0; csRunning=false; frame(2,false); CHECK(!IsOnAnyCutscene() && !g_bFastForwarding && audioResume>=1); timeScale=1.0f; HookOf_CTimer_Update(); CHECK(scaleSeenByUpdate==1.0f);
  // scripted ("semi") scenes are excluded unless the user opts in
  fresh(); pad.NewState.RightShoulder2=1; frame(5,false); slp(400); frame(5,true); CHECK(IsOnAnyCutscene() && IsOnScriptedCutscene());
  ui(true); CHECK(!g_bFastForwarding && !UI::lastFFButton);
  g_Settings.ffScripted=true; ui(true); CHECK(g_bFastForwarding && UI::lastFFButton); g_Settings.ffScripted=false; frame(1,true); CHECK(!g_bFastForwarding);
  pad.NewState.RightShoulder2=0; frame(2,false);
  // gameplay: never
  fresh(); pad.NewState.RightShoulder2=1; frame(2,false); ui(false); CHECK(!IsOnAnyCutscene() && !g_bFastForwarding && !UI::lastFFButton); pad.NewState.RightShoulder2=0;
  g_Settings = Settings(); CHECK(!g_Settings.ffEnabled && g_Settings.ffSpeed==2.0f && g_Settings.ffButton==GamepadButton::R2 && g_Settings.ffScreenButton && g_Settings.ffMuteAudio && !g_Settings.ffScripted);   // defaults

  printf("[20] *** FREEZE FRAME: captured once per pause, drawn opaque BEFORE the blur, never over the free camera ***\n");
  g_Settings = Settings(); fresh(); csRunning=true; frame(3,false); CHECK(IsOnAnyCutscene());
  CHECK(!g_Settings.freezeFrame);                                                                                          // off by default
  frzCaptures=frzDraws=frzInvalidates=blurDraws=0; frzCaptureOk=true;
  slp(400); CHECK(TogglePause()); frame(3,true); CHECK(frzCaptures==0 && frzDraws==0 && blurDraws>=3);                     // off: the normal pause screen, untouched
  slp(400); CHECK(TogglePause()); frame(2,false);
  g_Settings.freezeFrame=true; frzCaptures=frzDraws=frzInvalidates=blurDraws=0;
  frame(3,false); CHECK(frzCaptures==0 && frzDraws==0);                                                                    // not paused: nothing
  slp(400); CHECK(TogglePause()); CHECK(frzInvalidates==1);                                                                // a new pause forgets the old picture
  frame(1,true); CHECK(frzCaptures==1 && frzDraws==1 && blurDraws==1); CHECK(frzDrawSeq<blurDrawSeq);                      // captured at the moment of the pause, drawn first (the blur goes over it)
  frame(4,true); CHECK(frzCaptures==1 && frzDraws==5 && blurDraws==5);                                                     // then only redrawn, never re-captured
  g_Settings.showBlur=false; frame(2,true); CHECK(frzDraws==7);                                                            // works without the blur too
  g_Settings.showBlur=true;
  g_bFreeCamActive=true; frame(2,true); CHECK(frzDraws==7 && frzCaptures==1);                                              // the free camera sees the LIVE scene
  g_bFreeCamActive=false; frame(1,true); CHECK(frzDraws==8 && frzCaptures==1);                                             // back to the same frozen picture
  g_Settings.freezeFrame=false; frame(2,true); CHECK(frzDraws==8);                                                         // unticked while paused: live again at once
  g_Settings.freezeFrame=true; frame(1,true); CHECK(frzDraws==9 && frzCaptures==1);                                        // ticked again: the picture of the pause is still there
  slp(400); CHECK(TogglePause()); frzDraws=0; frame(3,false); CHECK(frzDraws==0);                                          // resumed: gone
  // ticked only while already paused: captured on the next frame
  g_Settings.freezeFrame=false; frzCaptures=frzDraws=0; slp(400); CHECK(TogglePause()); frame(2,true); CHECK(frzCaptures==0 && frzDraws==0);
  g_Settings.freezeFrame=true; frame(1,true); CHECK(frzCaptures==1 && frzDraws==1); frame(2,true); CHECK(frzCaptures==1 && frzDraws==3);
  slp(400); CHECK(TogglePause()); frame(1,false);
  // a failed capture: the live scene stays visible, nothing is drawn
  frzCaptureOk=false; frzCaptures=frzDraws=0; slp(400); CHECK(TogglePause()); frame(3,true); CHECK(frzDraws==0 && frzCaptures>=1 && g_bCutscenePaused); frzCaptureOk=true;
  slp(400); CHECK(TogglePause()); frame(1,false);
  // no UI wanted -> no pause-screen visuals at all (same rule as the blur)
  g_Settings.showInterface=false; frzCaptures=frzDraws=0; slp(400); CHECK(TogglePause()); frame(2,true); CHECK(frzCaptures==0 && frzDraws==0); g_Settings.showInterface=true;
  slp(400); CHECK(TogglePause()); frame(1,false);
  CHECK(!g_bFastForwarding);
  printf("\n%d passed, %d failed\n",passes,fails); return fails?1:0;
}

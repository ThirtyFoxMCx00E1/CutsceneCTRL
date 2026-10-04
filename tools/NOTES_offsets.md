# Disassembly traces behind the hard-coded numbers

All from the shipped v2.00 `libGTASA.so`, read with the NDK's `llvm-objdump`
(`--triple=thumbv7-none-linux-android` for armeabi-v7a). These are the
instructions that justify each constant in `src/GameTypes.h` / `src/UI.cpp`.

## CCamera::SetWideScreenOn  ->  m_WideScreenOn is byte 59 on 32-bit (67 on 64-bit)
    mov.w r1, #0x100
    strh  r1, [r0, #0x3a]     ; one 16-bit store: byte 58 = 0 (m_bWantsToSwitchWidescreenOff),
    bx    lr                  ;                   byte 59 = 1 (m_WideScreenOn)

## CCamera::SetCamPositionForFixedMode(const CVector& vec, const CVector& source)
    vldr d16,[r1] ; ldr r1,[r1,#8] ; str.w r1,[r0,#0x7f8] ; add.w r1,r0,#0x7f0 ; vstr d16,[r1]
        ; 1st arg -> this+0x7f0 (2032)  = m_vecFixedModeVector
    ldr r1,[r2,#8] ; vldr d16,[r2] ; str.w r1,[r0,#0x804] ; ... addw r0,r0,#0x7fc ; vstr d16,[r0]
        ; 2nd arg -> this+0x7fc (2044)  = m_vecFixedModeSource
    (m_vecFixedModeUpOffSet is never written here; it follows at 2056)

## CCamera::GetScreenFadeStatus  ->  0 = no fade, 1 = mid-fade, 2 = fully black
    addw r0,r0,#0xb84 ; vldr s2,[r0]            ; s2 = fade alpha
    literal pool word 0x437f0000 = 255.0f       ; s0
    movs r0,#1 ; vcmp s2,s0 -> it eq: r0=2 ; vcmp s2,#0 -> it eq: r0=0

## CFont::SetColor(CRGBA)  ->  the CRGBA is passed by hidden pointer, not in a register
    ldrb r2,[r0]        ; reads byte 0 THROUGH r0 (a pointer) ... x4, into a static
    (mangled as by-value; physically by-reference. Hence const CRGBA& in our typedefs.)

## CAudioEngine::PauseAllSounds / ResumeAllSounds  ->  ignore `this`, no args
    ldr r0,[pc,#8] ; add r0,pc ; ldr r0,[r0] ; b.w <plt>   ; loads a global and tail-calls

## CFont::SetOrientation(uchar)  ->  0 = centre, 1 = left, 2 = right
## CFont::SetJustify(uchar)      ->  text *justification* on/off (NOT alignment)
## Font styles the game itself uses: 1 = subtitles, 3 = Pricedown (mission title)
## AND_TouchEvent(type, id, x, y): 2 = press, 1 and 4 = release, everything else = move

## CGame::Process  ->  what keeps running while paused (read from the disassembly, armeabi-v7a)
    CPad::UpdatePads, CTouchInterface::Clear, CHID::Update, CStreaming::Update,
    CCutsceneMgr::Update                      <-- BEFORE the pause gate, so our hook still runs while paused
    (m_CodePause) -> MobileMenu::Update ; CTheZones/CCover/CAudioZones::Update
    --- pause gate: m_UserPause / m_CodePause set => branch over everything below ---
    CSprite2d::SetRecipNearClip, CSprite2d::InitPerFrame, CFont::InitPerFrame, CTheScripts::Process,
    CWorld::Process, ... CCamera::Process
(That is why the mod calls CTheScripts::Process / CCamera::Process itself while paused.)

## CTimer::Update  ->  ms_fTimeStepNonClipped is NOT zeroed by the pause flags
    vstr s0, [ms_fTimeStepNonClipped]         ; stored unconditionally, BEFORE the m_UserPause/m_CodePause branch
    ... pause branch only forces ms_fTimeStep (the clipped one) to 0.

## CCutsceneMgr::Update  is a 24-byte stub
    if (ms_cutsceneLoadStatus != 0) tail-call CCutsceneMgr::Update_overlay()

## CCutsceneMgr::Update_overlay  ->  advances the cutscene clock with the NON-clipped step
    ms_cutsceneTimer += ms_fTimeStepNonClipped * k      ; (the PC mod NOPs this increment for its pause)
    => with only m_UserPause/m_CodePause set, the cutscene clock (and anything animated from it) keeps running.
    Fix: hook CTimer::Update to zero both steps while paused, and restore ms_cutsceneTimer after Update.

## CAudioEngine::PauseAllSounds / ResumeAllSounds  (see above): ignore `this`.

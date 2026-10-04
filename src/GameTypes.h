// GameTypes.h
//
// Minimal, ABI-compatible mirrors of GTA San Andreas (mobile, v2.00) engine
// types, plus verified byte offsets of the class fields CutsceneCtrl reads
// or writes directly.
//
// PROVENANCE: every offset/field name in this file was extracted from the
// DWARF debug info embedded in the arm64-v8a build of libGTASA.so (v2.00)
// -- it is NOT guessed and NOT copied from the PC version's plugin-sdk.
// See /README.md "How the offsets were obtained" for the exact method
// (readelf --debug-dump=pubtypes / --debug-dump=info --dwarf-start=...).
//
// The 32-bit (armeabi-v7a) build strips .debug_info, so it can't be
// checked directly against DWARF. CPad/CControllerState below are pure
// POD int16 fields with no pointer members anywhere in their layout, so
// their offsets ARE identical between the two ABIs (verified: nothing
// before CPad::NewState/OldState can differ in size). CCamera is a
// different story -- it has vtable pointers and pointer members mixed
// in, so its offsets do NOT carry over between ABIs; see the comment
// on CCameraOffsets below for how those were actually obtained.
#pragma once
#include <cstdint>
#include <mod/amlmod.h> // for the BYBIT(val32, val64) macro used by CCameraOffsets below

struct CVector
{
    float x, y, z;
    CVector() : x(0), y(0), z(0) {}
    CVector(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

struct CVector2D
{
    float x, y;
    CVector2D() : x(0), y(0) {}
    CVector2D(float _x, float _y) : x(_x), y(_y) {}
};

struct CRect
{
    float left, bottom, right, top; // matches CSprite2d::Draw(const CRect&, ...) call order used by the game
    CRect() : left(0), bottom(0), right(0), top(0) {}
    CRect(float l, float b, float r, float t) : left(l), bottom(b), right(r), top(t) {}
};

struct CRGBA
{
    uint8_t r, g, b, a;
    CRGBA() : r(0), g(0), b(0), a(0) {}
    CRGBA(uint8_t _r, uint8_t _g, uint8_t _b, uint8_t _a) : r(_r), g(_g), b(_b), a(_a) {}
};

// ---------------------------------------------------------------------
// CControllerState  (verified: DWARF DIE at .debug_info+0x38054e,
// byte_size = 48, arm64-v8a libGTASA.so v2.00). All fields are int16.
// This is CPad::NewState / CPad::OldState's element type.
// ---------------------------------------------------------------------
struct CControllerState
{
    int16_t LeftStickX;              // +0
    int16_t LeftStickY;              // +2
    int16_t RightStickX;             // +4
    int16_t RightStickY;             // +6
    int16_t LeftShoulder1;           // +8
    int16_t LeftShoulder2;           // +10
    int16_t RightShoulder1;          // +12
    int16_t RightShoulder2;          // +14
    int16_t DPadUp;                  // +16
    int16_t DPadDown;                // +18
    int16_t DPadLeft;                // +20
    int16_t DPadRight;                // +22
    int16_t Start;                   // +24
    int16_t Select;                  // +26
    int16_t ButtonSquare;            // +28
    int16_t ButtonTriangle;          // +30
    int16_t ButtonCross;             // +32
    int16_t ButtonCircle;            // +34
    int16_t ShockButtonL;            // +36
    int16_t ShockButtonR;            // +38
    int16_t m_bChatIndicated;        // +40
    int16_t m_bPedWalk;              // +42
    int16_t m_bVehicleMouseLook;     // +44
    int16_t m_bRadioTrackSkip;       // +46
};
static_assert(sizeof(CControllerState) == 48, "CControllerState layout drifted from verified DWARF size");

// ---------------------------------------------------------------------
// CPad (verified: DIE at 0x37eb69, byte_size = 344).
// We only care about NewState/OldState -- everything else is left as
// padding so the struct is safe to reinterpret_cast a CPad* onto.
// ---------------------------------------------------------------------
struct CPad
{
    CControllerState NewState;   // +0
    CControllerState OldState;   // +48
    uint8_t _pad[344 - 96];
};
static_assert(sizeof(CPad) == 344, "CPad size drifted from verified DWARF size");

// Byte offsets into CCamera (TheCamera).
//
// IMPORTANT: these differ between the 32-bit and 64-bit builds, because
// CCamera's base class (CPlaceable) contains a vtable pointer and a
// CMatrix* member -- both 8 bytes on arm64, 4 bytes on armeabi-v7a -- and
// CCamera's own Cams[3] array (of CCam, which itself holds 3 pointer
// members) sits between the early bool flags and the later CVector
// fields. That makes the size difference non-uniform across the class,
// so a single flat "shrink by N bytes" correction is NOT safe.
//
// The values below were NOT hand-computed from the arm64 DWARF alone
// (an earlier version of this file did that, and it was wrong -- it's
// why pause detection silently failed on the 32-bit build). Instead,
// each 32-bit value was read directly out of the compiled armeabi-v7a
// machine code with llvm-objdump, from the actual instructions that
// touch that field:
//   - m_WideScreenOn: disassembly of CCamera::SetWideScreenOn (a
//     2-instruction function -- trivial to read the immediate offset
//     directly off the `strh` instruction).
//   - m_vecFixedModeVector / m_vecFixedModeSource: disassembly of
//     CCamera::SetCamPositionForFixedMode, which writes both in one
//     function body (see tools/NOTES_offsets.md for the full trace).
//   - m_vecFixedModeUpOffSet: not written by any function we hooked
//     directly, so its 32-bit offset is inferred from the verified
//     -76-byte delta of its two immediate, contiguously-declared
//     neighbors (C++ preserves declaration order regardless of ABI, and
//     there is no possible inserted padding between adjacent 12-byte,
//     4-byte-aligned CVector members) -- not independently disassembled.
// ActiveCam's 32-bit value is likewise inferred (same -8 delta as
// m_WideScreenOn, both being plain bytes in the same region before the
// Cams array) rather than independently disassembled, since it is not
// currently read by this mod.
namespace CCameraOffsets
{
    constexpr uintptr_t m_WideScreenOn         = BYBIT(59, 67);    // bool -- VERIFIED (disassembly)
    constexpr uintptr_t ActiveCam              = BYBIT(87, 95);    // uint8_t index into Cams[] -- inferred
    constexpr uintptr_t Cams                   = BYBIT(0, 376);    // CCam Cams[3] -- NOT verified for 32-bit, unused
    constexpr uintptr_t m_vecFixedModeVector   = BYBIT(2032, 2108);// CVector -- VERIFIED (disassembly)
    constexpr uintptr_t m_vecFixedModeSource   = BYBIT(2044, 2120);// CVector -- VERIFIED (disassembly)
    constexpr uintptr_t m_vecFixedModeUpOffSet = BYBIT(2056, 2132);// CVector -- inferred (see above)
}

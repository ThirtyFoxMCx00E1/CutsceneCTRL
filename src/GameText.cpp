#include "GameText.h"
#include "GameSymbols.h"
#include "GameTypes.h"
#include <cstring>
#include <algorithm>

namespace
{
    bool s_haveSyms = false;

    // ASCII -> the game's 16-bit text. '~' starts a control token ("~r~" ...) in GXT text, so it is dropped;
    // anything outside printable ASCII becomes '?'.
    int ToGxt(const char* s, unsigned short* out, int cap)
    {
        int n = 0;
        for (; s && *s && n < cap - 1; ++s)
        {
            unsigned char c = (unsigned char)*s;
            if (c == '~') continue;
            out[n++] = (c >= 32 && c < 127) ? (unsigned short)c : (unsigned short)'?';
        }
        out[n] = 0;
        return n;
    }

    // Puts CFont into a known state for one string. The state blocks are restored by the caller (see Scope).
    void Prepare(int style, float scale)
    {
        Sym::CFont_SetFontStyle((uint8_t)style);
        Sym::CFont_SetProportional(1);
        Sym::CFont_SetBackground(0, 0);
        Sym::CFont_SetOrientation(1);                       // left: x is the left edge of the text
        Sym::CFont_SetJustify(0);
        Sym::CFont_SetWrapx(100000.0f);                     // never wrap a short label
        Sym::CFont_SetDropShadowPosition(0);
        Sym::CFont_SetScale(scale);
    }

    // Snapshot / restore of CFont's two global state blocks.
    struct Scope
    {
        uint8_t det[64], rs[48];
        Scope()  { memcpy(det, Sym::CFont_Details, sizeof(det)); memcpy(rs, Sym::CFont_RenderState, sizeof(rs)); }
        ~Scope() { memcpy(Sym::CFont_Details, det, sizeof(det)); memcpy(Sym::CFont_RenderState, rs, sizeof(rs)); }
    };

    float ScaleFor(float lineH)
    {
        Sym::CFont_SetScale(1.0f);
        const float h1 = Sym::CFont_GetHeight(0);            // line height at scale 1; linear in the scale (SetScale / GetHeight disassembly)
        return h1 > 0.01f ? lineH / h1 : 1.0f;
    }
}

namespace GameText
{
    bool Available()
    {
        s_haveSyms = Sym::CFont_SetFontStyle && Sym::CFont_SetScale && Sym::CFont_SetColor && Sym::CFont_SetOrientation &&
                     Sym::CFont_SetProportional && Sym::CFont_SetBackground && Sym::CFont_SetWrapx && Sym::CFont_SetJustify &&
                     Sym::CFont_SetDropShadowPosition && Sym::CFont_PrintString && Sym::CFont_GetStringWidth &&
                     Sym::CFont_GetHeight && Sym::CFont_RenderFontBuffer && Sym::CFont_Details && Sym::CFont_RenderState;
        return s_haveSyms;
    }

    float Measure(const char* text, int style, float lineH)
    {
        if (!Available() || !text || lineH < 1.0f) return 0.0f;
        unsigned short buf[96]; ToGxt(text, buf, 96);
        Scope keep;
        Prepare(style, 1.0f);
        const float scale = ScaleFor(lineH);
        Sym::CFont_SetScale(scale);
        return Sym::CFont_GetStringWidth(buf, 1, 0);
    }

    void Draw(const char* text, int style, float x, float y, float lineH, uint32_t rgba, int shadowAlpha)
    {
        if (!Available() || !text || !*text || lineH < 1.0f) return;
        const int a = (int)(rgba >> 24);
        if (a <= 0 && shadowAlpha <= 0) return;
        unsigned short buf[96]; if (ToGxt(text, buf, 96) == 0) return;
        Scope keep;
        Prepare(style, 1.0f);
        const float scale = ScaleFor(lineH);
        Prepare(style, scale);
        if (shadowAlpha > 0)
        {
            const float d = std::max(1.0f, lineH * 0.05f);
            Sym::CFont_SetColor(CRGBA(0, 0, 0, (uint8_t)std::min(shadowAlpha, 255)));
            Sym::CFont_PrintString(x + d, y + d, buf);
        }
        if (a > 0)
        {
            Sym::CFont_SetColor(CRGBA((uint8_t)(rgba & 255), (uint8_t)((rgba >> 8) & 255), (uint8_t)((rgba >> 16) & 255), (uint8_t)a));
            Sym::CFont_PrintString(x, y, buf);
        }
        Sym::CFont_RenderFontBuffer();                       // flush now: the game's own flush for this frame has already happened
    }
}

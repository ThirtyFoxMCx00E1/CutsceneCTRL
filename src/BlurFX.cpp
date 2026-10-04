#include "BlurFX.h"
#include "GameSymbols.h"
#include "GameTypes.h"
#include <mod/amlmod.h>
#include <mod/logger.h>
#include "GLState.h"
#include <cstring>

// RwRaster type/format flags. These are the classic RenderWare constants
// (stable across every RW-derived engine build, including RW-lineage
// mobile ports like this one) -- rwRASTERTYPECAMERATEXTURE is "a texture
// that can also be rendered into", exactly what we need to blit the front
// buffer into and later draw as a normal textured sprite.
namespace RwConst
{
    constexpr int32_t rwRASTERTYPECAMERATEXTURE = 0x04;
}

namespace BlurFX
{
    // ---- Simulated (RW raster) blur ---------------------------------------
    static void*   s_blurRaster = nullptr;
    static void*   s_blurTexture = nullptr;
    static uint8_t s_spriteBuf[32]; // CSprite2d is verified to be a single
                                     // pointer-sized member (see GameTypes.h);
                                     // 32 bytes leaves comfortable headroom.
    static bool    s_spriteConstructed = false;
    static int32_t s_captureW = 0, s_captureH = 0;

    static void EnsureSimulatedResources()
    {
        if (s_blurRaster) return;
        if (!Sym::RwRasterCreate || !Sym::RwTextureCreate || !Sym::CSprite2d_Ctor) return;

        int w = 0, h = 0;
        aml->GetDisplaySize(&w, &h);
        if (w <= 0 || h <= 0) { w = 1280; h = 720; } // sane fallback

        s_captureW = (int32_t)(w / 4.0f);
        s_captureH = (int32_t)(h / 4.0f);
        if (s_captureW < 8) s_captureW = 8;
        if (s_captureH < 8) s_captureH = 8;

        s_blurRaster = Sym::RwRasterCreate(s_captureW, s_captureH, 0, RwConst::rwRASTERTYPECAMERATEXTURE);
        if (!s_blurRaster)
        {
            logger->Info("[CutsceneCtrl] BlurFX: RwRasterCreate failed, simulated blur disabled");
            return;
        }
        s_blurTexture = Sym::RwTextureCreate(s_blurRaster);

        memset(s_spriteBuf, 0, sizeof(s_spriteBuf));
        Sym::CSprite2d_Ctor(s_spriteBuf);
        s_spriteConstructed = true;
    }

    static void CaptureSimulated()
    {
        if (!Sym::CPostEffects_pRasterFrontBuffer || !*Sym::CPostEffects_pRasterFrontBuffer) return;
        EnsureSimulatedResources();
        if (!s_blurRaster) return;

        Sym::RwRasterPushContext(s_blurRaster);
        Sym::RwRasterRenderFast(*Sym::CPostEffects_pRasterFrontBuffer, 0, 0);
        Sym::RwRasterPopContext();
    }

    static void DrawSimulated(uint8_t alpha)
    {
        if (!s_blurRaster || !s_spriteConstructed || !Sym::CSprite2d_Draw) return;

        // CSprite2d's only real member is the texture pointer, at offset 0
        // (verified via DWARF -- see GameTypes.h). We write it directly
        // rather than calling SetTexture(char*), which looks textures up
        // by name in the currently loaded TXD and can't reference a
        // raster we created ourselves.
        *(void**)s_spriteBuf = s_blurTexture;

        int w = 0, h = 0;
        aml->GetDisplaySize(&w, &h);
        CRect full(0.0f, (float)h, (float)w, 0.0f);
        CRGBA color(255, 255, 255, alpha);
        Sym::CSprite2d_Draw(s_spriteBuf, full, color);
    }

    // ---- Freeze frame: a sharp, full-resolution copy ---------------------------------------------------
    // The simulated blur above works because it copies the front buffer into a QUARTER-size raster, which blurs it when it is
    // drawn back up. The freeze frame is the same two RenderWare calls into a raster of the full size, so the copy is 1:1.
    static void*   s_frzRaster = nullptr;
    static void*   s_frzTexture = nullptr;
    static uint8_t s_frzSprite[32];
    static bool    s_frzSpriteBuilt = false;
    static int32_t s_frzW = 0, s_frzH = 0;
    static bool    s_frzValid = false;       // a good capture exists for the current pause
    static bool    s_frzFailed = false;      // a capture failed for the current pause: don't retry every frame

    static void FreeFrozenResources()
    {
        if (s_frzTexture && Sym::RwTextureDestroy) Sym::RwTextureDestroy(s_frzTexture);
        if (s_frzRaster && Sym::RwRasterDestroy) Sym::RwRasterDestroy(s_frzRaster);
        s_frzTexture = nullptr; s_frzRaster = nullptr; s_frzValid = false;
    }

    static bool EnsureFrozenResources(int w, int h)
    {
        if (s_frzRaster && (s_frzW != w || s_frzH != h)) FreeFrozenResources();      // screen size changed: start again
        if (s_frzRaster) return true;
        if (!Sym::RwRasterCreate || !Sym::RwTextureCreate || !Sym::CSprite2d_Ctor) return false;

        s_frzRaster = Sym::RwRasterCreate(w, h, 0, RwConst::rwRASTERTYPECAMERATEXTURE);
        if (!s_frzRaster) { logger->Info("[CutsceneCtrl] BlurFX: freeze frame raster %dx%d could not be created", w, h); return false; }
        s_frzTexture = Sym::RwTextureCreate(s_frzRaster);
        if (!s_frzTexture) { Sym::RwRasterDestroy(s_frzRaster); s_frzRaster = nullptr; return false; }
        s_frzW = w; s_frzH = h;
        if (!s_frzSpriteBuilt) { memset(s_frzSprite, 0, sizeof(s_frzSprite)); Sym::CSprite2d_Ctor(s_frzSprite); s_frzSpriteBuilt = true; }
        logger->Info("[CutsceneCtrl] BlurFX: freeze frame raster %dx%d ready", w, h);
        return true;
    }

    // ---- Real (GLES2 shader) blur ------------------------------------------
    static GLuint s_captureTex = 0;
    static GLuint s_program = 0;
    static GLint  s_locTexture = -1, s_locTexel = -1, s_locAlpha = -1;
    static bool   s_glReady = false;
    static bool   s_glFailed = false;    // don't retry (and re-log) a shader that will never compile
    static int    s_capW = 0, s_capH = 0;

    static const char* kVertSrc =
        "attribute vec2 aPos;\n"
        "attribute vec2 aTexCoord;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "    vTexCoord = aTexCoord;\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";

    // Single-pass 9-tap-per-ring Gaussian. Not a pristine separable
    // two-pass blur, but a very close approximation at a fraction of the
    // cost -- plenty for a full-screen pause dim.
    static const char* kFragSrc =
        "precision mediump float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform vec2 uTexelSize;\n"
        "uniform float uAlpha;\n"
        "void main() {\n"
        "    vec4 sum = texture2D(uTexture, vTexCoord) * 0.227027;\n"
        "    float w[4];\n"
        "    w[0] = 0.1945946; w[1] = 0.1216216; w[2] = 0.054054; w[3] = 0.016216;\n"
        "    for (int i = 0; i < 4; i++) {\n"
        "        vec2 off = uTexelSize * (float(i) + 1.0) * 2.2;\n"
        "        sum += texture2D(uTexture, vTexCoord + off) * w[i];\n"
        "        sum += texture2D(uTexture, vTexCoord - off) * w[i];\n"
        "        sum += texture2D(uTexture, vTexCoord + vec2(off.x, -off.y)) * w[i];\n"
        "        sum += texture2D(uTexture, vTexCoord - vec2(off.x, -off.y)) * w[i];\n"
        "    }\n"
        "    gl_FragColor = vec4(sum.rgb, uAlpha);\n"
        "}\n";

    static GLuint CompileShader(GLenum type, const char* src)
    {
        GLuint sh = glCreateShader(type);
        if (!sh) return 0;
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[512] = {0}; GLsizei len = 0;
            glGetShaderInfoLog(sh, sizeof(log) - 1, &len, log);
            logger->Info("[CutsceneCtrl] BlurFX shader compile error: %s", log);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    }

    // Builds the program + capture texture. Caller has already saved GL state.
    static void BuildGLResources()
    {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertSrc);
        GLuint fs = vs ? CompileShader(GL_FRAGMENT_SHADER, kFragSrc) : 0;
        if (!vs || !fs)
        {
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            s_glFailed = true;
            return;
        }

        s_program = glCreateProgram();
        glAttachShader(s_program, vs);
        glAttachShader(s_program, fs);
        glBindAttribLocation(s_program, 0, "aPos");
        glBindAttribLocation(s_program, 1, "aTexCoord");
        glLinkProgram(s_program);
        GLint linked = 0;
        glGetProgramiv(s_program, GL_LINK_STATUS, &linked);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (!linked)
        {
            logger->Info("[CutsceneCtrl] BlurFX: shader program link failed, using simulated blur instead");
            glDeleteProgram(s_program); s_program = 0;
            s_glFailed = true;
            return;
        }

        s_locTexture = glGetUniformLocation(s_program, "uTexture");
        s_locTexel   = glGetUniformLocation(s_program, "uTexelSize");
        s_locAlpha   = glGetUniformLocation(s_program, "uAlpha");

        glGenTextures(1, &s_captureTex);
        glBindTexture(GL_TEXTURE_2D, s_captureTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_glReady = true;
        logger->Info("[CutsceneCtrl] BlurFX: real Gaussian shader ready");
    }

    static void CaptureReal()
    {
        if (s_glFailed) return;
        int w = 0, h = 0;
        aml->GetDisplaySize(&w, &h);
        if (w <= 0 || h <= 0) return;

        GLSaved saved; saved.Save();
        if (!s_glReady) BuildGLResources();
        if (s_glReady)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_captureTex);
            // Copies the framebuffer currently bound (the frame the game just
            // finished drawing -- we're called from inside its render hook).
            glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, w, h, 0);
            s_capW = w; s_capH = h;
        }
        saved.Restore();
    }

    static void DrawReal(uint8_t alpha)
    {
        if (!s_glReady || s_capW <= 0) { DrawSimulated(alpha); return; }

        static const GLfloat verts[] = {
            -1.f, -1.f,  0.f, 0.f,
             1.f, -1.f,  1.f, 0.f,
            -1.f,  1.f,  0.f, 1.f,
             1.f,  1.f,  1.f, 1.f,
        };

        GLSaved saved; saved.Save();

        glBindBuffer(GL_ARRAY_BUFFER, 0);                 // we use client-side arrays
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);

        glUseProgram(s_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_captureTex);
        glUniform1i(s_locTexture, 0);
        glUniform2f(s_locTexel, 1.0f / (float)s_capW, 1.0f / (float)s_capH);
        glUniform1f(s_locAlpha, alpha / 255.0f);

        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        saved.Restore();
    }

    // ---- public API --------------------------------------------------------
    void Init() { /* resources are created lazily on first use */ }

    void Shutdown()
    {
        FreeFrozenResources();
        if (s_frzSpriteBuilt && Sym::CSprite2d_Dtor) { Sym::CSprite2d_Dtor(s_frzSprite); s_frzSpriteBuilt = false; }
        if (s_spriteConstructed && Sym::CSprite2d_Dtor) { Sym::CSprite2d_Dtor(s_spriteBuf); s_spriteConstructed = false; }
        if (s_blurTexture && Sym::RwTextureDestroy) { Sym::RwTextureDestroy(s_blurTexture); s_blurTexture = nullptr; }
        if (s_blurRaster && Sym::RwRasterDestroy) { Sym::RwRasterDestroy(s_blurRaster); s_blurRaster = nullptr; }
        // GL objects belong to the game's context, which may already be gone
        // at unload time -- leaking two small objects beats a crash.
        s_glReady = false;
    }

    bool CaptureFrozen()
    {
        s_frzValid = false;
        if (s_frzFailed) return false;
        if (!Sym::CPostEffects_pRasterFrontBuffer || !*Sym::CPostEffects_pRasterFrontBuffer ||
            !Sym::RwRasterPushContext || !Sym::RwRasterRenderFast || !Sym::RwRasterPopContext || !Sym::CSprite2d_Draw) { s_frzFailed = true; return false; }
        int w = 0, h = 0;
        aml->GetDisplaySize(&w, &h);
        if (w <= 0 || h <= 0 || !EnsureFrozenResources(w, h)) { s_frzFailed = true; return false; }

        Sym::RwRasterPushContext(s_frzRaster);
        Sym::RwRasterRenderFast(*Sym::CPostEffects_pRasterFrontBuffer, 0, 0);     // 1:1, the whole frame
        Sym::RwRasterPopContext();
        s_frzValid = true;
        return true;
    }

    void InvalidateFrozen() { s_frzValid = false; s_frzFailed = false; }
    bool HasFrozen()        { return s_frzValid && s_frzRaster && s_frzSpriteBuilt; }

    void DrawFrozen()
    {
        if (!HasFrozen() || !Sym::CSprite2d_Draw) return;
        *(void**)s_frzSprite = s_frzTexture;                  // CSprite2d's only real member is the texture pointer (see DrawSimulated)
        int w = 0, h = 0;
        aml->GetDisplaySize(&w, &h);
        CRect full(0.0f, (float)h, (float)w, 0.0f);
        CRGBA color(255, 255, 255, 255);
        Sym::CSprite2d_Draw(s_frzSprite, full, color);
    }

    void CaptureNow(bool useGaussianShader)
    {
        // Only touch the machinery for the mode actually selected. The
        // simulated capture is always kept as the fallback image.
        CaptureSimulated();
        if (useGaussianShader) CaptureReal();
    }

    void DrawFullscreen(uint8_t alpha, bool useGaussianShader)
    {
        if (useGaussianShader && s_glReady)
            DrawReal(alpha);
        else
            DrawSimulated(alpha);
    }
}

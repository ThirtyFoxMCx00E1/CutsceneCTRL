#pragma once
// Headless software GLES (Mesa llvmpipe) so the REAL BlurFX.cpp shader can be run and its pixels read back.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <cstdio>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
struct GLCtx {
    EGLConfig cfg = nullptr; EGLDisplay d = EGL_NO_DISPLAY; EGLSurface s = EGL_NO_SURFACE; EGLContext c = EGL_NO_CONTEXT; int w = 0, h = 0;
    bool Init(int W, int H) {
        w = W; h = H;
        auto gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint a, b; if (!eglInitialize(d, &a, &b)) return false;
        eglBindAPI(EGL_OPENGL_ES_API);
        EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint n = 0; if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n < 1) return false;
        EGLint pb[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
        s = eglCreatePbufferSurface(d, cfg, pb); if (s == EGL_NO_SURFACE) return false;
        EGLint cx[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, cx); if (c == EGL_NO_CONTEXT) return false;
        return eglMakeCurrent(d, s, s, c);
    }
    // Reads the framebuffer (GL origin bottom-left) and returns it top-row-first RGBA.
    std::vector<uint8_t> Read() {
        std::vector<uint8_t> raw(w * h * 4), out(w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        for (int y = 0; y < h; ++y) memcpy(&out[(size_t)y * w * 4], &raw[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        return out;
    }
    void SavePPM(const std::vector<uint8_t>& px, const char* path) {
        FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; ++i) fwrite(&px[i * 4], 1, 3, f);
        fclose(f);
    }
};

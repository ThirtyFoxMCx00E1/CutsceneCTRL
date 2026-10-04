#pragma once
// GLSaved -- capture / restore every piece of GL state this mod touches.
//
// The game's RenderWare OpenGL layer caches GL state and skips redundant
// binds. Any raw GL call that changes state without putting it back leaves
// that cache lying, and the visible symptom is the scene rendering wrong
// (dark, untextured, wrong blending) until something forces a rebind.
// So: Save() before we draw, Restore() after, always.
#include <GLES2/gl2.h>

struct GLSaved
{
    static constexpr int kAttribs = 4;

    GLint program = 0, tex2d = 0, activeTex = 0, arrayBuf = 0, elemBuf = 0, unpackAlign = 4;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint scissorBox[4] = {0, 0, 0, 0};
    GLboolean blend = 0, depth = 0, cull = 0, scissor = 0, stencil = 0, dither = 0;
    GLint bSrcRGB = 0, bDstRGB = 0, bSrcA = 0, bDstA = 0, bEqRGB = 0, bEqA = 0;
    GLboolean colorMask[4] = {1, 1, 1, 1};
    GLboolean depthMask = 1;
    struct Attrib { GLint enabled = 0, size = 0, type = 0, norm = 0, stride = 0, buf = 0; void* ptr = nullptr; } a[kAttribs];

    void Save()
    {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elemBuf);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlign);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        blend = glIsEnabled(GL_BLEND); depth = glIsEnabled(GL_DEPTH_TEST); cull = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST); stencil = glIsEnabled(GL_STENCIL_TEST); dither = glIsEnabled(GL_DITHER);
        glGetIntegerv(GL_BLEND_SRC_RGB, &bSrcRGB);     glGetIntegerv(GL_BLEND_DST_RGB, &bDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &bSrcA);     glGetIntegerv(GL_BLEND_DST_ALPHA, &bDstA);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &bEqRGB); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &bEqA);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        for (int i = 0; i < kAttribs; ++i)
        {
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a[i].enabled);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a[i].size);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a[i].type);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a[i].norm);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a[i].stride);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a[i].buf);
            glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a[i].ptr);
        }
    }

    void Restore()
    {
        for (int i = 0; i < kAttribs; ++i)
        {
            glBindBuffer(GL_ARRAY_BUFFER, a[i].buf);
            if (a[i].size > 0)
                glVertexAttribPointer(i, a[i].size, a[i].type, (GLboolean)a[i].norm, a[i].stride, a[i].ptr);
            if (a[i].enabled) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i);
        }
        glBindBuffer(GL_ARRAY_BUFFER, arrayBuf);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elemBuf);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlign);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex2d);
        glActiveTexture(activeTex);
        glUseProgram(program);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
        auto en = [](GLenum cap, GLboolean on) { if (on) glEnable(cap); else glDisable(cap); };
        en(GL_BLEND, blend); en(GL_DEPTH_TEST, depth); en(GL_CULL_FACE, cull);
        en(GL_SCISSOR_TEST, scissor); en(GL_STENCIL_TEST, stencil); en(GL_DITHER, dither);
        glBlendFuncSeparate(bSrcRGB, bDstRGB, bSrcA, bDstA);
        glBlendEquationSeparate(bEqRGB, bEqA);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glDepthMask(depthMask);
    }
};

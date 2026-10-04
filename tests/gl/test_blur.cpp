#include <mod/logger.h>
#include <mod/amlmod.h>
FakeLogger fl; FakeLogger* logger = &fl; FakeAML fa; FakeAML* aml = &fa;
#include "gl_harness.h"
#include "BlurFX.h"
#include "GameSymbols.h"
#include <cstdlib>
#define CHECK(c) do{ if(!(c)){ printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails; } else ++passes; }while(0)
static int fails=0, passes=0;
static void Fill(int x,int y,int w,int h, float r,float g,float b){ glEnable(GL_SCISSOR_TEST); glScissor(x,y,w,h); glClearColor(r,g,b,1); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST); }
int main(){
  GLCtx g; if(!g.Init(1600,720)){ printf("no GL\n"); return 9; }
  // a sharp vertical edge: left half white, right half black
  glViewport(0,0,1600,720); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); Fill(0,0,800,720, 1,1,1);
  // game-like state to be preserved
  GLuint gp = glCreateProgram(); glUseProgram(0); GLuint gt; glGenTextures(1,&gt); glBindTexture(GL_TEXTURE_2D, gt);
  glEnable(GL_DEPTH_TEST); glDisable(GL_BLEND); glBlendFunc(GL_ONE,GL_ZERO); (void)gp;
  BlurFX::CaptureNow(true);                                    // compiles the shader + copies the frame
  glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);         // wipe the frame; only the captured copy remains
  BlurFX::DrawFullscreen(255, true);
  auto px = g.Read(); auto at=[&](int x,int y){ return &px[((size_t)y*g.w+x)*4]; };
  int farLeft = at(100,360)[0], farRight = at(1500,360)[0];
  int edgeBand = 0; for(int x=770;x<830;++x){ int v=at(x,360)[0]; if(v>25 && v<230) ++edgeBand; }
  printf("far-left=%d far-right=%d, intermediate (blurred) pixels across the edge band: %d\n", farLeft, farRight, edgeBand);
  CHECK(farLeft > 235); CHECK(farRight < 20);                  // flat areas unchanged
  CHECK(edgeBand >= 4);                                        // a hard edge has 0-1 intermediate pixels; the blur spreads it over several
  GLint v; glGetIntegerv(GL_TEXTURE_BINDING_2D,&v); CHECK((GLuint)v==gt);
  glGetIntegerv(GL_CURRENT_PROGRAM,&v); CHECK(v==0);
  CHECK(glIsEnabled(GL_DEPTH_TEST)); CHECK(!glIsEnabled(GL_BLEND));
  glGetIntegerv(GL_BLEND_SRC_RGB,&v); CHECK(v==GL_ONE);
  CHECK(glGetError()==GL_NO_ERROR);
  printf("\n%d passed, %d failed\n", passes, fails); return fails?1:0;
}

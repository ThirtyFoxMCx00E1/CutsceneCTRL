#!/usr/bin/env bash
# Host-side tests (x86 Linux, no phone / game needed).  Run from anywhere:  tests/run.sh
#
#  * imgui : the REAL UI.cpp + ImGuiRW.cpp + Dear ImGui 1.89.7 driven with simulated fingers. The game's RenderWare 2D
#            renderer is replaced by a small software rasteriser, so the pixels checked are what the real code submits to
#            RwIm2DRenderIndexedPrimitive (font atlas upload, glyph quads, clipping, render states). Also writes
#            preview images to $TMPDIR/cutscenectrl_tests/ui_*.rgba (composite with tests/compose.py).
#  * logic : the REAL CutsceneCtrl.cpp pause state machine against fake game memory: stuck-pause safety nets, fades,
#            free-camera hand-back, the pause-freezes-time fix, config reset, and the RenderEffects -> Render2dStuff
#            hand-off that draws the UI after the HUD.
#  * blur  : the optional real-Gaussian blur shader (BlurFX.cpp) on a headless software GLES context (Mesa llvmpipe).
#  * freeze: BlurFX's freeze-frame capture / draw against a fake RenderWare (arguments, failure paths, no leaks).
#
# Needs: g++, zlib dev headers; for `blur` also Mesa EGL + GLESv2 runtime and the GLES2/EGL/KHR headers (GLES_INC, or the
# Android NDK's via $ANDROID_NDK). Python 3 + Pillow + numpy for compose.py.
set -euo pipefail
cd "$(dirname "$0")/.."
S=src; OUT=${TMPDIR:-/tmp}/cutscenectrl_tests; mkdir -p "$OUT"
export ASAN_OPTIONS=detect_leaks=0
SAN="-fsanitize=address,undefined -g -O1"

# Dear ImGui is compiled once, without sanitizers (it is third-party code and slow to instrument).
IMOBJ=""
for f in imgui imgui_draw imgui_tables imgui_widgets; do
  o="$OUT/$f.o"; IMOBJ="$IMOBJ $o"
  if [ ! -f "$o" ] || [ "$S/imgui/$f.cpp" -nt "$o" ]; then
    g++ -std=c++17 -O1 -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS -DIMGUI_DISABLE_FILE_FUNCTIONS -c "$S/imgui/$f.cpp" -o "$o"
  fi
done
IMFLAGS="-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS -DIMGUI_DISABLE_FILE_FUNCTIONS"

echo "== imgui UI (software RenderWare) =="
g++ -std=c++17 $SAN $IMFLAGS -Itests/logic -I$S tests/imgui/test_imgui.cpp $S/UI.cpp $S/ImGuiRW.cpp $S/GameText.cpp $S/GameSymbols.cpp $IMOBJ -o $OUT/t_imgui -lz
CUTSCENE_TEST_OUT=$OUT $OUT/t_imgui | grep -E "FAIL|passed|^==|^   [a-z]|^    [A-Za-z]" || true
test "${PIPESTATUS[0]}" -eq 0

echo "== state machine =="
g++ -std=c++17 $SAN -Itests/logic -I$S tests/logic/test_logic.cpp $S/CutsceneCtrl.cpp $S/GameSymbols.cpp -o $OUT/t_logic -lz
$OUT/t_logic | grep -E "FAIL|passed"
test "${PIPESTATUS[0]}" -eq 0

if [ -f /usr/lib/x86_64-linux-gnu/libEGL.so.1 ] || ldconfig -p 2>/dev/null | grep -q libEGL.so.1; then
  export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
  if [ -z "${GLES_INC:-}" ] && [ -n "${ANDROID_NDK:-${ANDROID_NDK_HOME:-}}" ]; then
    SYS="${ANDROID_NDK:-${ANDROID_NDK_HOME:-}}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include"
    GLES_INC="$OUT/glinc"; mkdir -p "$GLES_INC"; cp -r "$SYS"/GLES2 "$SYS"/EGL "$SYS"/KHR "$GLES_INC"/
  fi
  if [ -n "${GLES_INC:-}" ]; then
    echo "== blur shader =="
    g++ -std=c++17 $SAN -I$GLES_INC -Itests/gl -I$S tests/gl/test_blur.cpp $S/BlurFX.cpp $S/GameSymbols.cpp -o $OUT/t_blur -l:libEGL.so.1 -l:libGLESv2.so.2 -lz
    $OUT/t_blur | grep -E "FAIL|passed"
    echo "== freeze frame (fake RenderWare) =="
    g++ -std=c++17 $SAN -I$GLES_INC -Itests/gl -I$S tests/gl/test_frozen.cpp $S/BlurFX.cpp $S/GameSymbols.cpp -o $OUT/t_frozen -l:libEGL.so.1 -l:libGLESv2.so.2 -lz
    $OUT/t_frozen | grep -E "FAIL|passed"
  fi
else
  echo "== blur shader == (skipped: no Mesa EGL on this machine)"
fi

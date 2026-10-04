LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc

# Dear ImGui 1.89.7 -- the real source, copied unchanged from CLEO_ImGui-main/CLEO ImGui/jni (src/imgui/)
IMGUI_SRC := src/imgui/imgui.cpp \
             src/imgui/imgui_draw.cpp \
             src/imgui/imgui_tables.cpp \
             src/imgui/imgui_widgets.cpp

LOCAL_SRC_FILES := src/main.cpp \
                    src/GameSymbols.cpp \
                    src/CutsceneCtrl.cpp \
                    src/BlurFX.cpp \
                    src/UI.cpp \
                    src/ImGuiRW.cpp \
                    src/GameText.cpp \
                    src/PauseAudio.cpp \
                    $(IMGUI_SRC) \
                    mod/logger.cpp \
                    mod/config.cpp

# Matches the naming convention requested: libCutsceneCtrl32.so / libCutsceneCtrl64.so
# living side by side in the same AML mods folder.
ifeq ($(TARGET_ARCH_ABI), armeabi-v7a)
    LOCAL_MODULE := CutsceneCtrl32
else
    LOCAL_MODULE := CutsceneCtrl64
endif

LOCAL_CFLAGS += -O2 -DNDEBUG -std=c++17 -Wall -Wno-unused-parameter
# Our ImGui is private to this mod: the CLEO ImGui plugin (and any other mod) has its own copy, so nothing of ours
# may be exported / interposed. Only AML's entry points (marked JNIEXPORT) stay visible.
LOCAL_CFLAGS += -fvisibility=hidden -fvisibility-inlines-hidden
LOCAL_CFLAGS += -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS \
                -DIMGUI_DISABLE_FILE_FUNCTIONS
LOCAL_LDFLAGS += -Wl,--exclude-libs,ALL -Wl,-Bsymbolic
ifeq ($(TARGET_ARCH_ABI), armeabi-v7a)
    LOCAL_CFLAGS += -mfloat-abi=softfp
endif
LOCAL_C_INCLUDES += .
LOCAL_LDLIBS += -llog -lGLESv2 -lEGL -lOpenSLES -lz
include $(BUILD_SHARED_LIBRARY)

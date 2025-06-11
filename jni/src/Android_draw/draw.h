#ifndef NATIVESURFACE_DRAW_H
#define NATIVESURFACE_DRAW_H
// System libs
#include <iostream>
#include <thread>
#include <chrono>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES3/gl3platform.h>
#include <GLES3/gl3ext.h>
#include <GLES3/gl32.h>

// User libs
#include "native_surface/ANativeWindowCreator.h"
#include "ImGui/imgui.h"
#include "ImGui/font/Font.h"
#include "ImGui/imgui_internal.h"
//#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "Android_touch/TouchHelperA.h"

#if defined(USE_OPENGL)
    #include "ImGui/backends/imgui_impl_opengl3.h"
    //#include "OpenglUtils.h"
#else
    #include "ImGui/backends/imgui_impl_vulkan.h"
    #include "Android_vulkan/VulkanUtils.h"
#endif


// namespace
using namespace std;
using namespace std::chrono_literals;
//extern EGLDisplay display;
//extern EGLConfig config;
//extern EGLSurface surface;
//extern EGLContext context;

// 屏幕信息
extern android::ANativeWindowCreator::DisplayInfo displayInfo;
// 绝对屏幕X _ Y
extern bool g_Initialized;


extern ImGuiWindow *g_window;
// window创建的 大小
extern int native_window_screen_x, native_window_screen_y;

// Func
bool init_egl(uint32_t _screen_x, uint32_t _screen_y, bool log = false);

bool initGUI_draw(uint32_t _screen_x, uint32_t _screen_y, bool log = false);

bool ImGui_init();

void screen_config();

void drawBegin();

void drawEnd();

void shutdown();
// 布局UI
void Layout_tick_UI();

#endif //NATIVESURFACE_DRAW_H

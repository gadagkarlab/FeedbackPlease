// FeedbackPlease: real-time song syllable detection and feedback (C++ port of
// the LabVIEW DetectSyllables system).
//
//   FeedbackPlease.exe [settings.ini] [--autostart] [--tune <folder> [--analyze] [--summary]] [--closedloop] [--msn | --msn-load]

#include <cstdio>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // before GLFW so APIENTRY is defined once
#include <timeapi.h>
#endif

#include <GLFW/glfw3.h>

#include "app/Gui.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

namespace fp {
void installCrashHandler();
}

static void glfwError(int code, const char* desc) { std::fprintf(stderr, "GLFW error %d: %s\n", code, desc); }

int main(int argc, char** argv) {
    fp::installCrashHandler();
    std::string settings = "feedbackplease.ini", tuneFolder;
    bool autostart = false, analyze = false, summary = false;
    bool closedLoop = false;
    bool msn = false;
    bool msnLoad = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--autostart")
            autostart = true;
        else if (a == "--tune" && i + 1 < argc)
            tuneFolder = argv[++i];
        else if (a == "--analyze")
            analyze = true;
        else if (a == "--summary")
            summary = true;
        else if (a == "--closedloop")
            closedLoop = true;
        else if (a == "--msn" || a == "--msn-load")
            msn = true, msnLoad = a == "--msn-load";
        else
            settings = a;
    }

#ifdef _WIN32
    timeBeginPeriod(1);  // 1 ms scheduler resolution for the simulated source's pacing
#endif

    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1600, 950, "FeedbackPlease", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // layout is fixed; settings live in feedbackplease.ini

    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window, &xs, &ys);
    ImFontConfig cfg;
    cfg.SizePixels = 15.0f * xs;
    io.Fonts->AddFontDefault(&cfg);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(xs);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    {
        fp::Gui gui(settings);
        if (autostart) gui.startEngine();
        if (!tuneFolder.empty()) gui.openTune(tuneFolder, analyze, summary);
#ifdef FP_CLOSEDLOOP
        if (closedLoop) gui.openClosedLoop();
#else
        (void)closedLoop;
#endif
#ifdef FP_MSN
        if (msn) gui.openMsn(msnLoad);
#else
        (void)msn;
#endif
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            // Minimized: vsync no longer throttles the loop, so it would spin a core
            // that the acquisition thread may need. Just wait for events instead.
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                glfwWaitEventsTimeout(0.1);
                continue;
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            gui.draw();
            ImGui::Render();
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }
    }  // Gui destructor stops acquisition and saves settings

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return 0;
}

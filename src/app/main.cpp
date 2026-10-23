// FeedbackPlease: real-time song syllable detection and feedback (C++ port of
// the LabVIEW DetectSyllables system).
//
//   FeedbackPlease.exe [settings.ini] [--mode single|closedloop|multi] [--choose]
//                      [--autostart] [--tune <folder> [--analyze] [--summary]] [--closedloop] [--msn | --msn-load]
//
// Without --mode the launch screen asks which app to run (single box, single box +
// closed loop, multi-box), unless a choice was remembered in feedbackplease_launch.txt;
// --choose shows the launch screen anyway.

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // before GLFW so APIENTRY is defined once
#include <timeapi.h>
#endif

#include <GLFW/glfw3.h>

#include "app/Gui.h"
#include "app/Widgets.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

namespace fp {
void installCrashHandler();
}

using fp::AppMode;

static void glfwError(int code, const char* desc) { std::fprintf(stderr, "GLFW error %d: %s\n", code, desc); }

namespace {

const char* kLaunchFile = "feedbackplease_launch.txt";

bool parseMode(const std::string& s, AppMode& m) {
    for (AppMode x : {AppMode::Single, AppMode::ClosedLoop, AppMode::Multi})
        if (s == fp::modeKey(x)) {
            m = x;
            return true;
        }
    return false;
}

// The remembered launch choice, if "remember" was ticked.
bool readRemembered(AppMode& m) {
    std::ifstream in(kLaunchFile);
    std::string line, mode;
    bool remember = false;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r"));
            s.erase(s.find_last_not_of(" \t\r") + 1);
        };
        trim(k);
        trim(v);
        if (k == "mode") mode = v;
        if (k == "remember") remember = v == "1";
    }
    return remember && parseMode(mode, m);
}

void writeRemembered(AppMode m, bool remember) {
    std::ofstream out(kLaunchFile);
    out << "mode = " << fp::modeKey(m) << "\nremember = " << (remember ? 1 : 0) << "\n";
}

bool closedLoopModeAvailable() {
#if defined(FP_CLOSEDLOOP) || defined(FP_MSN)
    return true;
#else
    return false;
#endif
}

// One frame of the launch screen. Returns true once a mode has been picked.
bool drawLauncher(AppMode& mode, bool& remember) {
    using namespace fp::ui;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##launch", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings);
    const float w = 46.0f * em();
    ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - w) * 0.5f, vp->WorkSize.y * 0.18f));
    ImGui::BeginGroup();
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("FeedbackPlease");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Real-time syllable detection and feedback. What would you like to run?");
    ImGui::Dummy(ImVec2(0, em()));

    bool picked = false;
    auto option = [&](AppMode m, const char* title, const char* desc, bool enabled, const char* note) {
        ImGui::PushID(int(m));
        ImGui::BeginDisabled(!enabled);
        if (ImGui::Button(title, ImVec2(w, 2.6f * em()))) {
            mode = m;
            picked = true;
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextDisabled("%s", desc);
        if (note && *note) ImGui::TextColored(kAmber, "%s", note);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 0.6f * em()));
        ImGui::PopID();
    };
    option(AppMode::Single, "1.  Single box: recording and feedback",
           "One box: microphone, syllable detection, feedback, TTL and syllable-code outputs. Tabs: Live and Tune.",
           true, nullptr);
    const char* clNote =
#if defined(FP_CLOSEDLOOP) && defined(FP_MSN)
        nullptr;
#elif defined(FP_MSN)
        "This build has no SpikeGLX SDK, so the Closed loop tab is left out (the MSN finder is there).";
#elif defined(FP_CLOSEDLOOP)
        "This build has no MSN finder.";
#else
        "Not available in this build (needs the SpikeGLX SDK or the MSN finder).";
#endif
    option(AppMode::ClosedLoop, "2.  Single box + closed loop",
           "Everything in 1, plus the Closed loop (spike-triggered feedback) and MSN finder tabs, and the "
           "closed-loop pulse feedback (noise burst on the closed loop's digital pulse).",
           closedLoopModeAvailable(), clNote);
    option(AppMode::Multi, "3.  Multi-box",
           "Several boxes recording at once, on one or more NI cards. The Live tab shows every box. The Tune tab has "
           "its own settings: audition a folder, then copy what you tuned to any box.",
           true, nullptr);
    ImGui::Checkbox("Remember my choice and skip this screen next time", &remember);
    ImGui::TextDisabled("(Switch mode... in the toolbar brings you back here; --choose on the command line too.)");
    ImGui::EndGroup();
    ImGui::End();
    return picked;
}

}  // namespace

int main(int argc, char** argv) {
    fp::installCrashHandler();
    std::string settings, tuneFolder;
    bool autostart = false, analyze = false, summary = false;
    bool closedLoop = false, msn = false, msnLoad = false, choose = false, haveMode = false;
    AppMode mode = AppMode::Single;
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
        else if (a == "--mode" && i + 1 < argc)
            haveMode = parseMode(argv[++i], mode);
        else if (a == "--choose")
            choose = true;
        else
            settings = a;
    }
    if (!haveMode && (closedLoop || msn) && closedLoopModeAvailable()) {
        mode = AppMode::ClosedLoop;  // those tabs only exist in the closed-loop mode
        haveMode = true;
    }
    bool remember = false;
    if (!haveMode && !choose) haveMode = readRemembered(mode), remember = haveMode;

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
    io.IniFilename = nullptr;  // layout is fixed; settings live in the .ini files

    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window, &xs, &ys);
    ImFontConfig cfg;
    cfg.SizePixels = 15.0f * xs;
    io.Fonts->AddFontDefault(&cfg);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(xs);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    std::unique_ptr<fp::Gui> gui;
    bool firstGui = true;
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
        if (!gui && haveMode) {
            // Each mode has its own default settings file, so a multi-box rig and a single
            // box don't overwrite each other.
            const std::string file = !settings.empty() ? settings
                                     : mode == AppMode::Multi ? "feedbackplease_multi.ini"
                                                              : "feedbackplease.ini";
            gui = std::make_unique<fp::Gui>(file, mode);
            glfwSetWindowTitle(window, (std::string("FeedbackPlease - ") + fp::toString(mode)).c_str());
            if (firstGui) {  // command-line actions apply to the first app only
                firstGui = false;
                if (autostart) gui->startEngine();
                if (!tuneFolder.empty()) gui->openTune(tuneFolder, analyze, summary);
#ifdef FP_CLOSEDLOOP
                if (closedLoop && mode == AppMode::ClosedLoop) gui->openClosedLoop();
#endif
#ifdef FP_MSN
                if (msn && mode == AppMode::ClosedLoop) gui->openMsn(msnLoad);
#endif
            }
        }
        if (gui) {
            gui->draw();
            if (gui->switchModeRequested()) {
                gui.reset();  // stops acquisition and saves its settings
                haveMode = false;
                glfwSetWindowTitle(window, "FeedbackPlease");
            }
        } else if (drawLauncher(mode, remember)) {
            writeRemembered(mode, remember);
            haveMode = true;
        }
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    gui.reset();  // Gui destructor stops acquisition and saves settings

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

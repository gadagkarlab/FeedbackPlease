// Tests for the closed-loop tab's settings and runner. No SpikeGLX or hardware needed: the runner
// is only exercised on its failure paths (no server, no Python), which still cover the worker
// thread, the state machine, the log and the child-process capture.
// Run: build/Release/fp_closedloop_test.exe   (needs SglxApi.dll next to it)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app/ClosedLoop.h"

using namespace fp;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static bool waitIdle(ClosedLoopRunner& r, double sec) {
    const auto t0 = std::chrono::steady_clock::now();
    while (r.busy()) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > sec) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

static bool logHas(ClosedLoopRunner& r, const std::string& needle) {
    std::vector<std::string> lines;
    r.fetchLog(0, lines);
    for (const auto& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

static void testWindow() {
    ClParams p;  // defaults: centre 57, 4 x 20 ms sub-windows overlapping by 2
    const ClWindow w = clWindow(p);
    CHECK(w.valid);
    CHECK(w.stepMs == 18.0 && w.spanMs == 74.0 && w.startMs == 20.0 && w.endMs == 94.0);
    p.subWinOverlapMs = 20;
    CHECK(!clWindow(p).valid);
}

static void testRoundTrip(const fs::path& dir) {
    ClParams a;
    a.host = "10.0.0.5";
    a.port = 4143;
    a.filterDir = "D:/filters out";
    a.targetId = 853;
    a.carShank = "2";
    a.skipCalibration = true;
    a.calibrationBackend = "cpp";
    a.trainingBinPath = "D:/raw/Lav69_d1.0_g0_tcat.imec0.ap.bin";
    a.calibrationTrainFrac = 0.7;
    a.calibrationRidge = 2.5e-4;
    a.niSyllableLines = "4,5,6";
    a.targetSyllable = 3;
    a.windowCenterMs = 61.5;
    a.decisionMode = "push_up";
    a.spikeCountThreshold = 2.5;
    a.adaptiveRenditions = 40;
    a.doLine = "Dev2/port0/line1";
    a.spikeTimesPath = "D:/out/spikes.txt";

    const std::string path = (dir / "cl.txt").string();
    CHECK(saveClParams(path, a));
    ClParams b;
    std::string err;
    CHECK(loadClParams(path, b, &err));
    CHECK(b.host == a.host && b.port == a.port && b.filterDir == a.filterDir && b.targetId == 853);
    CHECK(b.carShank == "2" && b.skipCalibration && b.calibrationBackend == "cpp");
    CHECK(b.trainingBinPath == a.trainingBinPath && b.calibrationTrainFrac == 0.7 && b.calibrationRidge == 2.5e-4);
    CHECK(b.niSyllableLines == "4,5,6" && b.targetSyllable == 3 && b.windowCenterMs == 61.5);
    CHECK(b.decisionMode == "push_up" && b.spikeCountThreshold == 2.5 && b.adaptiveRenditions == 40);
    CHECK(b.doLine == a.doLine && b.spikeTimesPath == a.spikeTimesPath);
    // An empty script is saved as the default, so ClosedLoop.exe can use the same file.
    CHECK(b.calibrationScript == clDefaultCalibrationScript());

    CHECK(!loadClParams((dir / "missing.txt").string(), b, &err));
    CHECK(!err.empty());
}

// Hand-written file in ClosedLoop's own format, comments and all, with the old windowStartMs key.
static void testLegacyConfig(const fs::path& dir) {
    const std::string path = (dir / "legacy.txt").string();
    {
        std::ofstream f(path);
        f << "# comment\n\nsglxPort = 4142\nfilterDir=D:/x\ntargetId=7\n"
             "subWinDurMs=20\nsubWinOverlapMs=2\nnumSubWin=4\nwindowStartMs=20\ndoPulseMs=25  \n";
    }
    ClParams p;
    CHECK(loadClParams(path, p));
    CHECK(p.targetId == 7 && p.doPulseMs == 25 && p.filterDir == "D:/x");
    CHECK(p.windowCenterMs == 57.0);  // 20 + 74 / 2
}

static void writeFilter(const fs::path& dir, int id, int T, int nCh) {
    fs::create_directories(dir);
    std::vector<int32_t> ch(nCh);
    for (int i = 0; i < nCh; ++i) ch[i] = i;
    std::ofstream(dir / ("channels_" + std::to_string(id) + ".bin"), std::ios::binary)
        .write(reinterpret_cast<const char*>(ch.data()), ch.size() * sizeof(int32_t));
    std::vector<double> taps(size_t(T) * nCh, 0.01);
    std::ofstream(dir / ("filter_" + std::to_string(id) + ".bin"), std::ios::binary)
        .write(reinterpret_cast<const char*>(taps.data()), taps.size() * sizeof(double));
    const double th = 12.5;
    std::ofstream(dir / ("threshold_" + std::to_string(id) + ".bin"), std::ios::binary)
        .write(reinterpret_cast<const char*>(&th), sizeof th);
}

static void testFilterInfo(const fs::path& dir) {
    ClParams p;
    p.filterDir = (dir / "filters").string();
    p.targetId = 1;
    CHECK(!inspectClFilter(p).ok);  // nothing saved yet
    writeFilter(dir / "filters", 1, p.templateLength, 3);
    const ClFilterInfo info = inspectClFilter(p);
    CHECK(info.ok && info.nChannels == 3 && info.threshold == 12.5);
}

static void testLiveFailsCleanly(const fs::path& dir) {
    ClosedLoopRunner r;
    ClParams p;
    p.filterDir = (dir / "filters").string();
    p.targetId = 1;
    p.carShank = "-1";
    p.host = "127.0.0.1";
    p.port = 1;  // nothing listens here
    p.doLine = "Dev1/port0/line0";

    std::string err;
    CHECK(r.startLive(p, false, err));
    CHECK(!r.startLive(p, false, err));  // already running
    CHECK(waitIdle(r, 30));
    CHECK(r.state() == ClosedLoopRunner::State::Idle);
    CHECK(r.lastError().find("Could not connect") != std::string::npos);
    CHECK(logHas(r, "Decision window: centre 57 ms, span 74 ms"));

    // Bad settings are caught before any connection is tried.
    ClParams bad = p;
    bad.decisionMode = "sideways";
    CHECK(r.startLive(bad, false, err));
    CHECK(waitIdle(r, 30));
    CHECK(r.lastError().find("decisionMode") != std::string::npos);

    // And the runner is reusable afterwards.
    CHECK(r.startPulseTest(p, err));
    CHECK(waitIdle(r, 30));
    CHECK(!r.lastError().empty());
}

static void testCalibrationFailsCleanly(const fs::path& dir) {
    ClosedLoopRunner r;
    ClParams p;
    p.filterDir = (dir / "filters").string();
    p.targetId = 1;
    p.trainingBinPath = (dir / "train.bin").string();
    p.trainingKsDir = (dir / "ks").string();
    std::ofstream(p.trainingBinPath) << "x";
    fs::create_directories(p.trainingKsDir);

    // Unset fields are reported by name before anything is launched.
    ClParams empty;
    empty.pythonExe = "python";  // the bare name is rejected
    std::string err;
    CHECK(r.startCalibration(empty, err));
    CHECK(waitIdle(r, 30));
    CHECK(r.lastError().find("Filter folder") != std::string::npos);
    CHECK(r.lastError().find("Python") != std::string::npos);

    p.pythonExe = "no_such_python_for_fp_test.exe";
    CHECK(r.startCalibration(p, err));
    CHECK(waitIdle(r, 30));
    CHECK(r.lastError().find("could not launch") != std::string::npos);

    // A process that does start has its output captured line by line and its exit code reported.
    p.pythonExe = "C:/Windows/System32/where.exe";  // "where.exe -u script ..." prints an error and exits nonzero
    CHECK(r.startCalibration(p, err));
    CHECK(waitIdle(r, 30));
    CHECK(logHas(r, "where.exe"));
    CHECK(!r.lastError().empty());
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "fp_closedloop_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::printf("Window geometry\n");
    testWindow();
    std::printf("Config round trip\n");
    testRoundTrip(dir);
    std::printf("Legacy config\n");
    testLegacyConfig(dir);
    std::printf("Filter info\n");
    testFilterInfo(dir);
    std::printf("Live start fails cleanly without SpikeGLX\n");
    testLiveFailsCleanly(dir);
    std::printf("Calibration fails cleanly without Python\n");
    testCalibrationFailsCleanly(dir);

    fs::remove_all(dir);
    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All tests passed\n");
    return 0;
}

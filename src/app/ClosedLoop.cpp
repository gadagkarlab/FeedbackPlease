#include "app/ClosedLoop.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "SglxCppClient.h"

#include "closedloop/Calibration.h"
#include "closedloop/Config.h"
#include "closedloop/DecisionThread.h"
#include "closedloop/Events.h"
#include "closedloop/FilterBank.h"
#include "closedloop/ImecFetchThread.h"
#include "closedloop/Log.h"
#include "closedloop/NiFetchThread.h"
#include "closedloop/SglxMetaReader.h"
#include "closedloop/Stats.h"
#include "closedloop/ThreadSafeQueue.h"

namespace fp {

namespace {

typedef std::chrono::steady_clock Clock;

constexpr size_t kMaxLogLines = 4000;
constexpr size_t kMaxRows = 5000;

#ifndef FP_CLOSEDLOOP_DEFAULT_SCRIPT
#define FP_CLOSEDLOOP_DEFAULT_SCRIPT "FilterGen/calibrate_for_closedloop.py"
#endif

std::string exeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return ".";
    const std::string path(buf, n);
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? "." : path.substr(0, slash);
#else
    return ".";
#endif
}

// Relative paths resolve against the executable's folder, as in ClosedLoop.exe (the working
// directory depends on how the GUI was launched).
std::string resolveRelativeToExe(const std::string& path) {
    if (path.empty()) return path;
    const bool isAbsolute = (path.size() >= 2 && path[1] == ':') || path[0] == '/' || path[0] == '\\';
    return isAbsolute ? path : exeDir() + "\\" + path;
}

std::string fmtG(double v) {
    std::ostringstream s;
    s.precision(10);
    s << v;
    return s.str();
}

Config toConfig(const ClParams& p) {
    Config c;
    c.set("sglxHost", p.host);
    c.set("sglxPort", std::to_string(p.port));
    c.set("filterDir", p.filterDir);
    c.set("targetId", std::to_string(p.targetId));
    c.set("templateLength", std::to_string(p.templateLength));
    c.set("carShank", p.carShank);
    c.set("applyHighpass", p.applyHighpass ? "true" : "false");
    c.set("highpassCutoffHz", fmtG(p.highpassCutoffHz));
    c.set("skipCalibration", p.skipCalibration ? "true" : "false");
    c.set("calibrationBackend", p.calibrationBackend);
    c.set("trainingBinPath", p.trainingBinPath);
    c.set("trainingKsDir", p.trainingKsDir);
    c.set("pythonExe", p.pythonExe);
    c.set("calibrationScript", p.calibrationScript.empty() ? std::string(FP_CLOSEDLOOP_DEFAULT_SCRIPT) : p.calibrationScript);
    c.set("nearestChans", std::to_string(p.nearestChans));
    c.set("filterNChannels", std::to_string(p.filterNChannels));
    c.set("templateOffset", std::to_string(p.templateOffset));
    c.set("calibrationTrainFrac", fmtG(p.calibrationTrainFrac));
    c.set("calibrationMaxMinutes", fmtG(p.calibrationMaxMinutes));
    c.set("calibrationRidge", fmtG(p.calibrationRidge));
    c.set("calibrationMaxSpikes", std::to_string(p.calibrationMaxSpikes));
    c.set("calibrationSeed", std::to_string(p.calibrationSeed));
    c.set("calibrationLogPath", p.calibrationLogPath);
    c.set("calibrationCriterion", p.calibrationCriterion);
    c.set("maxFalsePositiveRateHz", fmtG(p.maxFalsePositiveRateHz));
    c.set("fetchChunkMs", std::to_string(p.fetchChunkMs));
    c.set("streamTimeoutSec", fmtG(p.streamTimeoutSec));
    c.set("niSyllableLines", p.niSyllableLines);
    c.set("niDebounceSamples", std::to_string(p.niDebounceSamples));
    c.set("niRearmMs", fmtG(p.niRearmMs));
    c.set("niRefractoryMs", fmtG(p.niRefractoryMs));
    c.set("targetSyllable", std::to_string(p.targetSyllable));
    c.set("windowCenterMs", fmtG(p.windowCenterMs));
    c.set("subWinDurMs", fmtG(p.subWinDurMs));
    c.set("subWinOverlapMs", fmtG(p.subWinOverlapMs));
    c.set("numSubWin", std::to_string(p.numSubWin));
    c.set("decisionMode", p.decisionMode);
    c.set("spikeCountThreshold", fmtG(p.spikeCountThreshold));
    c.set("adaptiveRenditions", std::to_string(p.adaptiveRenditions));
    c.set("doLine", p.doLine);
    c.set("doPulseMs", std::to_string(p.doPulseMs));
    c.set("spikeTimesPath", p.spikeTimesPath);
    c.set("syllableTimesPath", p.syllableTimesPath);
    c.set("decisionLogPath", p.decisionLogPath);
    c.set("runSeconds", fmtG(p.runSeconds));
    return c;
}

void fromConfig(const Config& c, ClParams& p) {
    auto S = [&](const char* k, std::string& v) { if (c.has(k)) v = c.getString(k); };
    auto I = [&](const char* k, int& v) { if (c.has(k)) v = c.getInt(k); };
    auto D = [&](const char* k, double& v) { if (c.has(k)) v = c.getDouble(k); };
    auto B = [&](const char* k, bool& v) { if (c.has(k)) v = c.getBool(k); };
    S("sglxHost", p.host);
    I("sglxPort", p.port);
    S("filterDir", p.filterDir);
    I("targetId", p.targetId);
    I("templateLength", p.templateLength);
    S("carShank", p.carShank);
    B("applyHighpass", p.applyHighpass);
    D("highpassCutoffHz", p.highpassCutoffHz);
    B("skipCalibration", p.skipCalibration);
    S("calibrationBackend", p.calibrationBackend);
    S("trainingBinPath", p.trainingBinPath);
    S("trainingKsDir", p.trainingKsDir);
    S("pythonExe", p.pythonExe);
    S("calibrationScript", p.calibrationScript);
    I("nearestChans", p.nearestChans);
    I("filterNChannels", p.filterNChannels);
    I("templateOffset", p.templateOffset);
    D("calibrationTrainFrac", p.calibrationTrainFrac);
    D("calibrationMaxMinutes", p.calibrationMaxMinutes);
    D("calibrationRidge", p.calibrationRidge);
    I("calibrationMaxSpikes", p.calibrationMaxSpikes);
    I("calibrationSeed", p.calibrationSeed);
    S("calibrationLogPath", p.calibrationLogPath);
    S("calibrationCriterion", p.calibrationCriterion);
    D("maxFalsePositiveRateHz", p.maxFalsePositiveRateHz);
    I("fetchChunkMs", p.fetchChunkMs);
    D("streamTimeoutSec", p.streamTimeoutSec);
    S("niSyllableLines", p.niSyllableLines);
    I("niDebounceSamples", p.niDebounceSamples);
    D("niRearmMs", p.niRearmMs);
    D("niRefractoryMs", p.niRefractoryMs);
    I("targetSyllable", p.targetSyllable);
    D("windowCenterMs", p.windowCenterMs);
    D("subWinDurMs", p.subWinDurMs);
    D("subWinOverlapMs", p.subWinOverlapMs);
    I("numSubWin", p.numSubWin);
    S("decisionMode", p.decisionMode);
    D("spikeCountThreshold", p.spikeCountThreshold);
    I("adaptiveRenditions", p.adaptiveRenditions);
    S("doLine", p.doLine);
    I("doPulseMs", p.doPulseMs);
    S("spikeTimesPath", p.spikeTimesPath);
    S("syllableTimesPath", p.syllableTimesPath);
    S("decisionLogPath", p.decisionLogPath);
    D("runSeconds", p.runSeconds);

    // Old configs gave the window start; the panel uses the centre of the whole span.
    if (!c.has("windowCenterMs") && c.has("windowStartMs"))
        p.windowCenterMs = c.getDouble("windowStartMs") + clWindow(p).spanMs / 2.0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Params <-> file

std::string clDefaultCalibrationScript() { return FP_CLOSEDLOOP_DEFAULT_SCRIPT; }

ClWindow clWindow(const ClParams& p) {
    ClWindow w;
    w.stepMs = p.subWinDurMs - p.subWinOverlapMs;
    w.valid = p.subWinDurMs > 0 && p.subWinOverlapMs >= 0 && w.stepMs > 0 && p.numSubWin >= 1;
    if (!w.valid) return w;
    w.spanMs = (p.numSubWin - 1) * w.stepMs + p.subWinDurMs;
    w.startMs = p.windowCenterMs - w.spanMs / 2.0;
    w.endMs = w.startMs + w.spanMs;
    return w;
}

bool loadClParams(const std::string& path, ClParams& p, std::string* err) {
    try {
        fromConfig(Config::load(path), p);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool saveClParams(const std::string& path, const ClParams& p, std::string* err) {
    std::ofstream f(path);
    if (!f) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    const Config c = toConfig(p);
    auto put = [&](const char* k) { f << k << "=" << c.getString(k) << "\n"; };
    auto head = [&](const char* title) { f << "\n# --- " << title << " ---\n"; };
    f << "# ClosedLoop config written by FeedbackPlease; the same file ClosedLoop.exe reads.\n";
    head("SpikeGLX connection");
    put("sglxHost"); put("sglxPort");
    head("Filter");
    put("filterDir"); put("targetId"); put("templateLength");
    head("Preprocessing (must match the filter's training)");
    put("carShank"); put("applyHighpass"); put("highpassCutoffHz");
    head("Calibration");
    put("skipCalibration"); put("calibrationBackend"); put("trainingBinPath"); put("trainingKsDir");
    put("pythonExe"); put("calibrationScript"); put("nearestChans"); put("filterNChannels");
    put("templateOffset"); put("calibrationTrainFrac"); put("calibrationMaxMinutes");
    put("calibrationRidge"); put("calibrationMaxSpikes"); put("calibrationSeed");
    put("calibrationLogPath"); put("calibrationCriterion"); put("maxFalsePositiveRateHz");
    head("Fetch cadence");
    put("fetchChunkMs"); put("streamTimeoutSec");
    head("NI digital line conventions");
    put("niSyllableLines"); put("niDebounceSamples"); put("niRearmMs"); put("niRefractoryMs");
    head("Decision logic");
    put("targetSyllable"); put("windowCenterMs"); put("subWinDurMs"); put("subWinOverlapMs");
    put("numSubWin"); put("decisionMode"); put("spikeCountThreshold"); put("adaptiveRenditions");
    put("doLine"); put("doPulseMs");
    head("Output");
    put("spikeTimesPath"); put("syllableTimesPath"); put("decisionLogPath"); put("runSeconds");
    if (!f) {
        if (err) *err = "write failed: " + path;
        return false;
    }
    return true;
}

ClFilterInfo inspectClFilter(const ClParams& p) {
    ClFilterInfo info;
    if (p.filterDir.empty()) {
        info.text = "No filter folder set.";
        return info;
    }
    try {
        const FilterBank fb = FilterBank::load(p.filterDir, p.targetId, p.templateLength);
        info.ok = true;
        info.nChannels = fb.nChannels();
        info.threshold = fb.threshold;
        std::ostringstream s;
        s << fb.nChannels() << " channels, threshold " << fb.threshold;
        try {
            namespace fs = std::filesystem;
            const auto t = fs::last_write_time(fs::path(p.filterDir) / ("filter_" + std::to_string(p.targetId) + ".bin"));
            const auto sys = std::chrono::system_clock::now() +
                             std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now());
            const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
            char buf[64];
            std::tm tmv{};
#ifdef _WIN32
            localtime_s(&tmv, &tt);
#else
            localtime_r(&tt, &tmv);
#endif
            std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
            s << ", fitted " << buf;
        } catch (...) {
        }
        info.text = s.str();
    } catch (const std::exception& e) {
        info.text = e.what();
    }
    return info;
}

// ---------------------------------------------------------------------------------------------
// Runner

struct ClosedLoopRunner::Impl {
    LiveStats stats;
    Clock::time_point liveStart;
    std::atomic<bool> live{false};
#ifdef _WIN32
    std::mutex childMu;
    HANDLE child = nullptr;  // running calibration process, so stop() can kill it
#endif
};

ClosedLoopRunner::ClosedLoopRunner() : impl_(new Impl) {
    clog::setSink([this](const std::string& line) { log(line); });
}

ClosedLoopRunner::~ClosedLoopRunner() {
    stop();
    if (worker_.joinable()) worker_.join();
    clog::setSink(nullptr);
}

const char* ClosedLoopRunner::stateName(State s) {
    switch (s) {
        case State::Idle: return "Idle";
        case State::Calibrating: return "Calibrating";
        case State::Connecting: return "Connecting";
        case State::Live: return "Live";
        case State::Stopping: return "Stopping";
    }
    return "";
}

void ClosedLoopRunner::log(const std::string& line) {
    std::lock_guard<std::mutex> lk(mu_);
    log_.push_back(line);
    while (log_.size() > kMaxLogLines) {
        log_.pop_front();
        ++logBase_;
    }
}

void ClosedLoopRunner::setError(const std::string& e) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        error_ = e;
    }
    log("ERROR: " + e);
}

std::string ClosedLoopRunner::lastError() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

size_t ClosedLoopRunner::fetchLog(size_t from, std::vector<std::string>& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    const size_t end = logBase_ + log_.size();
    for (size_t i = std::max(from, logBase_); i < end; ++i) out.push_back(log_[i - logBase_]);
    return end;
}

size_t ClosedLoopRunner::fetchDecisions(size_t from, std::vector<ClDecisionRow>& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    const size_t end = rowsBase_ + rows_.size();
    for (size_t i = std::max(from, rowsBase_); i < end; ++i) out.push_back(rows_[i - rowsBase_]);
    return end;
}

ClCounters ClosedLoopRunner::counters() const {
    ClCounters c;
    {
        std::lock_guard<std::mutex> lk(mu_);
        c = counters_;
    }
    c.spikes = impl_->stats.spikes.load(std::memory_order_relaxed);
    c.syllables = impl_->stats.syllables.load(std::memory_order_relaxed);
    c.syllablesLost = impl_->stats.syllablesLost.load(std::memory_order_relaxed);
    c.imecSample = impl_->stats.imecSample.load(std::memory_order_relaxed);
    if (impl_->live && state() == State::Live)
        c.elapsedSec = std::chrono::duration<double>(Clock::now() - impl_->liveStart).count();
    return c;
}

void ClosedLoopRunner::stop() {
    stopRequested_.store(true);
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(impl_->childMu);
    if (impl_->child) TerminateProcess(impl_->child, 1);
#endif
}

void ClosedLoopRunner::spawn(std::string& err, void (ClosedLoopRunner::*job)()) {
    (void)err;
    if (worker_.joinable()) worker_.join();  // previous job already finished (state is Idle)
    stopRequested_.store(false);
    streamLost_.store(false);
    {
        std::lock_guard<std::mutex> lk(mu_);
        error_.clear();
    }
    worker_ = std::thread(job, this);
}

namespace {
bool beginJob(std::atomic<ClosedLoopRunner::State>& st, ClosedLoopRunner::State to, std::string& err) {
    ClosedLoopRunner::State idle = ClosedLoopRunner::State::Idle;
    if (!st.compare_exchange_strong(idle, to)) {
        err = "Closed loop is already running";
        return false;
    }
    return true;
}
}  // namespace

bool ClosedLoopRunner::startCalibration(const ClParams& p, std::string& err) {
    if (!beginJob(state_, State::Calibrating, err)) return false;
    job_.reset(new ClParams(p));
    spawn(err, &ClosedLoopRunner::calibrationJob);
    return true;
}

bool ClosedLoopRunner::startLive(const ClParams& p, bool calibrateFirst, std::string& err) {
    if (!beginJob(state_, calibrateFirst ? State::Calibrating : State::Connecting, err)) return false;
    job_.reset(new ClParams(p));
    jobCalibrateFirst_ = calibrateFirst;
    spawn(err, &ClosedLoopRunner::liveJob);
    return true;
}

bool ClosedLoopRunner::startPulseTest(const ClParams& p, std::string& err) {
    if (!beginJob(state_, State::Connecting, err)) return false;
    job_.reset(new ClParams(p));
    spawn(err, &ClosedLoopRunner::pulseJob);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Calibration

#ifdef _WIN32
// Runs `commandLine` with its stdout/stderr piped into the log, one message per line.
// Returns the exit code, or -1 if it could not be launched. stop() kills the process.
int ClosedLoopRunner::runProcessCapture(const std::string& commandLine) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(commandLine.begin(), commandLine.end());
    buf.push_back('\0');
    const BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->childMu);
        impl_->child = pi.hProcess;
        if (stopRequested_.load()) TerminateProcess(pi.hProcess, 1);  // Stop pressed while launching
    }

    std::string pending;
    auto flush = [&]() {
        while (!pending.empty() && (pending.back() == ' ' || pending.back() == '\t')) pending.pop_back();
        if (!pending.empty()) log(pending);
        pending.clear();
    };
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            if (chunk[i] == '\n' || chunk[i] == '\r')
                flush();
            else
                pending.push_back(chunk[i]);
        }
    }
    flush();
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    {
        std::lock_guard<std::mutex> lk(impl_->childMu);
        impl_->child = nullptr;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}
#else
int ClosedLoopRunner::runProcessCapture(const std::string&) { return -1; }
#endif

namespace {

// CAR group = one shank of the probe, read from the training recording's own .meta so the live
// and training groups can't disagree. See LiveSorter_BF/README.md ("Preprocessing must match training").
std::vector<int> resolveCarChannels(const Config& cfg, const std::string& filterDir, int targetId) {
    const std::string carShankStr = cfg.getString("carShank", "-1");
    int carShank;
    if (carShankStr == "auto") {
        const std::string path = filterDir + "/car_shank_" + std::to_string(targetId) + ".bin";
        std::ifstream fh(path.c_str(), std::ios::binary);
        int32_t v = -1;
        if (!fh.read(reinterpret_cast<char*>(&v), sizeof(v)))
            throw std::runtime_error("carShank=auto: could not read '" + path +
                                     "' -- run a Python calibration for this target first, or set carShank explicitly");
        carShank = v;
        ClLog() << "carShank=auto -> shank " << carShank << " (from " << path << ")";
    } else {
        size_t used = 0;
        try {
            carShank = std::stoi(carShankStr, &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (used == 0 || used != carShankStr.size())
            throw std::runtime_error("carShank must be 'auto', -1, or a 0-based shank index, got '" + carShankStr + "'");
    }

    std::vector<int> ids;
    if (carShank >= 0) {
        const std::string bin = cfg.requireString("trainingBinPath");
        const std::string metaPath = bin.substr(0, bin.find_last_of('.')) + ".meta";
        const SglxMeta meta = SglxMeta::load(metaPath);
        ids = meta.shankChannels(carShank);
        ClLog() << "CAR group: " << ids.size() << " channels on shank " << carShank << " (from " << metaPath << ")";
    } else {
        ClLog() << "carShank=-1 -- CAR disabled";
    }
    return ids;
}

}  // namespace

bool ClosedLoopRunner::runCalibration(const ClParams& p) {
    // Catch the usual set-up mistakes up front, with a message that says which field to fill in.
    {
        namespace fs = std::filesystem;
        std::vector<std::string> missing;
        if (p.filterDir.empty()) missing.push_back("Filter folder (where the fitted filter is written)");
        if (p.trainingBinPath.empty()) missing.push_back("Training .bin");
        else if (!fs::exists(p.trainingBinPath)) missing.push_back("Training .bin (file not found: " + p.trainingBinPath + ")");
        if (p.trainingKsDir.empty()) missing.push_back("Kilosort folder");
        else if (!fs::is_directory(p.trainingKsDir)) missing.push_back("Kilosort folder (not found: " + p.trainingKsDir + ")");
        if (p.calibrationBackend == "python") {
            const bool pathLike = p.pythonExe.find_first_of("/\\") != std::string::npos;
            if (p.pythonExe.empty() || p.pythonExe == "python" || p.pythonExe == "python.exe")
                missing.push_back("Python (full path to the python.exe that has numpy and scipy, e.g. your conda env; a bare 'python' is usually the Microsoft Store stub)");
            else if (pathLike && !fs::exists(p.pythonExe))
                missing.push_back("Python (not found: " + p.pythonExe + ")");
        }
        if (!missing.empty()) {
            std::string m = "Calibration needs:";
            for (const auto& s : missing) m += "\n  - " + s;
            setError(m);
            return false;
        }
    }
    const Config cfg = toConfig(p);
    const std::string filterDir = cfg.requireString("filterDir");
    const int targetId = cfg.requireInt("targetId");
    const int templateLength = cfg.getInt("templateLength", 61);

    if (p.calibrationBackend == "python") {
        const std::string script = resolveRelativeToExe(cfg.getString("calibrationScript"));
        std::ostringstream cmd;
        cmd << "\"" << (p.pythonExe.empty() ? "python" : p.pythonExe) << "\" -u \"" << script << "\""
            << " --ks-dir \"" << cfg.requireString("trainingKsDir") << "\""
            << " --bin-path \"" << cfg.requireString("trainingBinPath") << "\""
            << " --target " << targetId;
        if (p.carShank != "-1") cmd << " --car-shank " << p.carShank;
        cmd << " --nearest-chans " << p.nearestChans << " --n-channels " << p.filterNChannels
            << " --template-length " << p.templateLength << " --template-offset " << p.templateOffset
            << " --train-frac " << fmtG(p.calibrationTrainFrac);
        if (p.calibrationMaxMinutes > 0) cmd << " --max-minutes " << fmtG(p.calibrationMaxMinutes);
        cmd << " --ridge " << fmtG(p.calibrationRidge) << " --max-spikes " << p.calibrationMaxSpikes
            << " --fc " << fmtG(p.highpassCutoffHz) << " --out-dir \"" << filterDir << "\""
            << " --seed " << p.calibrationSeed;

        ClLog() << "Running: " << cmd.str();
        const int rc = runProcessCapture(cmd.str());
        if (stopRequested_.load()) {
            ClLog() << "Calibration stopped.";
            return false;
        }
        if (rc == -1) {
            setError("could not launch '" + p.pythonExe + "' (set the Python executable that has numpy/scipy)");
            return false;
        }
        if (rc != 0) {
            setError("'" + script + "' exited with code " + std::to_string(rc));
            return false;
        }
        const FilterBank fb = FilterBank::load(filterDir, targetId, templateLength);
        ClLog() << "Calibration done: threshold=" << fb.threshold << " (" << fb.nChannels() << " channels)";
        return true;
    }

    if (p.calibrationBackend != "cpp") {
        setError("calibrationBackend must be 'python' or 'cpp'");
        return false;
    }
    // The C++ backend only re-sweeps the threshold; it starts from the saved channels/taps.
    ClLog() << "Running C++ calibration against training data (cannot be interrupted)...";
    FilterBank fb = FilterBank::load(filterDir, targetId, templateLength);
    const std::vector<int> car = resolveCarChannels(cfg, filterDir, targetId);
    const Calibration::Result r = Calibration::run(
        cfg.requireString("trainingBinPath"), cfg.requireString("trainingKsDir"), targetId, fb, car,
        p.applyHighpass, p.highpassCutoffHz, p.fetchChunkMs, p.calibrationCriterion, p.maxFalsePositiveRateHz,
        p.calibrationLogPath);
    fb.threshold = r.bestThreshold;
    fb.saveThreshold(filterDir);
    ClLog() << "Calibration done: threshold=" << r.bestThreshold << "  recall=" << r.bestPoint.recall
            << "  precision=" << r.bestPoint.precision << "  f1=" << r.bestPoint.f1
            << "  fpRateHz=" << r.bestPoint.fpRateHz;
    return true;
}

void ClosedLoopRunner::calibrationJob() {
    try {
        ClLog() << "=== Calibration ===";
        if (runCalibration(*job_)) calibrationDone_.store(true);
    } catch (const std::exception& e) {
        setError(e.what());
    }
    state_.store(State::Idle);
}

// ---------------------------------------------------------------------------------------------
// Live

void ClosedLoopRunner::liveJob() {
    void* hIM = nullptr;
    void* hNI = nullptr;
    void* hDO = nullptr;
    auto closeHandles = [&]() {
        // Sequential, like creation (the SDK's handle table isn't thread-safe).
        for (void** h : {&hIM, &hNI, &hDO}) {
            if (*h) {
                sglx_close(*h);
                sglx_destroyHandle(*h);
                *h = nullptr;
            }
        }
    };

    try {
        const ClParams p = *job_;
        const Config cfg = toConfig(p);
        impl_->live = false;

        // ---- Validate everything before touching hardware (same checks as ClosedLoop.exe) ----
        const std::string filterDir = cfg.requireString("filterDir");
        const int targetId = cfg.requireInt("targetId");
        const int templateLength = cfg.getInt("templateLength", 61);
        if (filterDir.empty()) throw std::runtime_error("filterDir is not set");

        const std::vector<int> syllableLines = cfg.getIntList("niSyllableLines");
        if (syllableLines.empty()) throw std::runtime_error("niSyllableLines is empty");
        const int maxCode = (1 << syllableLines.size()) - 1;
        const int targetSyllable = p.targetSyllable;
        if (targetSyllable != -1 && (targetSyllable < 1 || targetSyllable > maxCode))
            throw std::runtime_error("targetSyllable must be -1 (any) or 1.." + std::to_string(maxCode));

        DecisionThread::Mode mode;
        if (p.decisionMode == "push_down")
            mode = DecisionThread::Mode::PushDown;
        else if (p.decisionMode == "push_up")
            mode = DecisionThread::Mode::PushUp;
        else
            throw std::runtime_error("decisionMode must be 'push_down' or 'push_up', got '" + p.decisionMode + "'");
        if (p.adaptiveRenditions < 0) throw std::runtime_error("adaptiveRenditions must be >= 0 (0 = fixed threshold)");

        const ClWindow w = clWindow(p);
        if (p.subWinDurMs <= 0) throw std::runtime_error("subWinDurMs must be > 0");
        if (!w.valid) throw std::runtime_error("subWinOverlapMs must be >= 0 and < subWinDurMs, numSubWin >= 1");
        if (p.niDebounceSamples < 1) throw std::runtime_error("niDebounceSamples must be >= 1");
        if (p.niRearmMs <= 0 || p.niRefractoryMs < 0) throw std::runtime_error("niRearmMs must be > 0 and niRefractoryMs >= 0");
        if (p.doLine.empty()) throw std::runtime_error("doLine is not set");
        if (p.fetchChunkMs < 1) throw std::runtime_error("fetchChunkMs must be >= 1");

        ClLog() << "=== Live ===";
        ClLog() << "Target syllable: " << (targetSyllable == -1 ? std::string("any code") : "code " + std::to_string(targetSyllable));
        ClLog() << "Decision mode: " << p.decisionMode << ", threshold " << p.spikeCountThreshold
                << (p.adaptiveRenditions > 0
                        ? " for the first " + std::to_string(p.adaptiveRenditions) + " renditions, then the median of the last " +
                              std::to_string(p.adaptiveRenditions)
                        : std::string(" (fixed)"));
        {
            std::ostringstream s;
            s << "Decision window: centre " << p.windowCenterMs << " ms, span " << w.spanMs << " ms (" << w.startMs
              << " to " << w.endMs << " ms after onset); sub-windows:";
            for (int k = 0; k < p.numSubWin; ++k)
                s << " [" << w.startMs + k * w.stepMs << ", " << w.startMs + k * w.stepMs + p.subWinDurMs << "]";
            ClLog() << s.str() << " ms";
            if (w.startMs < 0) ClLog() << "Note: the window starts before syllable onset -- spikes up to " << -w.startMs << " ms before it count";
        }

        // ---- Calibration, if asked ----
        if (jobCalibrateFirst_) {
            ClLog() << "=== Calibration ===";
            if (!runCalibration(p)) {
                if (!stopRequested_.load()) throw std::runtime_error("calibration failed -- not going live");
                state_.store(State::Idle);
                return;
            }
            calibrationDone_.store(true);
        } else {
            ClLog() << "Using the saved filter (no calibration)";
        }
        if (stopRequested_.load()) {
            state_.store(State::Idle);
            return;
        }

        state_.store(State::Connecting);
        FilterBank filterBank = FilterBank::load(filterDir, targetId, templateLength);
        ClLog() << "Filter for target " << targetId << ": " << filterBank.nChannels() << " channels, threshold=" << filterBank.threshold;
        const std::vector<int> carChannelIds = resolveCarChannels(cfg, filterDir, targetId);

        // ---- Connect: three handles, created and connected sequentially, one owner thread each ----
        hIM = sglx_createHandle_std();
        hNI = sglx_createHandle_std();
        hDO = sglx_createHandle_std();
        const char* names[3] = {"IMEC", "NI", "DO"};
        void* hs[3] = {hIM, hNI, hDO};
        for (int i = 0; i < 3; ++i)
            if (!sglx_connect(hs[i], p.host.c_str(), p.port))
                throw std::runtime_error(std::string("Could not connect ") + names[i] + " handle to " + p.host + ":" +
                                         std::to_string(p.port) + ": " + sglx_getError(hs[i]));
        const std::string version = sglx_getVersion(hIM);
        ClLog() << "Connected 3 handles to " << p.host << ":" << p.port << " (SpikeGLX " << version << ")";

        bool acquiring = false;
        if (!sglx_isRunning(acquiring, hDO))
            throw std::runtime_error(std::string("Could not query SpikeGLX run state: ") + sglx_getError(hDO));
        if (!acquiring)
            throw std::runtime_error("SpikeGLX is connected but not acquiring -- start a run (or file replay) in SpikeGLX first");

        const double imecRate = sglx_getStreamSampleRate(hDO, 2, 0);
        if (imecRate <= 0) throw std::runtime_error(std::string("Could not get IMEC sample rate: ") + sglx_getError(hDO));

        // ---- Threads ----
        {
            std::lock_guard<std::mutex> lk(mu_);
            counters_ = ClCounters();
            counters_.imecRate = imecRate;
            counters_.sglxVersion = version;
            counters_.threshold = p.spikeCountThreshold;
            rows_.clear();
            rowsBase_ = 0;
        }
        LiveStats& stats = impl_->stats;
        stats.spikes = 0;
        stats.syllables = 0;
        stats.syllablesLost = 0;
        stats.imecSample = 0;

        ThreadSafeQueue<SpikeEvent> spikeQueue;
        ThreadSafeQueue<SyllableEvent> syllableQueue;
        std::atomic<long long> imecDecidedThrough(-1);

        ImecFetchThread imecThread(hIM, filterBank, carChannelIds, p.applyHighpass, p.highpassCutoffHz, p.fetchChunkMs,
                                   spikeQueue, imecDecidedThrough, p.spikeTimesPath);
        NiFetchThread niThread(hNI, syllableLines, p.niDebounceSamples, p.niRearmMs, p.niRefractoryMs, p.fetchChunkMs,
                               syllableQueue, p.syllableTimesPath);
        DecisionThread decisionThread(hDO, spikeQueue, syllableQueue, imecDecidedThrough, imecRate, targetSyllable,
                                      w.startMs / 1000.0, p.subWinDurMs / 1000.0, p.subWinOverlapMs / 1000.0, p.numSubWin,
                                      mode, p.spikeCountThreshold, p.adaptiveRenditions, p.doLine, p.doPulseMs,
                                      p.decisionLogPath);
        imecThread.setStats(&stats);
        niThread.setStats(&stats);
        imecThread.watchStream(p.streamTimeoutSec, stopRequested_, streamLost_);
        niThread.watchStream(p.streamTimeoutSec, stopRequested_, streamLost_);

        decisionThread.setObserver([this](const DecisionRecord& r) {
            std::lock_guard<std::mutex> lk(mu_);
            ClDecisionRow row;
            row.seq = static_cast<long long>(rowsBase_ + rows_.size());
            row.code = r.code;
            row.count = r.count;
            row.totalCount = r.totalCount;
            row.threshold = r.threshold;
            row.triggered = r.triggered;
            row.stalled = r.stalled;
            row.pulseMs = r.pulseMs;
            row.spikesMs = r.spikesMs;
            rows_.push_back(row);
            while (rows_.size() > kMaxRows) {
                rows_.pop_front();
                ++rowsBase_;
            }
            ++counters_.decisions;
            if (r.triggered) ++counters_.triggers;
            if (r.stalled) ++counters_.stalled;
            counters_.threshold = r.threshold;
        });

        ClLog() << "Starting fetch/decision threads";
        impl_->liveStart = Clock::now();
        impl_->live = true;
        imecThread.start();
        niThread.start();
        decisionThread.start();
        state_.store(State::Live);

        while (!stopRequested_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (p.runSeconds > 0 &&
                std::chrono::duration<double>(Clock::now() - impl_->liveStart).count() >= p.runSeconds) {
                ClLog() << "runSeconds reached";
                break;
            }
        }

        state_.store(State::Stopping);
        ClLog() << "Stopping...";
        imecThread.stop();
        niThread.stop();
        decisionThread.stop();
        imecThread.join();
        niThread.join();
        decisionThread.join();
        closeHandles();
        impl_->live = false;
        if (streamLost_.load()) setError("a SpikeGLX stream delivered no data for " + fmtG(p.streamTimeoutSec) +
                                          " s (SpikeGLX stopped, closed or restarted its run)");
        else ClLog() << "Done.";
    } catch (const std::exception& e) {
        setError(e.what());
        closeHandles();
        impl_->live = false;
    }
    state_.store(State::Idle);
}

// ---------------------------------------------------------------------------------------------
// Pulse test

void ClosedLoopRunner::pulseJob() {
    void* h = nullptr;
    try {
        const ClParams p = *job_;
        if (p.doLine.empty()) throw std::runtime_error("doLine is not set");
        h = sglx_createHandle_std();
        if (!sglx_connect(h, p.host.c_str(), p.port))
            throw std::runtime_error(std::string("Could not connect: ") + sglx_getError(h));
        ClLog() << "Pulse test: " << p.doPulseMs << " ms on " << p.doLine << " (SpikeGLX " << sglx_getVersion(h) << ")";
        // All-ones bits: `lines` picks the line, and this sets it high whichever way SpikeGLX reads the pattern.
        const bool up = sglx_ni_DO_set(h, p.doLine, 0xFFFFFFFFu);
        if (!up) ClLog() << "DO high FAILED: " << sglx_getError(h);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, p.doPulseMs)));
        const bool down = sglx_ni_DO_set(h, p.doLine, 0x0);
        if (!down) ClLog() << "DO low FAILED: " << sglx_getError(h);
        if (up && down) ClLog() << "Pulse sent.";
    } catch (const std::exception& e) {
        setError(e.what());
    }
    if (h) {
        sglx_close(h);
        sglx_destroyHandle(h);
    }
    state_.store(State::Idle);
}

}  // namespace fp

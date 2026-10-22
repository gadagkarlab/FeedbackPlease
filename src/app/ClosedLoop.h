#pragma once
// Spike-triggered closed loop (the former stand-alone ClosedLoop.exe), run inside the GUI.
//
//   ClParams            every ClosedLoop config.txt key, editable in the "Closed loop" tab
//   ClosedLoopRunner    calibration (filter fit from training data) and the live loop:
//                       IMEC spike detection + NI syllable onsets from SpikeGLX -> spike count
//                       in a window after the syllable -> digital pulse
//
// The runner owns its own threads and SpikeGLX connections and shares nothing with the
// microphone Engine, so both can run at the same time. The syllable codes the Engine writes to
// its code port reach the runner through SpikeGLX's NI stream (wire the port to the lines in
// "NI syllable lines").

#include <atomic>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fp {

struct ClParams {
    // SpikeGLX connection
    std::string host = "localhost";
    int port = 4142;

    // Filter (channels_<id>.bin / filter_<id>.bin / threshold_<id>.bin live in filterDir)
    std::string filterDir;
    int targetId = 0;
    int templateLength = 61;

    // Preprocessing; must match how the filter was trained
    std::string carShank = "auto";  // "auto", "-1" (CAR off) or a 0-based shank index
    bool applyHighpass = true;
    double highpassCutoffHz = 300.0;

    // Calibration
    bool skipCalibration = false;  // Start live without refitting the saved filter
    std::string calibrationBackend = "python";  // "python" or "cpp"
    std::string trainingBinPath;
    std::string trainingKsDir;
    std::string pythonExe = "C:/Users/ksnpx/anaconda3/envs/closedloop/python.exe";
    std::string calibrationScript;
    int nearestChans = 10;
    int filterNChannels = 5;
    int templateOffset = 20;
    double calibrationTrainFrac = 0.5;
    double calibrationMaxMinutes = 30.0;  // 0 = whole recording
    double calibrationRidge = 1e-3;
    int calibrationMaxSpikes = 2000;
    int calibrationSeed = 0;
    // cpp backend only
    std::string calibrationLogPath;
    std::string calibrationCriterion = "best_f1";  // or best_recall_under_fp_rate
    double maxFalsePositiveRateHz = 1.0;

    // Fetch cadence
    int fetchChunkMs = 5;
    double streamTimeoutSec = 5.0;

    // NI digital line conventions
    std::string niSyllableLines = "5,6,7";
    int niDebounceSamples = 2;
    double niRearmMs = 1.0;
    double niRefractoryMs = 10.0;

    // Decision logic
    int targetSyllable = -1;  // -1 = any code
    double windowCenterMs = 57.0;
    double subWinDurMs = 20.0;
    double subWinOverlapMs = 2.0;
    int numSubWin = 4;
    std::string decisionMode = "push_down";  // or push_up
    double spikeCountThreshold = 3.0;
    int adaptiveRenditions = 0;
    std::string doLine = "Dev4/port0/line5";
    int doPulseMs = 50;

    // Output (blank = don't write)
    std::string spikeTimesPath;
    std::string syllableTimesPath;
    std::string decisionLogPath;
    double runSeconds = 0.0;  // 0 = until Stop
};

// Geometry of the decision window, in ms after syllable onset.
struct ClWindow {
    double startMs = 0, stepMs = 0, spanMs = 0, endMs = 0;
    bool valid = false;
};
ClWindow clWindow(const ClParams& p);

// The calibration script used when ClParams::calibrationScript is empty.
std::string clDefaultCalibrationScript();

// ClosedLoop's own "key=value" config.txt format, so existing config files load as they are.
bool loadClParams(const std::string& path, ClParams& p, std::string* err = nullptr);
bool saveClParams(const std::string& path, const ClParams& p, std::string* err = nullptr);

// What is currently saved in filterDir for the target (what Start live without calibration uses).
struct ClFilterInfo {
    bool ok = false;
    std::string text;  // channels / threshold / date, or why it can't be loaded
    int nChannels = 0;
    double threshold = 0;
};
ClFilterInfo inspectClFilter(const ClParams& p);

// One finished decision window (one syllable rendition).
struct ClDecisionRow {
    long long seq = 0;
    int code = 0;
    int count = 0;       // busiest pair of neighbouring sub-windows
    int totalCount = 0;  // whole window
    double threshold = 0;
    bool triggered = false;
    bool stalled = false;
    double pulseMs = NAN;  // onset -> pulse
    std::vector<double> spikesMs;  // spike times relative to onset, sorted
};

struct ClCounters {
    long long spikes = 0, syllables = 0, syllablesLost = 0, imecSample = 0;
    long long decisions = 0, triggers = 0, stalled = 0;
    double imecRate = 0;
    double elapsedSec = 0;  // since the live loop started
    double threshold = 0;   // spike count threshold in effect for the latest rendition
    std::string sglxVersion;
};

class ClosedLoopRunner {
public:
    enum class State { Idle, Calibrating, Connecting, Live, Stopping };

    ClosedLoopRunner();
    ~ClosedLoopRunner();

    // Each returns false (with err) if something is already running or the settings are unusable.
    bool startCalibration(const ClParams& p, std::string& err);
    // calibrateFirst: refit the filter from the training data, then go live.
    bool startLive(const ClParams& p, bool calibrateFirst, std::string& err);
    // Raise doLine for doPulseMs once, to check the wiring.
    bool startPulseTest(const ClParams& p, std::string& err);
    // Asks whatever is running to finish; returns immediately. A Python calibration is killed.
    void stop();

    State state() const { return state_.load(); }
    bool busy() const { return state() != State::Idle; }
    static const char* stateName(State s);

    ClCounters counters() const;
    // Lines / rows added since index `from`; returns the new end index to pass next time.
    size_t fetchLog(size_t from, std::vector<std::string>& out) const;
    size_t fetchDecisions(size_t from, std::vector<ClDecisionRow>& out) const;

    // Set when the last job ended with an error (cleared when the next starts).
    std::string lastError() const;
    // Calibration finished OK since the last time this was read; lets the GUI refresh its filter info.
    bool takeCalibrationDone() { return calibrationDone_.exchange(false); }

private:
    void spawn(std::string& err, void (ClosedLoopRunner::*job)());
    void calibrationJob();
    void liveJob();
    void pulseJob();

    bool runCalibration(const ClParams& p);
    int runProcessCapture(const std::string& commandLine);
    void log(const std::string& line);
    void setError(const std::string& e);

    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> streamLost_{false};
    std::atomic<bool> calibrationDone_{false};
    std::thread worker_;

    std::unique_ptr<ClParams> job_;  // settings of the running job (copied at start)
    bool jobCalibrateFirst_ = false;

    mutable std::mutex mu_;
    std::deque<std::string> log_;
    size_t logBase_ = 0;  // index of log_.front()
    std::deque<ClDecisionRow> rows_;
    size_t rowsBase_ = 0;
    ClCounters counters_;  // the non-atomic parts
    std::string error_;

    struct Impl;
    std::unique_ptr<Impl> impl_;  // LiveStats, child process handle, start time
};

}  // namespace fp

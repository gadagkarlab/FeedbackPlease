#pragma once
// One recording/feedback box: detection, decisions, outputs and everything the GUI
// shows about it. The acquisition thread that owns the box's frames (see Engine)
// calls process() once per frame; the GUI talks to it through the thread-safe
// methods. This is the former body of the single-box Engine::run loop.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Detector.h"
#include "core/Params.h"
#include "core/SongRecorder.h"
#include "core/WavFile.h"
#include "io/IOBackend.h"

namespace fp {

struct EngineStats {
    bool running = false;
    std::string backend;
    double sampleRate = 0.0;
    int frameSamples = 0;
    int windowSamples = 0;
    double elapsedSec = 0.0;
    uint64_t frames = 0;
    double backlogMs = 0.0;     // acquired but not yet processed
    double maxBacklogMs = 0.0;
    double procUsAvg = 0.0;     // detector + outputs per frame
    double procUsMax = 0.0;
    double lastIoiMs = 0.0, lastSdMs = 0.0;
    double value = 0.0, displayValue = 0.0;
    bool inSyllable = false;
    bool songActive = false;
    double songFraction = 0.0, songRatio = 0.0;
    int songFftSize = 0;
    double songBinHz = 0.0;
    FbState fbState = FbState::Listening;
    uint64_t onsets = 0, matches = 0, blocked = 0, trials = 0, fires1 = 0, fires2 = 0, suppressed = 0, catches = 0;
    uint64_t songBouts = 0;
    uint64_t pulseTrigs = 0, pulseFires = 0;  // closed-loop pulses seen / answered with the burst
    bool pulseInput = false;                  // pulse input line is high now
    int songFilesDone = 0;      // song-triggered WAVs finished this session
    bool songFileOpen = false;  // a song-triggered WAV is being written now
    double recQueueSec = 0.0;    // audio waiting to be written
    double recDroppedSec = 0.0;  // audio dropped because writing fell too far behind
    int currentCode = 0;               // value on the syllable code port now
    std::vector<uint64_t> codeCounts;   // per entry of the syllable code list ("All Sylls Count")
    uint64_t codeUnmatched = 0;         // IOI ends where no entry matched
    std::string sessionPrefix;
};

struct LogEvent {
    double tMs = 0.0;
    std::string type;
    std::string detail;
};

struct ScopeSnapshot {
    double tEndSec = 0.0;  // time of the newest sample
    double rawRate = 0.0;
    std::vector<float> raw;  // newest last
    double envRate = 0.0;    // frames per second
    std::vector<float> env;  // display units
    std::vector<float> syl;  // 0/1 per frame
    std::vector<float> songFrac;   // per frame: fraction of kernel above ratio threshold
    std::vector<float> songRatio;  // per frame: latest power ratio
    std::vector<float> songOn;     // per frame: 0/1 song active
    std::vector<float> tmplScore;  // per frame: correlation with the target template (template method)
    std::vector<double> matchT, fireT, suppressT, blockedT;  // seconds, same clock as tEndSec
};

// Writes song-triggered WAVs, the envelope WAV and the events CSV from a background
// thread so disk I/O never stalls the acquisition loop. The box only queues data.
class AsyncRecorder {
public:
    ~AsyncRecorder() { close(); }
    // Empty envPath / eventsPath disable those outputs; songs == nullptr disables
    // song-triggered files.
    bool open(const std::string& envPath, double envRate, const std::string& eventsPath,
              const SongFileConfig* songs, std::string* err);
    // One frame for the song files: mic samples + the frame's detection value, marker
    // level and song state.
    void pushFrame(const float* mic, int n, float detection, float marker, bool song);
    void pushEnv(float v);
    void pushEventLine(std::string line);  // one CSV row, without the newline
    bool eventsOpen() const { return eventsOn_; }
    int songFilesDone() const { return songs_.filesDone(); }
    bool songRecording() const { return songs_.recording(); }
    // Audio waiting for the writer thread, and audio dropped because the writer fell more
    // than kMaxQueueSec behind (instead of letting the queue eat all memory), in seconds.
    double queuedSec();
    double droppedSec() const { return songFs_ > 0 ? double(dropped_.load()) / songFs_ : 0.0; }
    static constexpr double kMaxQueueSec = 30.0;
    void close();

private:
    struct FrameMeta {
        float detection, marker;
        bool song;
    };
    void run();
    WavWriter env_;
    SongFileWriter songs_;  // used only by the writer thread once it is running
    bool songsOn_ = false;  // set before the thread starts
    bool envOn_ = false;    // set before the thread starts
    int frameN_ = 0;        // samples per frame (constant for a run)
    std::ofstream events_;  // used only by the writer thread once it is running
    bool eventsOn_ = false; // set before the thread starts; safe to read from the engine thread
    std::mutex mx_;
    std::condition_variable cv_;
    std::vector<float> pendMic_, pendEnv_;
    std::vector<FrameMeta> pendMeta_;
    std::vector<std::string> pendLines_;
    uint64_t pendSkip_ = 0;           // samples dropped since the writer last took the queue
    size_t maxMic_ = 0, maxEnv_ = 0;  // queue limits: kMaxQueueSec of audio / envelope
    double songFs_ = 0.0;
    std::atomic<uint64_t> dropped_{0};  // samples dropped this session
    bool stop_ = false;
    std::thread th_;
};

class Box {
public:
    Box() = default;
    ~Box() { end(); }
    Box(const Box&) = delete;
    Box& operator=(const Box&) = delete;

    // ---- owner side (Engine)
    // p already has the actual sample rate and frame size. Recordings go to
    // <outputDir>/<bird>/<YYYYMMDD>/ (see openSession).
    bool begin(const AppParams& p, const std::string& backend, std::string& err);
    // One frame of n samples, on the acquisition thread. Outputs go to io.
    void process(const float* x, int n, IOBackend& io, size_t backlogSamples);
    void finish(IOBackend& io);  // acquisition thread, after the last frame: outputs low
    void end();                  // after the thread has exited: close files
    void setError(const std::string& e);

    // ---- GUI side (thread-safe)
    // Live parameter changes (spectrum, thresholds, syllables, feedback). Hardware
    // settings only take effect on the next start.
    void updateParams(const AppParams& p);
    void manualFire(int which) { manualFire_.store(which); }  // 1, 2 = waves, 3 = pulse burst
    void resetCounters() { resetCounters_.store(true); }
    EngineStats stats() const;
    void snapshot(double seconds, ScopeSnapshot& out) const;
    std::vector<LogEvent> events(size_t max) const;
    std::string error() const;

private:
    template <class T>
    struct Ring {
        std::vector<T> d;
        size_t head = 0, count = 0;
        void reset(size_t cap) { d.assign(cap, T{}); head = count = 0; }
        void push(T v) {
            d[head] = v;
            head = (head + 1) % d.size();
            if (count < d.size()) ++count;
        }
        void copyLast(size_t n, std::vector<T>& out) const {
            n = std::min(n, count);
            out.resize(n);
            size_t start = (head + d.size() - n) % d.size();
            for (size_t i = 0; i < n; ++i) out[i] = d[(start + i) % d.size()];
        }
    };

    void logEvent(double tMs, const char* type, const std::string& detail, double value);
    bool openSession(const AppParams& p, std::string& err);

    // Parameters handed from the GUI to the acquisition thread.
    mutable std::mutex paramMx_;
    AppParams pending_;
    std::atomic<uint64_t> paramVersion_{0};
    std::atomic<int> manualFire_{0};
    std::atomic<bool> resetCounters_{false};
    AppParams startParams_;  // as given to begin(), with the actual sample rate

    // Acquisition-thread state.
    std::unique_ptr<Detector> det_;
    AppParams p_;
    uint64_t seenVersion_ = 0;
    double fs_ = 0.0, frameMs_ = 0.0;
    DigitalOutputs dout_;
    double fbPulseLeftMs_ = 0.0, matchPulseLeftMs_ = 0.0;
    double procAvg_ = 0.0, procMax_ = 0.0, maxBacklog_ = 0.0;
    uint64_t frames_ = 0;
    uint64_t onsets_ = 0, matches_ = 0, blocked_ = 0, trials_ = 0, fires1_ = 0, fires2_ = 0, suppressed_ = 0,
             catches_ = 0;
    uint64_t pulseTrigs_ = 0, pulseFires_ = 0;
    bool pulsePrev_ = false;
    double pulseLastMs_ = -1e12;
    uint64_t songBouts_ = 0;
    std::vector<uint64_t> codeCounts_;
    double loggedDropSec_ = 0.0, lastDropLogMs_ = -1e18;  // recording-drop messages
    std::vector<int> countedCodes_;  // codes of the list the counts belong to
    uint64_t codeUnmatched_ = 0;
    int codeOut_ = 0;  // value currently on the syllable code port
    double codePulseLeftMs_ = 0.0;
    bool songWasActive_ = false;
    double lastIoi_ = 0.0, lastSd_ = 0.0;
    AsyncRecorder recorder_;
    bool begun_ = false;

    // Shared with the GUI.
    mutable std::mutex mx_;
    EngineStats stats_;
    Ring<float> raw_, env_, syl_, songFrac_, songRatio_, songOn_, tmplScore_;
    std::deque<double> matchT_, fireT_, suppressT_, blockedT_;
    std::deque<LogEvent> events_;
    std::string error_;
};

}  // namespace fp

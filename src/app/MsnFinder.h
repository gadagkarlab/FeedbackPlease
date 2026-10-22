#pragma once
// MSN finder back end: runs tools/msn_finder/msn_server.py (Python, numpy/scipy) as a child process and
// talks to it over pipes. The server reads a CatGT + Kilosort run, ranks units by the variance of their
// spike-time histogram for one syllable, and returns the numbers behind each unit's figure. Nothing is
// rendered or written to disk by either side.
//
// All requests return immediately; results are published as immutable snapshots that the GUI thread can
// grab at any time. Requests are processed by the server in order.

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fp {

struct MsnResponse;

struct MsnParams {
    // Python
    std::string pythonExe = "C:/Users/ksnpx/anaconda3/envs/closedloop/python.exe";
    std::string script;  // empty = default (tools/msn_finder/msn_server.py)
    // Data
    std::string runDir;  // CatGT output, e.g. X:/Choc57_out/catgt_Choc57_d2.0_g0
    std::string ksName = "imec0_ks4";
    std::string cacheDir;  // where decoded syllable labels / sound pulses are cached (empty = next to the script)
    bool goodOnly = true;
    double maxRateHz = 50.0;  // <= 0: no cutoff
    // Ranking
    int label = 1;
    int nbins = 20;
    int minSpikes = 10;
    int nUnits = 20;  // lowest-variance units to list; 0 = all
    // Figure
    double pad = 0.5;    // raster margin, fraction of median syllable duration
    double winMs = 20;   // counting window centred on the unit's histogram peak
    int roll = 10;       // renditions per rolling-mean step
};

std::string msnDefaultScript();

struct MsnSession {
    std::string name;
    double durationSec = 0;
    int nUnits = 0;
    std::vector<int> labels, labelCounts;
};

struct MsnRankRow {
    int unit = 0;
    double variance = 0, nSpikes = 0, peakMs = 0;
};

struct MsnRanking {
    int label = 0;
    int nRenditions = 0;
    double medianMs = 0;
    std::vector<MsnRankRow> rows;  // increasing variance
};

// Mean spectrogram of one syllable label, [freq][time] row-major, frequency ascending.
struct MsnSpec {
    int label = 0;
    std::vector<float> freqHz, timeFrac, db;
};

struct MsnFigure {
    int label = 0, unit = 0, nRenditions = 0;
    double medianMs = 0, peakMs = 0, peakYum = 0;
    int peakChannel = 0, peakShank = 0;
    std::vector<float> endMs;           // syllable duration per rendition
    std::vector<float> spikeMs;         // spike times relative to onset (unwarped)
    std::vector<int> spikeRendition;    // rendition index of each spike
    std::vector<float> wfT, wf;         // template waveform
    std::vector<float> fpXY;            // channel positions, x0 y0 x1 y1 ...
    std::vector<int> fpShank;
    std::vector<float> fpAmp;           // template peak-to-peak per channel
};

class MsnBackend {
public:
    MsnBackend();
    ~MsnBackend();

    // Each starts the Python process if needed. false + err if it can't be started or sent.
    bool requestLoad(const MsnParams& p, std::string& err);
    bool requestRank(int label, int nbins, int minSpikes, int nUnits, std::string& err);
    bool requestFigure(int label, int unit, double pad, std::string& err);
    void restart();  // kill the server (e.g. a request is taking too long); the next request starts a new one

    int pending() const;  // requests sent and not yet answered
    bool busy() const { return pending() > 0; }
    std::string lastError() const;
    void clearError();

    std::shared_ptr<const MsnSession> session() const;
    std::shared_ptr<const MsnRanking> ranking() const;
    std::shared_ptr<const MsnSpec> spec() const;
    std::shared_ptr<const MsnFigure> figure() const;
    // Monotonic counters so the view can tell when a snapshot changed.
    uint64_t sessionVersion() const { return sessionVer_.load(); }
    uint64_t rankingVersion() const { return rankingVer_.load(); }
    uint64_t figureVersion() const { return figureVer_.load(); }

    size_t fetchLog(size_t from, std::vector<std::string>& out) const;

private:
    struct Impl;
    struct Pending {
        enum Kind { Load, Rank, Fig } kind;
        int label = 0, unit = 0;
    };

    bool ensureProcess(const MsnParams& p, std::string& err);
    bool send(const std::string& line, Pending pend, std::string& err);
    void stdoutLoop(std::shared_ptr<Impl> impl);
    void stderrLoop(std::shared_ptr<Impl> impl);
    void finish(const MsnResponse& r, bool ok, const std::string& msg);
    void log(const std::string& line);
    void setError(const std::string& e);
    void kill();

    mutable std::mutex mu_;
    std::shared_ptr<Impl> impl_;
    std::thread outThread_, errThread_;
    std::deque<Pending> pending_;
    std::string error_;
    std::deque<std::string> log_;
    size_t logBase_ = 0;
    std::shared_ptr<const MsnSession> session_;
    std::shared_ptr<const MsnRanking> ranking_;
    std::shared_ptr<const MsnSpec> spec_;
    std::shared_ptr<const MsnFigure> figure_;
    std::atomic<uint64_t> sessionVer_{0}, rankingVer_{0}, figureVer_{0};
    std::string launchedWith_;  // python + script of the running server
    bool specSent_ = false;
    int specLabel_ = -1;
};

}  // namespace fp

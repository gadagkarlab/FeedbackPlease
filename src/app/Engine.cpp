#include "app/Engine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fp {

namespace {
constexpr double kScopeSeconds = 10.0;  // how much history the GUI can show
constexpr size_t kMaxEvents = 1000;

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

std::string fmt(const char* f, double a, double b = 0, double c = 0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}
}  // namespace

// ---------------------------------------------------------------- AsyncRecorder

bool AsyncRecorder::open(const std::string& envPath, double envRate, const std::string& eventsPath,
                         const SongFileConfig* songs, std::string* err) {
    close();
    bool ok = true;
    if (!envPath.empty() && !env_.open(envPath, envRate)) {
        if (err) *err = "cannot write " + envPath;
        ok = false;
    }
    if (ok && !eventsPath.empty()) {
        events_.open(std::filesystem::u8path(eventsPath));
        if (!events_.is_open()) {
            if (err) *err = "cannot write " + eventsPath;
            ok = false;
        }
    }
    if (ok && songs) ok = songs_.open(*songs, err);
    if (!ok) {
        close();  // don't leave half the files open
        return false;
    }
    if (events_.is_open()) events_ << "sample,time_ms,event,value,detail\n";
    eventsOn_ = events_.is_open();
    songsOn_ = songs != nullptr;
    frameN_ = 0;
    if (!env_.isOpen() && !eventsOn_ && !songsOn_) return true;
    stop_ = false;
    th_ = std::thread(&AsyncRecorder::run, this);
    return true;
}

void AsyncRecorder::pushEventLine(std::string line) {
    if (!eventsOn_) return;
    std::lock_guard<std::mutex> lk(mx_);
    pendLines_.push_back(std::move(line));
}

void AsyncRecorder::pushFrame(const float* mic, int n, float detection, float marker, bool song) {
    if (!songsOn_) return;
    std::lock_guard<std::mutex> lk(mx_);
    frameN_ = n;  // constant for a run
    pendMic_.insert(pendMic_.end(), mic, mic + n);
    pendMeta_.push_back({detection, marker, song});
}

void AsyncRecorder::pushEnv(float v) {
    if (!env_.isOpen()) return;
    std::lock_guard<std::mutex> lk(mx_);
    pendEnv_.push_back(v);
}

void AsyncRecorder::run() {
    std::vector<float> mic, e;
    std::vector<FrameMeta> meta;
    std::vector<std::string> lines;
    for (;;) {
        bool done;
        int n;
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return stop_; });
            // Swapping hands the (cleared) buffers back, so steady state has no reallocations.
            mic.swap(pendMic_);
            meta.swap(pendMeta_);
            e.swap(pendEnv_);
            lines.swap(pendLines_);
            n = frameN_;
            done = stop_;
        }
        // Song files: pre-roll ring + start/extend/end decisions, frame by frame.
        for (size_t f = 0; f < meta.size() && n > 0; ++f)
            songs_.push(&mic[f * size_t(n)], n, meta[f].detection, meta[f].marker, meta[f].song);
        env_.write(e.data(), e.size());
        for (const auto& l : lines) events_ << l << '\n';
        if (!lines.empty()) events_.flush();  // a crash loses at most ~100 ms of events
        mic.clear();
        meta.clear();
        e.clear();
        lines.clear();
        if (done) break;
    }
}

void AsyncRecorder::close() {
    if (th_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(mx_);
            stop_ = true;
        }
        cv_.notify_all();
        th_.join();
    }
    songs_.close();  // finishes a song file in progress (its WAV header and index row)
    env_.close();
    if (events_.is_open()) events_.close();
    eventsOn_ = songsOn_ = false;
    pendMic_.clear();
    pendMeta_.clear();
    pendEnv_.clear();
    pendLines_.clear();
}

// ---------------------------------------------------------------- Engine

bool Engine::start(const AppParams& params, std::string& err) {
    stop();
    {
        std::lock_guard<std::mutex> lk(mx_);
        error_.clear();
    }

    io_ = params.source.kind == SourceKind::Daq ? makeDaqBackend() : makeSimBackend();
    if (!io_->start(params, err)) {
        io_.reset();
        return false;
    }

    AppParams p = params;
    const double fs = io_->sampleRate();
    // Keep the frame duration when a simulated WAV has a different rate than the settings.
    const int frameN = std::max(1, int(std::lround(p.hw.samplesPerFrame * fs / p.hw.sampleRate)));
    p.hw.sampleRate = fs;
    p.hw.samplesPerFrame = frameN;
    startParams_ = p;
    {
        std::lock_guard<std::mutex> lk(paramMx_);
        pending_ = p;
    }

    if (!openSession(p, err)) {
        io_->stop();
        io_.reset();
        recorder_.close();
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mx_);
        const std::string prefix = stats_.sessionPrefix;
        stats_ = EngineStats{};
        stats_.sessionPrefix = prefix;
        stats_.running = true;
        stats_.backend = io_->name();
        stats_.sampleRate = fs;
        stats_.frameSamples = frameN;
        raw_.reset(size_t(fs * kScopeSeconds));
        const double frameRate = fs / frameN;
        for (auto* r : {&env_, &syl_, &songFrac_, &songRatio_, &songOn_}) r->reset(size_t(frameRate * kScopeSeconds));
        matchT_.clear();
        fireT_.clear();
        suppressT_.clear();
        blockedT_.clear();
        events_.clear();
    }

    stopRequested_ = false;
    running_ = true;
    thread_ = std::thread(&Engine::run, this);
    return true;
}

void Engine::stop() {
    stopRequested_ = true;
    if (thread_.joinable()) thread_.join();
    if (io_) {
        io_->stop();
        io_.reset();
    }
    running_ = false;
    recorder_.close();
    std::lock_guard<std::mutex> lk(mx_);
    stats_.running = false;
}

bool Engine::openSession(const AppParams& p, std::string& err) {
    std::string prefix;
    const auto& rec = p.rec;
    const bool songFiles = rec.songFiles && (rec.chMic || rec.chDetection || rec.chMarkers);
    if (rec.saveEvents || songFiles || rec.saveEnvelope) {
        std::error_code ec;
        std::filesystem::create_directories(rec.outputDir, ec);
        if (ec) {
            err = "cannot create " + rec.outputDir + ": " + ec.message();
            return false;
        }
        prefix = (std::filesystem::path(rec.outputDir) / ("session_" + timestamp())).string();
        saveParams(prefix + "_settings.ini", p);
        const double fs = p.hw.sampleRate;
        SongFileConfig songs;
        songs.prefix = prefix;
        songs.sampleRate = fs;
        songs.preRollSec = rec.preRollSec;
        songs.postRollSec = rec.postRollSec;
        songs.mic = rec.chMic;
        songs.detection = rec.chDetection;
        songs.markers = rec.chMarkers;
        std::string recErr;
        if (!recorder_.open(rec.saveEnvelope ? prefix + "_envelope.wav" : "", fs / p.hw.samplesPerFrame,
                            rec.saveEvents ? prefix + "_events.csv" : "", songFiles ? &songs : nullptr, &recErr)) {
            err = "cannot open recording files: " + recErr;
            return false;
        }
    }
    std::lock_guard<std::mutex> lk(mx_);
    stats_.sessionPrefix = prefix;
    return true;
}

void Engine::updateParams(const AppParams& p) {
    std::lock_guard<std::mutex> lk(paramMx_);
    pending_ = p;
    ++paramVersion_;
}

void Engine::logEvent(double tMs, const char* type, const std::string& detail, double value) {
    if (recorder_.eventsOpen()) {
        const uint64_t sample = uint64_t(std::llround(tMs * startParams_.hw.sampleRate / 1000.0));
        char head[96];
        std::snprintf(head, sizeof head, "%llu,%.3f,", (unsigned long long)sample, tMs);
        char val[32];
        std::snprintf(val, sizeof val, ",%.6g,", value);
        recorder_.pushEventLine(head + std::string(type) + val + detail);
    }
    std::lock_guard<std::mutex> lk(mx_);
    events_.push_back({tMs, type, detail});
    if (events_.size() > kMaxEvents) events_.pop_front();
}

void Engine::run() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    using clock = std::chrono::steady_clock;

    const double fs = startParams_.hw.sampleRate;
    const int n = startParams_.hw.samplesPerFrame;
    const double frameMs = n * 1000.0 / fs;

    Detector det(uint32_t(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    AppParams p = startParams_;
    det.setParams(p);
    uint64_t seenVersion = paramVersion_.load();

    std::vector<float> frame(size_t(n), 0.0f);
    DigitalOutputs dout;
    double fbPulseLeftMs = 0.0, matchPulseLeftMs = 0.0;
    double procAvg = 0.0, procMax = 0.0, maxBacklog = 0.0;
    uint64_t frames = 0;
    uint64_t onsets = 0, matches = 0, blocked = 0, trials = 0, fires1 = 0, fires2 = 0, suppressed = 0, catches = 0;
    uint64_t pulseTrigs = 0, pulseFires = 0;  // closed-loop pulses seen / answered with the noise burst
    bool pulsePrev = false;                   // pulse input level last frame (for edge detection)
    double pulseLastMs = -1e12;
    uint64_t songBouts = 0;
    std::vector<uint64_t> codeCounts;
    std::vector<int> countedCodes;  // codes of the list the counts belong to
    uint64_t codeUnmatched = 0;
    int codeOut = 0;            // value currently on the syllable code port
    double codePulseLeftMs = 0.0;
    bool songWasActive = false;
    double lastIoi = 0.0, lastSd = 0.0;
    std::string err;

    while (!stopRequested_) {
        if (!io_->read(frame.data(), n, err)) {
            std::lock_guard<std::mutex> lk(mx_);
            error_ = err;
            break;
        }
        const auto t0 = clock::now();

        if (paramVersion_.load() != seenVersion) {
            std::lock_guard<std::mutex> lk(paramMx_);
            seenVersion = paramVersion_.load();
            AppParams np = pending_;
            np.hw = startParams_.hw;  // hardware is fixed for the run
            p = np;
            det.setParams(p);
        }
        if (resetCounters_.exchange(false)) {
            onsets = matches = blocked = trials = fires1 = fires2 = suppressed = catches = songBouts = 0;
            pulseTrigs = pulseFires = 0;
            std::fill(codeCounts.begin(), codeCounts.end(), 0);
            codeUnmatched = 0;
            procMax = maxBacklog = 0.0;
        }

        FrameResult r = det.process(frame.data(), n);

        // A manual test fire bypasses detection and the state machine.
        const int manual = manualFire_.exchange(0);
        if (manual == 1 || manual == 2) {
            r.fb.arm = 0;
            r.fb.fire = manual;
        }

        // Closed-loop pulse: a rising edge on the input line plays the noise burst (wave 3). A trigger that
        // arrives within the refractory period of the last one is counted but ignored. Manual test = 3.
        int pulseFire = manual == 3 ? 3 : 0;
        bool pulseEdge = false, pulseIgnored = false;
        const int pulseLevel = io_->pollPulseInput();
        if (pulseLevel >= 0) {
            pulseEdge = pulseLevel == 1 && !pulsePrev;
            pulsePrev = pulseLevel == 1;
        }
        if (pulseEdge) {
            ++pulseTrigs;
            if (p.pulse.enabled) {
                if (r.tMs - pulseLastMs >= p.pulse.refractoryMs) {
                    pulseFire = 3;
                    pulseLastMs = r.tMs;
                } else {
                    pulseIgnored = true;
                }
            }
        }

        // ---- outputs first, bookkeeping after
        if (r.fb.arm) io_->armWave(r.fb.arm);
        if (r.fb.fire) {
            io_->fireWave(r.fb.fire);
            fbPulseLeftMs = std::max(p.fb.ttlPulseMs, frameMs);
        }
        if (pulseFire) {
            io_->fireWave(3);
            fbPulseLeftMs = std::max(p.fb.ttlPulseMs, frameMs);
        }
        if (r.match.match) matchPulseLeftMs = std::max(p.fb.ttlPulseMs, frameMs);
        dout.feedback = fbPulseLeftMs > 0.0;
        dout.match = matchPulseLeftMs > 0.0;
        dout.syllable = r.seg.inSyllable;
        dout.song = r.song.active;
        io_->writeDigital(dout);

        // Syllable code port: set at each IOI end to the first matching entry's code
        // (0 if none), then cleared according to the hold mode.
        int codeNext = codeOut;
        if (r.sylCode.evaluated) {
            codeNext = r.sylCode.code;
            codePulseLeftMs = p.codes.pulseMs;
        } else if (p.codes.hold == SylCodeHold::UntilOffset && r.seg.offset) {
            codeNext = 0;
        } else if (p.codes.hold == SylCodeHold::Pulse) {
            codePulseLeftMs -= frameMs;
            if (codePulseLeftMs <= 0.0) codeNext = 0;
        }
        if (!p.codes.enabled) codeNext = 0;
        if (codeNext != codeOut) {
            codeOut = codeNext;
            io_->writeSyllableCode(uint8_t(codeOut));
        }
        fbPulseLeftMs -= frameMs;
        matchPulseLeftMs -= frameMs;
        io_->service();

        const double procUs = std::chrono::duration<double, std::micro>(clock::now() - t0).count();
        procAvg = frames == 0 ? procUs : 0.99 * procAvg + 0.01 * procUs;
        if (frames > 10) procMax = std::max(procMax, procUs);  // skip warm-up
        ++frames;
        const size_t backlog = io_->backlog();
        const double backlogMs = backlog * 1000.0 / fs;
        maxBacklog = std::max(maxBacklog, backlogMs);

        // ---- events
        const double t = r.tMs;
        if (r.seg.onset) {
            ++onsets;
            if (r.seg.newIOI) lastIoi = r.seg.ioiMs;
            logEvent(t, "onset", r.seg.newIOI ? fmt("IOI %.0f ms", r.seg.ioiMs) : "first", r.value);
        }
        if (r.seg.offset) {
            lastSd = r.seg.sdMs;
            logEvent(t, "offset", fmt("SD %.0f ms", r.seg.sdMs), r.value);
        }
        if (r.song.active != songWasActive) {
            songWasActive = r.song.active;
            if (r.song.active) ++songBouts;
            logEvent(t, r.song.active ? "song on" : "song off", fmt("fraction %.2f", r.song.fraction), r.song.ratio);
        }
        // Counts are per list position: start over if syllables were added, removed or reordered.
        bool listChanged = countedCodes.size() != p.codes.list.size();
        for (size_t i = 0; !listChanged && i < countedCodes.size(); ++i)
            listChanged = countedCodes[i] != p.codes.list[i].code;
        if (listChanged) {
            countedCodes.clear();
            for (const auto& c : p.codes.list) countedCodes.push_back(c.code);
            codeCounts.assign(p.codes.list.size(), 0);
        }
        if (r.sylCode.evaluated) {
            if (r.sylCode.index >= 0) {
                ++codeCounts[size_t(r.sylCode.index)];
                logEvent(t, "syllable", p.codes.list[size_t(r.sylCode.index)].name + fmt("  (code %.0f)", r.sylCode.code),
                         r.value);
            } else {
                ++codeUnmatched;
            }
        }
        if (r.match.match) {
            ++matches;
            if (r.matchBlocked) ++blocked;
            const SyllableCode* tgt = targetSyllable(p.codes);
            logEvent(t, "match", (tgt ? "target " + tgt->name : std::string("target")) +
                                     (r.matchBlocked ? "  (blocked: no song)" : ""),
                     r.value);
        }
        if (r.fb.trialStarted) ++trials;
        if (r.fb.catchTrial) {
            ++catches;
            logEvent(t, "catch", "probability draw: no feedback", r.value);
        }
        if (r.fb.fire) {
            (r.fb.fire == 1 ? fires1 : fires2)++;
            logEvent(t, r.fb.fire == 1 ? "feedback1" : "feedback2", manual ? "manual" : "", r.value);
        }
        if (pulseEdge) logEvent(t, "pulse in", pulseIgnored ? "ignored (refractory)" : p.pulse.enabled ? "" : "ignored (pulse feedback off)", r.value);
        if (pulseFire) {
            ++pulseFires;
            logEvent(t, "pulse feedback", manual == 3 ? "manual" : fmt("delay %.0f ms, %.0f ms", p.pulse.delayMs, p.pulse.durationMs), r.value);
        }
        if (r.fb.suppressed) {
            ++suppressed;
            logEvent(t, "suppressed", fmt("feedback %.0f below detect-signal threshold", r.fb.suppressed), r.value);
        }
        const std::string outErr = io_->takeOutputError();
        if (!outErr.empty()) logEvent(t, "error", outErr, 0.0);

        // Song files get the same states as the TTL lines, summed into one marker track.
        const float marker = 0.1f * dout.song + 0.2f * dout.syllable + 0.3f * dout.match + 0.4f * dout.feedback;
        recorder_.pushFrame(frame.data(), n, float(r.value), marker, r.song.active);
        recorder_.pushEnv(float(r.value));

        // ---- GUI-visible state
        {
            std::lock_guard<std::mutex> lk(mx_);
            for (float v : frame) raw_.push(v);
            env_.push(float(r.displayValue));
            syl_.push(r.seg.inSyllable ? 1.0f : 0.0f);
            songFrac_.push(float(r.song.fraction));
            songRatio_.push(float(r.song.ratio));
            songOn_.push(r.song.active ? 1.0f : 0.0f);
            const double ts = t / 1000.0;
            if (r.match.match) (r.matchBlocked ? blockedT_ : matchT_).push_back(ts);
            if (r.fb.fire || pulseFire) fireT_.push_back(ts);
            if (r.fb.suppressed) suppressT_.push_back(ts);
            for (auto* q : {&matchT_, &fireT_, &suppressT_, &blockedT_})
                while (!q->empty() && q->front() < ts - kScopeSeconds) q->pop_front();

            auto& s = stats_;
            s.windowSamples = det.windowSamples();
            s.elapsedSec = ts;
            s.frames = frames;
            s.backlogMs = backlogMs;
            s.maxBacklogMs = maxBacklog;
            s.procUsAvg = procAvg;
            s.procUsMax = procMax;
            s.lastIoiMs = lastIoi;
            s.lastSdMs = lastSd;
            s.value = r.value;
            s.displayValue = r.displayValue;
            s.inSyllable = r.seg.inSyllable;
            s.songActive = r.song.active;
            s.songFraction = r.song.fraction;
            s.songRatio = r.song.ratio;
            s.songFftSize = det.songDetector().fftSize();
            s.songBinHz = det.songDetector().binHz();
            s.songBouts = songBouts;
            s.blocked = blocked;
            s.songFilesDone = recorder_.songFilesDone();
            s.songFileOpen = recorder_.songRecording();
            s.currentCode = codeOut;
            s.codeCounts = codeCounts;
            s.codeUnmatched = codeUnmatched;
            s.fbState = r.fbState;
            s.onsets = onsets;
            s.matches = matches;
            s.trials = trials;
            s.fires1 = fires1;
            s.fires2 = fires2;
            s.suppressed = suppressed;
            s.catches = catches;
            s.pulseTrigs = pulseTrigs;
            s.pulseFires = pulseFires;
            s.pulseInput = pulsePrev;
        }
    }

    io_->writeDigital(DigitalOutputs{});
    io_->writeSyllableCode(0);
    running_ = false;
    std::lock_guard<std::mutex> lk(mx_);
    stats_.running = false;
}

EngineStats Engine::stats() const {
    std::lock_guard<std::mutex> lk(mx_);
    return stats_;
}

void Engine::snapshot(double seconds, ScopeSnapshot& out) const {
    std::lock_guard<std::mutex> lk(mx_);
    const double fs = stats_.sampleRate;
    out.rawRate = fs;
    out.envRate = stats_.frameSamples > 0 ? fs / stats_.frameSamples : 0.0;
    out.tEndSec = stats_.elapsedSec;
    if (raw_.d.empty()) {
        out.raw.clear();
        out.env.clear();
        out.syl.clear();
        return;
    }
    raw_.copyLast(size_t(seconds * fs), out.raw);
    env_.copyLast(size_t(seconds * out.envRate), out.env);
    syl_.copyLast(size_t(seconds * out.envRate), out.syl);
    songFrac_.copyLast(size_t(seconds * out.envRate), out.songFrac);
    songRatio_.copyLast(size_t(seconds * out.envRate), out.songRatio);
    songOn_.copyLast(size_t(seconds * out.envRate), out.songOn);
    out.matchT.assign(matchT_.begin(), matchT_.end());
    out.blockedT.assign(blockedT_.begin(), blockedT_.end());
    out.fireT.assign(fireT_.begin(), fireT_.end());
    out.suppressT.assign(suppressT_.begin(), suppressT_.end());
}

std::vector<LogEvent> Engine::events(size_t max) const {
    std::lock_guard<std::mutex> lk(mx_);
    const size_t n = std::min(max, events_.size());
    return std::vector<LogEvent>(events_.end() - std::ptrdiff_t(n), events_.end());
}

std::string Engine::error() const {
    std::lock_guard<std::mutex> lk(mx_);
    return error_;
}

}  // namespace fp

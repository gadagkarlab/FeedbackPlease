#include "app/Box.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>

namespace fp {

namespace {
constexpr double kScopeSeconds = 10.0;  // how much history the GUI can show
constexpr size_t kMaxEvents = 1000;

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
    songFs_ = songs ? songs->sampleRate : 0.0;
    maxMic_ = size_t(kMaxQueueSec * songFs_);
    maxEnv_ = size_t(kMaxQueueSec * envRate);
    pendSkip_ = 0;
    dropped_ = 0;
    frameN_ = 0;
    envOn_ = env_.isOpen();
    if (!envOn_ && !eventsOn_ && !songsOn_) return true;
    stop_ = false;
    th_ = std::thread(&AsyncRecorder::run, this);
    return true;
}

void AsyncRecorder::pushEventLine(std::string line) {
    if (!eventsOn_) return;
    std::lock_guard<std::mutex> lk(mx_);
    if (pendLines_.size() < 200000) pendLines_.push_back(std::move(line));  // bounded like the audio
}

void AsyncRecorder::pushFrame(const float* mic, int n, float detection, float marker, bool song) {
    if (!songsOn_) return;
    std::lock_guard<std::mutex> lk(mx_);
    frameN_ = n;  // constant for a run
    // The writer is far behind (slow or unreachable disk, or starved of CPU): drop this frame
    // rather than queue without limit. The writer is told, so song files end at the gap.
    if (pendMic_.size() + size_t(n) > maxMic_) {
        pendSkip_ += uint64_t(n);
        dropped_ += uint64_t(n);
        return;
    }
    pendMic_.insert(pendMic_.end(), mic, mic + n);
    pendMeta_.push_back({detection, marker, song});
}

void AsyncRecorder::pushEnv(float v) {
    if (!envOn_) return;  // flag, not env_.isOpen(): env_ belongs to the writer thread
    std::lock_guard<std::mutex> lk(mx_);
    if (pendEnv_.size() < maxEnv_) pendEnv_.push_back(v);
}

void AsyncRecorder::run() {
    std::vector<float> mic, e;
    std::vector<FrameMeta> meta;
    std::vector<std::string> lines;
    for (;;) {
        bool done;
        int n;
        uint64_t skip;
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return stop_; });
            // Swapping hands the (cleared) buffers back, so steady state has no reallocations.
            mic.swap(pendMic_);
            meta.swap(pendMeta_);
            e.swap(pendEnv_);
            lines.swap(pendLines_);
            skip = pendSkip_;
            pendSkip_ = 0;
            n = frameN_;
            done = stop_;
        }
        // Song files: pre-roll ring + start/extend/end decisions, frame by frame.
        for (size_t f = 0; f < meta.size() && n > 0; ++f)
            songs_.push(&mic[f * size_t(n)], n, meta[f].detection, meta[f].marker, meta[f].song);
        songs_.skip(skip);  // frames dropped after these (the queue was full)
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
    eventsOn_ = songsOn_ = envOn_ = false;
    // Give the memory back (clear() would keep it): boxes stay alive after Stop.
    std::vector<float>().swap(pendMic_);
    std::vector<FrameMeta>().swap(pendMeta_);
    std::vector<float>().swap(pendEnv_);
    std::vector<std::string>().swap(pendLines_);
    pendSkip_ = 0;
}

double AsyncRecorder::queuedSec() {
    if (songFs_ <= 0) return 0.0;
    std::lock_guard<std::mutex> lk(mx_);
    return double(pendMic_.size()) / songFs_;
}


// ---------------------------------------------------------------- Box

bool Box::begin(const AppParams& p, const std::string& backend, std::string& err) {
    end();
    startParams_ = p;
    {
        std::lock_guard<std::mutex> lk(paramMx_);
        pending_ = p;
    }
    if (!openSession(p, err)) return false;

    fs_ = p.hw.sampleRate;
    const int n = p.hw.samplesPerFrame;
    frameMs_ = n * 1000.0 / fs_;
    // Different seeds per box so their feedback probability draws are independent.
    det_ = std::make_unique<Detector>(uint32_t(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
                                      uint32_t(std::hash<std::string>{}(p.boxName)));
    p_ = p;
    det_->setParams(p_);
    seenVersion_ = paramVersion_.load();
    dout_ = DigitalOutputs{};
    fbPulseLeftMs_ = matchPulseLeftMs_ = 0.0;
    procAvg_ = procMax_ = maxBacklog_ = 0.0;
    frames_ = onsets_ = matches_ = blocked_ = trials_ = fires1_ = fires2_ = suppressed_ = catches_ = 0;
    pulseTrigs_ = pulseFires_ = 0;
    pulsePrev_ = false;
    pulseLastMs_ = -1e12;
    songBouts_ = 0;
    codeCounts_.clear();
    loggedDropSec_ = 0.0;
    lastDropLogMs_ = -1e18;
    countedCodes_.clear();
    codeUnmatched_ = 0;
    codeOut_ = 0;
    codePulseLeftMs_ = 0.0;
    songWasActive_ = false;
    lastIoi_ = lastSd_ = 0.0;

    {
        std::lock_guard<std::mutex> lk(mx_);
        const std::string prefix = stats_.sessionPrefix;
        stats_ = EngineStats{};
        stats_.sessionPrefix = prefix;
        stats_.running = true;
        stats_.backend = backend;
        stats_.sampleRate = fs_;
        stats_.frameSamples = n;
        raw_.reset(size_t(fs_ * kScopeSeconds));
        const double frameRate = fs_ / n;
        for (auto* r : {&env_, &syl_, &songFrac_, &songRatio_, &songOn_, &tmplScore_}) r->reset(size_t(frameRate * kScopeSeconds));
        matchT_.clear();
        fireT_.clear();
        suppressT_.clear();
        blockedT_.clear();
        events_.clear();
        error_.clear();
    }
    begun_ = true;
    return true;
}

void Box::end() {
    if (!begun_) return;
    recorder_.close();
    begun_ = false;
    std::lock_guard<std::mutex> lk(mx_);
    stats_.running = false;
}

void Box::setError(const std::string& e) {
    std::lock_guard<std::mutex> lk(mx_);
    error_ = e;
}

// Layout: <outputDir>/<bird>/<YYYYMMDD>/<bird>_YYYYMMDD_HHMMSS_mmm.wav for the song files
// (day and time of each file's first sample), plus <bird>_<session start>_session_*
// (settings, events, song index, envelope) in the day the session started.
bool Box::openSession(const AppParams& p, std::string& err) {
    namespace fs = std::filesystem;
    std::string prefix;
    const auto& rec = p.rec;
    const bool songFiles = rec.songFiles && (rec.chMic || rec.chDetection || rec.chMarkers);
    if (rec.saveEvents || songFiles || rec.saveEnvelope) {
        // Sample 0 is (to within a frame) now; song file times count samples from here.
        const double startMs = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
        const std::string bird = birdFileName(p.birdName);
        const fs::path birdDir = fs::u8path(rec.outputDir) / fs::u8path(bird);
        const fs::path dayDir = birdDir / localDay(startMs);
        std::error_code ec;
        fs::create_directories(dayDir, ec);
        if (ec) {
            err = "cannot create " + dayDir.u8string() + ": " + ec.message();
            return false;
        }
        const std::string stamp = localStamp(startMs);
        prefix = (dayDir / fs::u8path(bird + "_" + stamp.substr(0, 15) + "_session")).u8string();
        saveParams(prefix + "_settings.ini", p);
        const double fs = p.hw.sampleRate;
        SongFileConfig songs;
        songs.indexPath = prefix + "_songs.csv";
        songs.birdDir = birdDir.u8string();
        songs.bird = bird;
        songs.startEpochMs = startMs;
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

void Box::updateParams(const AppParams& p) {
    std::lock_guard<std::mutex> lk(paramMx_);
    pending_ = p;
    ++paramVersion_;
}

void Box::logEvent(double tMs, const char* type, const std::string& detail, double value) {
    if (recorder_.eventsOpen()) {
        const uint64_t sample = uint64_t(std::llround(tMs * fs_ / 1000.0));
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

void Box::process(const float* x, int n, IOBackend& io, size_t backlogSamples) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const double frameMs = frameMs_;
    auto& p = p_;

    if (paramVersion_.load() != seenVersion_) {
        std::lock_guard<std::mutex> lk(paramMx_);
        seenVersion_ = paramVersion_.load();
        AppParams np = pending_;
        np.hw = startParams_.hw;  // hardware is fixed for the run
        np.boxName = startParams_.boxName;
        p = np;
        det_->setParams(p);
    }
    if (resetCounters_.exchange(false)) {
        onsets_ = matches_ = blocked_ = trials_ = fires1_ = fires2_ = suppressed_ = catches_ = songBouts_ = 0;
        pulseTrigs_ = pulseFires_ = 0;
        std::fill(codeCounts_.begin(), codeCounts_.end(), 0);
        codeUnmatched_ = 0;
        procMax_ = maxBacklog_ = 0.0;
    }

    FrameResult r = det_->process(x, n);

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
    const int pulseLevel = io.pollPulseInput();
    if (pulseLevel >= 0) {
        pulseEdge = pulseLevel == 1 && !pulsePrev_;
        pulsePrev_ = pulseLevel == 1;
    }
    if (pulseEdge) {
        ++pulseTrigs_;
        if (p.pulse.enabled) {
            if (r.tMs - pulseLastMs_ >= p.pulse.refractoryMs) {
                pulseFire = 3;
                pulseLastMs_ = r.tMs;
            } else {
                pulseIgnored = true;
            }
        }
    }

    // ---- outputs first, bookkeeping after
    if (r.fb.arm) io.armWave(r.fb.arm);
    if (r.fb.fire) {
        io.fireWave(r.fb.fire);
        fbPulseLeftMs_ = std::max(p.fb.ttlPulseMs, frameMs);
    }
    if (pulseFire) {
        io.fireWave(3);
        fbPulseLeftMs_ = std::max(p.fb.ttlPulseMs, frameMs);
    }
    if (r.match.match) matchPulseLeftMs_ = std::max(p.fb.ttlPulseMs, frameMs);
    dout_.feedback = fbPulseLeftMs_ > 0.0;
    dout_.match = matchPulseLeftMs_ > 0.0;
    dout_.syllable = r.seg.inSyllable;
    dout_.song = r.song.active;
    io.writeDigital(dout_);

    // Syllable code port: set at each IOI end to the first matching entry's code
    // (0 if none), then cleared according to the hold mode.
    int codeNext = codeOut_;
    if (r.sylCode.evaluated) {
        codeNext = r.sylCode.code;
        codePulseLeftMs_ = p.codes.pulseMs;
    } else if (r.tmpl.index >= 0) {  // template method: set when a template is detected
        codeNext = r.tmpl.code;
        codePulseLeftMs_ = p.codes.pulseMs;
    } else if (p.codes.hold == SylCodeHold::UntilOffset && r.seg.offset) {
        codeNext = 0;
    } else if (p.codes.hold == SylCodeHold::Pulse) {
        codePulseLeftMs_ -= frameMs;
        if (codePulseLeftMs_ <= 0.0) codeNext = 0;
    }
    if (!p.codes.enabled) codeNext = 0;
    if (codeNext != codeOut_) {
        codeOut_ = codeNext;
        io.writeSyllableCode(uint8_t(codeOut_));
    }
    fbPulseLeftMs_ -= frameMs;
    matchPulseLeftMs_ -= frameMs;
    io.service();

    const double procUs = std::chrono::duration<double, std::micro>(clock::now() - t0).count();
    procAvg_ = frames_ == 0 ? procUs : 0.99 * procAvg_ + 0.01 * procUs;
    if (frames_ > 10) procMax_ = std::max(procMax_, procUs);  // skip warm-up
    ++frames_;
    const double backlogMs = backlogSamples * 1000.0 / fs_;
    if (frames_ > 100) maxBacklog_ = std::max(maxBacklog_, backlogMs);  // skip the start-up catch-up

    // ---- events
    const double t = r.tMs;
    if (r.seg.onset) {
        ++onsets_;
        if (r.seg.newIOI) lastIoi_ = r.seg.ioiMs;
        logEvent(t, "onset", r.seg.newIOI ? fmt("IOI %.0f ms", r.seg.ioiMs) : "first", r.value);
    }
    if (r.seg.offset) {
        lastSd_ = r.seg.sdMs;
        logEvent(t, "offset", fmt("SD %.0f ms", r.seg.sdMs), r.value);
    }
    if (r.song.active != songWasActive_) {
        songWasActive_ = r.song.active;
        if (r.song.active) ++songBouts_;
        logEvent(t, r.song.active ? "song on" : "song off", fmt("fraction %.2f", r.song.fraction), r.song.ratio);
    }
    // Counts are per list position: start over if syllables were added, removed or reordered.
    bool listChanged = countedCodes_.size() != p.codes.list.size();
    for (size_t i = 0; !listChanged && i < countedCodes_.size(); ++i)
        listChanged = countedCodes_[i] != p.codes.list[i].code;
    if (listChanged) {
        countedCodes_.clear();
        for (const auto& c : p.codes.list) countedCodes_.push_back(c.code);
        codeCounts_.assign(p.codes.list.size(), 0);
    }
    if (r.sylCode.evaluated) {
        if (r.sylCode.index >= 0) {
            ++codeCounts_[size_t(r.sylCode.index)];
            logEvent(t, "syllable", p.codes.list[size_t(r.sylCode.index)].name + fmt("  (code %.0f)", r.sylCode.code),
                     r.value);
        } else {
            ++codeUnmatched_;
        }
    }
    if (p.tmpl.method == DetectMethod::Template)
        for (const auto& h : det_->templateMatcher().hits())
            if (h.index < int(p.tmpl.list.size()))
                logEvent(t, "template",
                         p.tmpl.list[size_t(h.index)].name + fmt("  (code %.0f)", double(p.tmpl.list[size_t(h.index)].code)) +
                             (h.gated ? "" : "  not counted: timing gate"),
                         r.tmpl.levelDb);
    if (r.match.match) {
        ++matches_;
        if (r.matchBlocked) ++blocked_;
        const SyllableCode* tgt = targetSyllable(p.codes);
        const bool tm = p.tmpl.method == DetectMethod::Template;
        const std::string tname = tm ? (p.tmpl.target >= 0 && p.tmpl.target < int(p.tmpl.list.size())
                                            ? p.tmpl.list[size_t(p.tmpl.target)].name
                                            : std::string())
                                     : (tgt ? tgt->name : std::string());
        logEvent(t, "match",
                 "target " + tname + (r.matchBlocked ? "  (blocked: no song)" : ""),
                 r.value);
    }
    if (r.fb.trialStarted) ++trials_;
    if (r.fb.catchTrial) {
        ++catches_;
        logEvent(t, "catch", "probability draw: no feedback", r.value);
    }
    if (r.fb.fire) {
        (r.fb.fire == 1 ? fires1_ : fires2_)++;
        logEvent(t, r.fb.fire == 1 ? "feedback1" : "feedback2", manual ? "manual" : "", r.value);
    }
    if (pulseEdge)
        logEvent(t, "pulse in",
                 pulseIgnored ? "ignored (refractory)" : p.pulse.enabled ? "" : "ignored (pulse feedback off)", r.value);
    if (pulseFire) {
        ++pulseFires_;
        logEvent(t, "pulse feedback",
                 manual == 3 ? "manual" : fmt("delay %.0f ms, %.0f ms", p.pulse.delayMs, p.pulse.durationMs), r.value);
    }
    if (r.fb.suppressed) {
        ++suppressed_;
        logEvent(t, "suppressed", fmt("feedback %.0f below detect-signal threshold", r.fb.suppressed), r.value);
    }
    // Recording fell behind and audio was dropped (at most one message every 10 s).
    const double droppedSec = recorder_.droppedSec();
    if (droppedSec > loggedDropSec_ && t - lastDropLogMs_ >= 10000.0) {
        logEvent(t, "error",
                 fmt("recording fell behind: %.1f s of audio dropped so far (disk too slow or CPU starved)", droppedSec),
                 0.0);
        loggedDropSec_ = droppedSec;
        lastDropLogMs_ = t;
    }
    const std::string outErr = io.takeOutputError();
    if (!outErr.empty()) logEvent(t, "error", outErr, 0.0);

    // Song files get the same states as the TTL lines, summed into one marker track.
    const float marker = 0.1f * dout_.song + 0.2f * dout_.syllable + 0.3f * dout_.match + 0.4f * dout_.feedback;
    recorder_.pushFrame(x, n, float(r.value), marker, r.song.active);
    recorder_.pushEnv(float(r.value));

    // ---- GUI-visible state
    std::lock_guard<std::mutex> lk(mx_);
    for (int i = 0; i < n; ++i) raw_.push(x[i]);
    env_.push(float(r.displayValue));
    syl_.push(r.seg.inSyllable ? 1.0f : 0.0f);
    songFrac_.push(float(r.song.fraction));
    songRatio_.push(float(r.song.ratio));
    songOn_.push(r.song.active ? 1.0f : 0.0f);
    tmplScore_.push(float(r.tmpl.targetScore));
    const double ts = t / 1000.0;
    if (r.match.match) (r.matchBlocked ? blockedT_ : matchT_).push_back(ts);
    if (r.fb.fire || pulseFire) fireT_.push_back(ts);
    if (r.fb.suppressed) suppressT_.push_back(ts);
    for (auto* q : {&matchT_, &fireT_, &suppressT_, &blockedT_})
        while (!q->empty() && q->front() < ts - kScopeSeconds) q->pop_front();

    auto& s = stats_;
    s.windowSamples = det_->windowSamples();
    s.elapsedSec = ts;
    s.frames = frames_;
    s.backlogMs = backlogMs;
    s.maxBacklogMs = maxBacklog_;
    s.procUsAvg = procAvg_;
    s.procUsMax = procMax_;
    s.lastIoiMs = lastIoi_;
    s.lastSdMs = lastSd_;
    s.value = r.value;
    s.displayValue = r.displayValue;
    s.inSyllable = r.seg.inSyllable;
    s.songActive = r.song.active;
    s.songFraction = r.song.fraction;
    s.songRatio = r.song.ratio;
    s.songFftSize = det_->songDetector().fftSize();
    s.songBinHz = det_->songDetector().binHz();
    s.songBouts = songBouts_;
    s.blocked = blocked_;
    s.songFilesDone = recorder_.songFilesDone();
    s.songFileOpen = recorder_.songRecording();
    s.recQueueSec = recorder_.queuedSec();
    s.recDroppedSec = recorder_.droppedSec();
    s.currentCode = codeOut_;
    s.codeCounts = codeCounts_;
    s.codeUnmatched = codeUnmatched_;
    s.fbState = r.fbState;
    s.onsets = onsets_;
    s.matches = matches_;
    s.trials = trials_;
    s.fires1 = fires1_;
    s.fires2 = fires2_;
    s.suppressed = suppressed_;
    s.catches = catches_;
    s.pulseTrigs = pulseTrigs_;
    s.pulseFires = pulseFires_;
    s.pulseInput = pulsePrev_;
}

void Box::finish(IOBackend& io) {
    io.writeDigital(DigitalOutputs{});
    io.writeSyllableCode(0);
}

EngineStats Box::stats() const {
    std::lock_guard<std::mutex> lk(mx_);
    return stats_;
}

void Box::snapshot(double seconds, ScopeSnapshot& out) const {
    std::lock_guard<std::mutex> lk(mx_);
    const double fs = stats_.sampleRate;
    out.rawRate = fs;
    out.envRate = stats_.frameSamples > 0 ? fs / stats_.frameSamples : 0.0;
    out.tEndSec = stats_.elapsedSec;
    if (raw_.d.empty()) {
        out = ScopeSnapshot{};
        return;
    }
    raw_.copyLast(size_t(seconds * fs), out.raw);
    env_.copyLast(size_t(seconds * out.envRate), out.env);
    syl_.copyLast(size_t(seconds * out.envRate), out.syl);
    songFrac_.copyLast(size_t(seconds * out.envRate), out.songFrac);
    songRatio_.copyLast(size_t(seconds * out.envRate), out.songRatio);
    songOn_.copyLast(size_t(seconds * out.envRate), out.songOn);
    tmplScore_.copyLast(size_t(seconds * out.envRate), out.tmplScore);
    out.matchT.assign(matchT_.begin(), matchT_.end());
    out.blockedT.assign(blockedT_.begin(), blockedT_.end());
    out.fireT.assign(fireT_.begin(), fireT_.end());
    out.suppressT.assign(suppressT_.begin(), suppressT_.end());
}

std::vector<LogEvent> Box::events(size_t max) const {
    std::lock_guard<std::mutex> lk(mx_);
    const size_t n = std::min(max, events_.size());
    return std::vector<LogEvent>(events_.end() - std::ptrdiff_t(n), events_.end());
}

std::string Box::error() const {
    std::lock_guard<std::mutex> lk(mx_);
    return error_;
}

}  // namespace fp

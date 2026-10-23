#include "core/Offline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "core/BandEnvelope.h"
#include "core/Detector.h"
#include "core/SongDetector.h"

namespace fp {

AppParams paramsForRate(const AppParams& p, double fileRate) {
    AppParams r = p;
    r.hw.samplesPerFrame = std::max(1, int(std::lround(p.hw.samplesPerFrame * fileRate / p.hw.sampleRate)));
    r.hw.sampleRate = fileRate;
    return r;
}

bool computeEnvelopeTrack(const std::vector<float>& x, const AppParams& pr, EnvelopeTrack& out,
                          const std::atomic<bool>* cancel, std::atomic<float>* progress, int threads) {
    const int n = pr.hw.samplesPerFrame;
    const int N = Detector::windowSamplesFor(pr);
    const size_t frames = x.size() / size_t(n);
    out.sampleRate = pr.hw.sampleRate;
    out.frameSamples = n;
    out.value.assign(frames, 0.0f);
    if (frames == 0) return true;

    if (threads <= 0) threads = std::max(1, int(std::thread::hardware_concurrency()) - 1);
    threads = int(std::min<size_t>(size_t(threads), std::max<size_t>(1, frames / 256)));
    std::atomic<size_t> done{0};
    std::atomic<bool> stopped{false};

    // Each frame's value depends only on the N samples ending at that frame, so frames
    // can be computed independently. Same BandEnvelope code as real time => same numbers.
    auto worker = [&](size_t k0, size_t k1) {
#ifdef _WIN32
        // Tuning can run while acquiring live: never compete with the engine thread.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        BandEnvelope env;
        env.configure(pr.hw.sampleRate, N, pr.spectrum, pr.proc);
        for (size_t k = k0; k < k1; ++k) {
            if (((k - k0) & 255) == 0) {
                if (cancel && cancel->load()) {
                    stopped = true;
                    return;
                }
                if (progress && k > k0) {
                    const size_t d = done.fetch_add(256) + 256;
                    progress->store(float(d) / float(frames));
                }
            }
            const size_t end = (k + 1) * size_t(n);
            if (end >= size_t(N)) out.value[k] = float(env.compute(&x[end - N]).value);
        }
    };
    std::vector<std::thread> pool;
    const size_t per = (frames + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const size_t a = t * per, b = std::min(frames, a + per);
        if (a < b) pool.emplace_back(worker, a, b);
    }
    for (auto& th : pool) th.join();
    if (progress) progress->store(1.0f);
    return !stopped;
}

void computeSongTrack(const std::vector<float>& x, const AppParams& pr, SongTrack& out) {
    const int n = pr.hw.samplesPerFrame;
    const size_t frames = x.size() / size_t(n);
    SongDetector sd;
    sd.configure(pr.hw.sampleRate, pr.song);
    out.ratio.resize(frames);
    out.fraction.resize(frames);
    out.active.resize(frames);
    for (size_t k = 0; k < frames; ++k) {
        const SongState& s = sd.process(&x[k * size_t(n)], n);
        out.ratio[k] = float(s.ratio);
        out.fraction[k] = float(s.fraction);
        out.active[k] = s.active ? 1 : 0;
    }
}

void runDecisions(const EnvelopeTrack& env, const SongTrack& song, const AppParams& pr, DecisionTrack& out,
                  uint32_t seed) {
    out = DecisionTrack{};
    const size_t frames = std::min(env.value.size(), song.active.size());
    out.inSyllable.resize(frames);
    Detector det(seed);
    det.setParams(pr);
    const double dtMs = env.frameMs();
    SongState st;
    for (size_t k = 0; k < frames; ++k) {
        st.ratio = song.ratio[k];
        st.fraction = song.fraction[k];
        st.active = song.active[k] != 0;
        st.detected = st.active;
        const FrameResult r = det.decide(uint64_t(k + 1) * uint64_t(env.frameSamples), env.value[k], st, dtMs);
        out.inSyllable[k] = r.seg.inSyllable ? 1 : 0;
        if (r.seg.offset) {
            // The gap turned out real (>= min gap): the frames since the drop were not syllable.
            const size_t first = size_t(std::max(0.0, std::round(r.seg.offsetMs / dtMs) - 1.0));
            for (size_t j = first; j <= k; ++j) out.inSyllable[j] = 0;
            if (!out.syllables.empty()) {
                auto& s = out.syllables.back();
                s.offsetMs = r.seg.offsetMs;
                s.sdMs = r.seg.sdMs;
            }
        }
        if (r.seg.onset) {
            SyllableInfo s;
            s.onsetMs = r.tMs;
            if (r.seg.newIOI) s.ioiMs = r.seg.ioiMs;
            if (!out.syllables.empty()) {
                s.prevIoiMs = out.syllables.back().ioiMs;
                s.prevSdMs = out.syllables.back().sdMs;
            }
            if (r.sylCode.evaluated) {
                s.codeIndex = r.sylCode.index;
                s.code = r.sylCode.code;
            }
            out.syllables.push_back(s);
        }
        if (r.match.match) {
            // Onset-evaluated types match at this syllable's onset; offset-evaluated
            // types (SD, IOI+SD) at its offset. Either way it's the latest syllable.
            if (!out.syllables.empty()) out.syllables.back().matched = true;
            (r.matchBlocked ? out.blockedMs : out.matchMs).push_back(r.tMs);
        }
        if (r.fb.catchTrial) out.catchMs.push_back(r.tMs);
        if (r.fb.fire == 1) out.fire1Ms.push_back(r.tMs);
        if (r.fb.fire == 2) out.fire2Ms.push_back(r.tMs);
        if (r.fb.suppressed) out.suppressedMs.push_back(r.tMs);
    }
}

namespace {
struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
    Hasher& operator<<(double v) { bytes(&v, sizeof v); return *this; }
    Hasher& operator<<(int v) { bytes(&v, sizeof v); return *this; }
    Hasher& operator<<(bool v) { return *this << int(v); }
};
}  // namespace

uint64_t envelopeKey(const AppParams& p) {
    Hasher h;
    h << p.hw.sampleRate << p.hw.samplesPerFrame << p.spectrum.windowMs << int(p.spectrum.combination);
    for (const auto& b : p.spectrum.include) h << b.gain << b.lowHz << b.highHz;
    for (const auto& b : p.spectrum.subtract) h << b.gain << b.lowHz << b.highHz;
    h << p.proc.meanSubtract << p.proc.lowCutoff << p.proc.lowCutoffThresh << p.proc.softwareGain << p.proc.gain;
    return h.h;
}

uint64_t songKey(const AppParams& p) {
    const auto& s = p.song;
    Hasher h;
    h << p.hw.sampleRate << p.hw.samplesPerFrame << s.enabled << s.meanSubtract << s.gain << s.offset
      << s.butterworth << s.filterOrder << s.filterLowHz << s.filterHighHz << s.windowLength << s.frequencyBins
      << s.hopMs << s.nonBirdCutoffHz << s.birdLowHz << s.birdHighHz << int(s.ratioMode) << s.ratioThresh
      << s.minBirdRms << s.songDurationSec << s.durationThresh << s.holdSec;
    return h.h;
}

uint64_t decisionKey(const AppParams& p) {
    Hasher h;
    h << p.spectrum.sylStartThresh << p.spectrum.sylEndThresh << p.spectrum.minGapMs;
    h << p.codes.target;
    const auto& f = p.fb;
    h << f.enabled << f.prob1 << f.delay1Ms << f.thresh1 << f.prob2 << f.delay2Ms << f.thresh2 << int(f.mode2)
      << f.refractoryMs << f.extraDelayMs;
    h << p.song.enabled << p.song.gateFeedback;
    h << p.codes.enabled << int(p.codes.list.size());
    for (const auto& c : p.codes.list) {
        h << c.enabled << c.code << int(c.type) << int(c.mode);
        for (const TimeWindow* w : {&c.ioi, &c.sd, &c.prevIoi}) h << w->setpointMs << w->epsMs << w->minMs << w->maxMs;
    }
    return h.h;
}

}  // namespace fp

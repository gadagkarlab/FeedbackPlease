#include "core/Detector.h"

#include <algorithm>
#include <cmath>

namespace fp {

Detector::Detector(uint32_t seed) : fsm_(seed) {
    classifier_.reset();
    setParams(AppParams{});
}

int Detector::windowSamplesFor(const AppParams& p) {
    return std::max(16, int(std::lround(p.spectrum.windowMs * p.hw.sampleRate / 1000.0)));
}

void Detector::setParams(const AppParams& p) {
    const int n = windowSamplesFor(p);
    const bool resize = n != windowN_ || p.hw.sampleRate != p_.hw.sampleRate;
    p_ = p;
    if (resize) {
        windowN_ = n;
        ring_.assign(2 * size_t(n), 0.0f);
        head_ = 0;
        filled_ = 0;
    }
    env_.configure(p_.hw.sampleRate, windowN_, p_.spectrum, p_.proc);
    song_.configure(p_.hw.sampleRate, p_.song);
}

void Detector::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    head_ = 0;
    filled_ = 0;
    samples_ = 0;
    seg_.reset();
    classifier_.reset();
    song_.reset();
    fsm_.reset();
}

double Detector::toDisplay(double v, bool dB) {
    return dB ? 20.0 * std::log10(std::max(v, 1e-9)) : v;
}

FrameResult Detector::process(const float* x, int n) {
    for (int i = 0; i < n; ++i) {
        ring_[head_] = x[i];
        ring_[head_ + windowN_] = x[i];
        if (++head_ == windowN_) head_ = 0;
    }
    filled_ = std::min(windowN_, filled_ + n);
    samples_ += uint64_t(n);

    BandEnvelope::Result e;
    const bool ready = filled_ >= windowN_;
    if (ready) e = env_.compute(&ring_[head_]);  // oldest sample is at head_
    const SongState& song = song_.process(x, n);

    FrameResult r = decide(samples_, e.value, song, n * 1000.0 / p_.hw.sampleRate);
    r.ready = ready;
    r.includeAmp = e.includeAmp;
    r.subtractAmp = e.subtractAmp;
    return r;
}

FrameResult Detector::decide(uint64_t sampleEnd, double value, const SongState& song, double dtMs) {
    FrameResult r;
    r.sampleEnd = sampleEnd;
    r.tMs = double(sampleEnd) * 1000.0 / p_.hw.sampleRate;
    r.ready = true;
    r.value = value;
    r.displayValue = toDisplay(value, p_.proc.displayDb);

    r.seg = seg_.update(value, r.tMs, p_.spectrum.sylStartThresh, p_.spectrum.sylEndThresh, p_.spectrum.minGapMs);
    r.sylCode = classifier_.update(r.seg, p_.codes);
    // The target is just one of the syllables: it matches when it is the first match.
    r.match.evaluated = r.sylCode.evaluated;
    r.match.match = r.sylCode.evaluated && r.sylCode.index >= 0 && r.sylCode.index == p_.codes.target;

    r.song = song;
    r.songGateOpen = !p_.song.enabled || !p_.song.gateFeedback || song.active;
    r.matchBlocked = r.match.match && !r.songGateOpen;
    r.fb = fsm_.update(r.match.match && r.songGateOpen, value, dtMs, p_.fb);
    r.fbState = fsm_.state();
    return r;
}

}  // namespace fp

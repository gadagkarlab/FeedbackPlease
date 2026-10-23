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
    const int nf = TemplateFeatures::windowSamplesFor(p.hw.sampleRate, p.tmpl);
    const int ring = std::max(n, nf);
    const bool resize = ring != ringN_ || p.hw.sampleRate != p_.hw.sampleRate;
    p_ = p;
    windowN_ = n;
    if (resize) {
        ringN_ = ring;
        ring_.assign(2 * size_t(ring), 0.0f);
        head_ = 0;
        filled_ = 0;
    }
    env_.configure(p_.hw.sampleRate, windowN_, p_.spectrum, p_.proc);
    song_.configure(p_.hw.sampleRate, p_.song);
    feat_.configure(p_.hw.sampleRate, p_.tmpl, p_.proc.meanSubtract);
    shape_.assign(size_t(feat_.bands()), 0.0f);
}

void Detector::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    head_ = 0;
    filled_ = 0;
    samples_ = 0;
    seg_.reset();
    classifier_.reset();
    matcher_.reset();
    song_.reset();
    fsm_.reset();
}

double Detector::toDisplay(double v, bool dB) {
    return dB ? 20.0 * std::log10(std::max(v, 1e-9)) : v;
}

FrameResult Detector::process(const float* x, int n) {
    for (int i = 0; i < n; ++i) {
        ring_[head_] = x[i];
        ring_[head_ + ringN_] = x[i];
        if (++head_ == ringN_) head_ = 0;
    }
    filled_ = std::min(ringN_, filled_ + n);
    samples_ += uint64_t(n);
    // The newest m samples, oldest first, are contiguous ending at head_ + ringN_.
    auto last = [&](int m) { return &ring_[size_t(head_ + ringN_ - m)]; };

    BandEnvelope::Result e;
    const bool ready = filled_ >= windowN_;
    if (ready) e = env_.compute(last(windowN_));
    const float* shape = nullptr;
    double level = -200.0;
    if (p_.tmpl.method == DetectMethod::Template && filled_ >= feat_.windowSamples()) {
        level = feat_.compute(last(feat_.windowSamples()), shape_.data());
        shape = shape_.data();
    }
    const SongState& song = song_.process(x, n);

    FrameResult r = decide(samples_, e.value, song, n * 1000.0 / p_.hw.sampleRate, shape, level);
    r.ready = ready;
    r.includeAmp = e.includeAmp;
    r.subtractAmp = e.subtractAmp;
    return r;
}

FrameResult Detector::decide(uint64_t sampleEnd, double value, const SongState& song, double dtMs,
                             const float* shape, double levelDb) {
    FrameResult r;
    r.sampleEnd = sampleEnd;
    r.tMs = double(sampleEnd) * 1000.0 / p_.hw.sampleRate;
    r.ready = true;
    r.value = value;
    r.displayValue = toDisplay(value, p_.proc.displayDb);

    r.seg = seg_.update(value, r.tMs, p_.spectrum.sylStartThresh, p_.spectrum.sylEndThresh, p_.spectrum.minGapMs);
    if (p_.tmpl.method == DetectMethod::Template) {
        // Templates replace the IOI / SD syllables: the target matches when its template
        // is detected (and passes its timing gate).
        r.tmpl = matcher_.update(shape, feat_.bands(), levelDb, r.tMs, p_.tmpl);
        r.match.evaluated = r.tmpl.evaluated;
        for (const auto& h : matcher_.hits())
            if (h.gated && h.index == p_.tmpl.target) r.match.match = true;
    } else {
        r.sylCode = classifier_.update(r.seg, p_.codes);
        // The target is just one of the syllables: it matches when it is the first match.
        r.match.evaluated = r.sylCode.evaluated;
        r.match.match = r.sylCode.evaluated && r.sylCode.index >= 0 && r.sylCode.index == p_.codes.target;
    }

    r.song = song;
    r.songGateOpen = !p_.song.enabled || !p_.song.gateFeedback || song.active;
    r.matchBlocked = r.match.match && !r.songGateOpen;
    r.fb = fsm_.update(r.match.match && r.songGateOpen, value, dtMs, p_.fb);
    r.fbState = fsm_.state();
    return r;
}

}  // namespace fp

#include "core/SpectralTemplate.h"

#include <algorithm>
#include <cmath>

#include "core/Filters.h"

namespace fp {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kNever = -1e18;

void normalize(float* v, int n) {
    double m = 0.0;
    for (int i = 0; i < n; ++i) m += v[i];
    m /= std::max(1, n);
    double ss = 0.0;
    for (int i = 0; i < n; ++i) {
        v[i] = float(v[i] - m);
        ss += double(v[i]) * v[i];
    }
    const double inv = ss > 1e-12 ? 1.0 / std::sqrt(ss) : 0.0;
    for (int i = 0; i < n; ++i) v[i] = float(v[i] * inv);
}
}  // namespace

int TemplateFeatures::windowSamplesFor(double fs, const TemplateParams& tp) {
    return std::max(16, int(std::lround(tp.windowMs * fs / 1000.0)));
}

void TemplateFeatures::configure(double fs, const TemplateParams& tp, bool meanSubtract) {
    fs_ = fs;
    meanSubtract_ = meanSubtract;
    n_ = windowSamplesFor(fs, tp);
    nfft_ = nextPow2(n_);
    win_.resize(size_t(n_));
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n_; ++i) {
        win_[size_t(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n_);
        s1 += win_[size_t(i)];
        s2 += win_[size_t(i)] * win_[size_t(i)];
    }
    // Single-sided bin power in Vrms^2 of a sine (x2 / s1^2), and summed bins divided by
    // the ENBW in bins (zero padding scales it by nfft / n) give the RMS power in a band.
    const double enbw = n_ * s2 / (s1 * s1) * double(nfft_) / n_;
    norm_ = 2.0 / (s1 * s1) / enbw;

    const double df = fs / nfft_;
    const int nyq = nfft_ / 2;
    bands_ = templateBands(tp);
    bandK0_.assign(size_t(bands_), 0);
    bandK1_.assign(size_t(bands_), 0);
    for (int b = 0; b < bands_; ++b) {
        const double lo = tp.lowHz + b * tp.bandHz, hi = std::min(tp.highHz, lo + tp.bandHz);
        int k0 = std::clamp(int(std::ceil(lo / df)), 1, nyq), k1 = std::clamp(int(std::ceil(hi / df)) - 1, 1, nyq);
        if (k1 < k0) k0 = k1 = std::clamp(int(std::lround(0.5 * (lo + hi) / df)), 1, nyq);  // narrower than a bin
        bandK0_[size_t(b)] = k0;
        bandK1_[size_t(b)] = k1;
    }
    levelK0_ = bands_ > 0 ? bandK0_.front() : 0;
    levelK1_ = bands_ > 0 ? bandK1_.back() : -1;
    buf_.resize(size_t(nfft_));
    power_.resize(size_t(nyq) + 1);
}

double TemplateFeatures::compute(const float* x, float* shape) {
    double mean = 0.0;
    if (meanSubtract_) {
        for (int i = 0; i < n_; ++i) mean += x[i];
        mean /= n_;
    }
    for (int i = 0; i < n_; ++i) buf_[size_t(i)] = {(x[i] - mean) * win_[size_t(i)], 0.0};
    std::fill(buf_.begin() + n_, buf_.end(), std::complex<double>{});
    fftInPlace(buf_);
    for (int k = levelK0_; k <= levelK1_; ++k) power_[size_t(k)] = std::norm(buf_[size_t(k)]) * norm_;

    double level = 0.0;
    for (int k = levelK0_; k <= levelK1_; ++k) level += power_[size_t(k)];
    for (int b = 0; b < bands_; ++b) {
        double p = 0.0;
        for (int k = bandK0_[size_t(b)]; k <= bandK1_[size_t(b)]; ++k) p += power_[size_t(k)];
        shape[b] = float(std::log10(p + 1e-20));
    }
    normalize(shape, bands_);
    return 10.0 * std::log10(level + 1e-20);
}

// ---------------------------------------------------------------- matcher

void TemplateMatcher::reset() {
    scores_.clear();
    run_.clear();
    lastMs_.clear();
    hits_.clear();
}

double TemplateMatcher::correlate(const float* shape, int bands, const std::vector<float>& t) {
    if (!shape || bands <= 0 || int(t.size()) != bands) return 0.0;
    double d = 0.0;
    for (int i = 0; i < bands; ++i) d += double(shape[i]) * t[size_t(i)];
    return d;
}

double TemplateMatcher::score(const float* shape, int bands, const SpectralTemplate& t) {
    if (t.discriminative && int(t.weights.size()) == bands) return correlate(shape, bands, t.weights) + t.bias;
    return correlate(shape, bands, t.shape);
}

std::vector<float> TemplateMatcher::average(const std::vector<std::vector<float>>& shapes) {
    std::vector<float> out;
    for (const auto& s : shapes) {
        if (s.empty()) continue;
        if (out.empty()) out.assign(s.size(), 0.0f);
        if (s.size() != out.size()) continue;
        for (size_t i = 0; i < s.size(); ++i) out[i] += s[i];
    }
    normalize(out.data(), int(out.size()));
    return out;
}

TemplateResult TemplateMatcher::update(const float* shape, int bands, double levelDb, double tMs,
                                       const TemplateParams& tp) {
    TemplateResult r;
    const size_t n = tp.list.size();
    if (scores_.size() != n) {  // templates added or removed: start over
        scores_.assign(n, 0.0f);
        run_.assign(n, 0);
        lastMs_.assign(n, kNever);
    }
    hits_.clear();
    r.evaluated = shape != nullptr;
    r.levelDb = levelDb;
    const bool loud = shape && levelDb >= tp.minLevelDb;
    for (size_t i = 0; i < n; ++i) {
        const SpectralTemplate& t = tp.list[i];
        const double s = loud ? score(shape, bands, t) : 0.0;
        scores_[i] = float(s);
        run_[i] = (t.enabled && loud && s >= t.thresh) ? run_[i] + 1 : 0;
        // Detected once per stretch above threshold, when it has lasted minFrames.
        if (run_[i] != std::max(1, t.minFrames) || tMs - lastMs_[i] < tp.rearmMs) continue;
        lastMs_[i] = tMs;
        bool gated = true;
        if (t.gateIndex >= 0 && size_t(t.gateIndex) < n && size_t(t.gateIndex) != i) {
            const double dt = tMs - lastMs_[size_t(t.gateIndex)];
            gated = dt >= t.gateMinMs && dt <= t.gateMaxMs;
        }
        hits_.push_back({int(i), gated});
        if (gated && r.index < 0) {
            r.index = int(i);
            r.code = t.code;
        }
    }
    if (tp.target >= 0 && size_t(tp.target) < n) r.targetScore = scores_[size_t(tp.target)];
    return r;
}

}  // namespace fp

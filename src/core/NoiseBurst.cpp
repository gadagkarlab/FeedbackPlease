#include "core/NoiseBurst.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "core/Filters.h"

namespace fp {

namespace {
size_t samplesFor(double ms, double rate) { return ms <= 0 ? 0 : size_t(std::llround(ms * rate / 1000.0)); }
}  // namespace

std::vector<double> makeNoiseBurst(const PulseFeedbackParams& p, double rate, double vMin, double vMax, uint32_t seed,
                                   std::string* err) {
    if (!(p.lowHz > 0 && p.highHz > p.lowHz && p.highHz < rate / 2 && p.filterOrder >= 1)) {
        if (err) *err = "noise band must satisfy 0 < low < high < rate/2 (" + std::to_string(rate / 2) + " Hz) and order >= 1";
        return {};
    }
    ButterworthBandpass bp;
    bp.design(rate, p.lowHz, p.highHz, p.filterOrder);
    if (!bp.valid()) {
        if (err) *err = "could not design the noise bandpass filter";
        return {};
    }
    const size_t nDelay = samplesFor(p.delayMs, rate), nNoise = samplesFor(p.durationMs, rate);
    std::vector<float> x(nNoise);
    std::mt19937 rng(seed);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    for (auto& v : x) v = gauss(rng) * float(p.gain);
    bp.process(x.data(), int(x.size()));

    std::vector<double> out(nDelay + nNoise, 0.0);
    for (size_t i = 0; i < nNoise; ++i) out[nDelay + i] = std::clamp(double(x[i]), vMin, vMax);
    return out;
}

std::vector<double> makeWaveBurst(const std::vector<float>& samples, double rate, const PulseFeedbackParams& p, double vMin,
                                  double vMax) {
    const size_t nDelay = samplesFor(p.delayMs, rate);
    std::vector<double> out(nDelay + samples.size(), 0.0);
    for (size_t i = 0; i < samples.size(); ++i) out[nDelay + i] = std::clamp(double(samples[i]) * p.gain, vMin, vMax);
    return out;
}

double expectedNoiseRms(const PulseFeedbackParams& p, double rate) {
    if (rate <= 0 || p.highHz <= p.lowHz) return 0.0;
    return std::fabs(p.gain) * std::sqrt(std::max(0.0, p.highHz - p.lowHz) / (rate / 2));
}

}  // namespace fp

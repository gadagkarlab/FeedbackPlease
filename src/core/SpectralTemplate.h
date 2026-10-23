#pragma once
// Spectral template detection (see TemplateParams). TemplateFeatures turns the last
// windowMs of audio into a normalized band spectrum; TemplateMatcher correlates it
// with every template and decides when a template is detected.

#include <complex>
#include <vector>

#include "core/Params.h"

namespace fp {

class TemplateFeatures {
public:
    void configure(double sampleRate, const TemplateParams& tp, bool meanSubtract);
    static int windowSamplesFor(double sampleRate, const TemplateParams& tp);
    int windowSamples() const { return n_; }
    int bands() const { return bands_; }

    // window: windowSamples() samples, oldest first. Writes bands() values to shape
    // (mean removed, unit length; all 0 for silence) and returns the level: RMS between
    // lowHz and highHz in dB re 1 V.
    double compute(const float* window, float* shape);

private:
    double fs_ = 0.0;
    int n_ = 0, nfft_ = 0, bands_ = 0;
    bool meanSubtract_ = true;
    std::vector<double> win_;
    double norm_ = 1.0;  // power scaling to Vrms^2 (window sum and ENBW)
    std::vector<int> bandK0_, bandK1_;  // inclusive FFT bin range per band
    int levelK0_ = 0, levelK1_ = -1;
    std::vector<std::complex<double>> buf_;
    std::vector<double> power_;
};

struct TemplateResult {
    bool evaluated = false;  // template mode and the features were ready
    int index = -1;          // first template (list order) detected this frame that passed its gate
    int code = 0;            // its code
    double levelDb = -200.0;
    double targetScore = 0.0;  // correlation with the target template (0 when below the level)
};

struct TemplateHit {
    int index;
    bool gated;  // true = passed its timing gate (or has none)
};

class TemplateMatcher {
public:
    void reset();
    // shape: TemplateFeatures output (nullptr until ready). Scores of all templates are
    // in scores() afterwards, hits of this frame in hits().
    TemplateResult update(const float* shape, int bands, double levelDb, double tMs, const TemplateParams& tp);
    const std::vector<float>& scores() const { return scores_; }
    const std::vector<TemplateHit>& hits() const { return hits_; }

    // Normalized correlation of a frame shape with a template (0 if sizes differ).
    static double correlate(const float* shape, int bands, const std::vector<float>& tmpl);
    // Average of example shapes, normalized like a frame shape.
    static std::vector<float> average(const std::vector<std::vector<float>>& shapes);

private:
    std::vector<float> scores_;
    std::vector<int> run_;
    std::vector<double> lastMs_;
    std::vector<TemplateHit> hits_;
};

}  // namespace fp

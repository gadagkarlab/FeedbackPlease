#pragma once
// Offline analysis of whole files for tuning. Produces exactly what the real-time
// Detector would produce, frame by frame, but split into stages so that changing a
// threshold or setpoint only re-runs the cheap decision stage:
//
//   audio --(spectrum/processing params)--> EnvelopeTrack   (parallel, the slow part)
//   audio --(song params)-------------------> SongTrack
//   envelope + song --(thresholds, setpoints, feedback)--> DecisionTrack   (fast)

#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "core/Params.h"

namespace fp {

// Settings as the Detector would see them for a file at fileRate: the frame keeps
// its duration in ms (like the simulated source in the app).
AppParams paramsForRate(const AppParams& p, double fileRate);

struct EnvelopeTrack {
    double sampleRate = 0.0;
    int frameSamples = 0;
    std::vector<float> value;  // detection signal per frame (linear)
    double frameMs() const { return frameSamples * 1000.0 / sampleRate; }
    double frameEndMs(size_t k) const { return double(k + 1) * frameMs(); }
};

struct SongTrack {
    std::vector<float> ratio;
    std::vector<float> fraction;
    std::vector<uint8_t> active;
};

struct SyllableInfo {
    static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    double onsetMs = 0.0;
    double offsetMs = kNaN;  // NaN if the file ends mid-syllable
    double sdMs = kNaN;
    double ioiMs = kNaN;      // from the previous onset (NaN for the first)
    double prevIoiMs = kNaN;  // the previous syllable's IOI
    double prevSdMs = kNaN;   // the previous syllable's duration
    bool matched = false;     // the target sequence matched at this syllable
    int codeIndex = -1;       // first matching entry of the syllable code list, -1 if none
    int code = 0;             // its U8 code
};

// Template features per frame (template method): bands values per frame + level.
struct FeatureTrack {
    int bands = 0;
    std::vector<float> shape;    // frames x bands
    std::vector<float> levelDb;  // per frame; -1e9 before the first full window
    size_t frames() const { return levelDb.size(); }
    const float* at(size_t k) const { return bands > 0 && levelDb[k] > -1e8f ? &shape[k * size_t(bands)] : nullptr; }
};

struct TemplateHitInfo {
    double ms = 0.0;
    int index = -1;
    bool gated = true;
};

struct DecisionTrack {
    std::vector<uint8_t> inSyllable;  // per frame
    std::vector<SyllableInfo> syllables;
    std::vector<double> matchMs, blockedMs, suppressedMs, catchMs;
    std::vector<double> fire1Ms, fire2Ms;
    std::vector<std::vector<float>> tmplScores;  // template method: per template, per frame
    std::vector<TemplateHitInfo> tmplHits;
};

// pr must already be rate-adjusted (paramsForRate). Returns false if cancelled.
// threads = 0 uses all cores but one.
bool computeEnvelopeTrack(const std::vector<float>& x, const AppParams& pr, EnvelopeTrack& out,
                          const std::atomic<bool>* cancel = nullptr, std::atomic<float>* progress = nullptr,
                          int threads = 0);
void computeSongTrack(const std::vector<float>& x, const AppParams& pr, SongTrack& out);
// Template features of every frame (empty unless the template method is selected).
void computeFeatureTrack(const std::vector<float>& x, const AppParams& pr, FeatureTrack& out);
// The template features of the window ending at tMs (for building templates from clicks).
bool templateShapeAt(const std::vector<float>& x, const AppParams& pr, double tMs, std::vector<float>& shape,
                     double* levelDb = nullptr);
// Recomputes t.shape from its examples (each file is read from disk). p: the tuning set
// (not rate-adjusted). Examples that cannot be read are skipped; false if none could.
bool rebuildTemplateShape(SpectralTemplate& t, const AppParams& p, std::string* err = nullptr);
void runDecisions(const EnvelopeTrack& env, const SongTrack& song, const FeatureTrack* feat, const AppParams& pr,
                  DecisionTrack& out,
                  uint32_t seed = 12345);

// Fingerprints of the settings each stage depends on, to know what to recompute.
uint64_t envelopeKey(const AppParams& pr);
uint64_t songKey(const AppParams& pr);
uint64_t featureKey(const AppParams& pr);
uint64_t decisionKey(const AppParams& pr);

}  // namespace fp

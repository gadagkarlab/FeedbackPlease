#pragma once
// One call per acquired frame (default 40 samples = 1 ms): updates the sliding
// window, computes the band envelope, segments syllables, checks the target
// sequence and steps the feedback state machine. Equivalent to one iteration of
// the LabVIEW acquisition loop calling DetectSyllables.vi. No I/O happens here.

#include <cstdint>
#include <vector>

#include "core/BandEnvelope.h"
#include "core/FeedbackStateMachine.h"
#include "core/Params.h"
#include "core/SongDetector.h"
#include "core/SpectralTemplate.h"
#include "core/SyllableCodes.h"
#include "core/Syllables.h"

namespace fp {

struct FrameResult {
    uint64_t sampleEnd = 0;  // total samples processed including this frame
    double tMs = 0.0;        // time of the end of this frame since start
    bool ready = false;      // false until the first full window has been collected
    double value = 0.0;      // detection signal (linear)
    double displayValue = 0.0;
    double includeAmp = 0.0;
    double subtractAmp = 0.0;
    SegmenterOutput seg;
    MatchResult match;      // the target syllable was recognized
    SylCodeResult sylCode;  // syllable classification at each IOI end (sequence method)
    TemplateResult tmpl;    // spectral template detection (template method)
    SongState song;
    bool songGateOpen = true;  // matches may start feedback trials
    bool matchBlocked = false; // a match happened but no song was detected
    FeedbackOutput fb;
    FbState fbState = FbState::Listening;
};

class Detector {
public:
    explicit Detector(uint32_t seed = 12345);

    // Live parameter update. Detection state is kept unless the sample rate or
    // window length changed, in which case the window buffer is refilled.
    void setParams(const AppParams& p);
    void reset();
    FrameResult process(const float* x, int n);

    // The decision half of process(): segmentation, sequence match, song gate and
    // feedback state machine, given an already computed detection value and song
    // state. Used to replay precomputed envelopes when tuning offline.
    // shape / levelDb: TemplateFeatures output for the template method (nullptr if not ready).
    FrameResult decide(uint64_t sampleEnd, double value, const SongState& song, double dtMs,
                       const float* shape = nullptr, double levelDb = -200.0);
    const TemplateMatcher& templateMatcher() const { return matcher_; }

    static int windowSamplesFor(const AppParams& p);

    const AppParams& params() const { return p_; }
    int windowSamples() const { return windowN_; }
    const SongDetector& songDetector() const { return song_; }

    // Converts a linear detection value to what the GUI shows (dB if enabled).
    static double toDisplay(double v, bool dB);

private:
    AppParams p_;
    BandEnvelope env_;
    SyllableSegmenter seg_;
    SyllableClassifier classifier_;
    SongDetector song_;
    FeedbackStateMachine fsm_;
    TemplateFeatures feat_;
    TemplateMatcher matcher_;
    std::vector<float> shape_;

    int windowN_ = 0;
    int ringN_ = 0;  // longest window (detection or template)
    std::vector<float> ring_;  // 2*ringN_, each sample written twice so any window is contiguous
    int head_ = 0;
    int filled_ = 0;
    uint64_t samples_ = 0;
};

}  // namespace fp

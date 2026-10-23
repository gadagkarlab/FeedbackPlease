#pragma once
// SyllableSegmenter: port of "Syllable and IOI Time F(x)" in DetectSyllables.vi.
// (CalcDetectSignal.vi's matching now lives in SyllableClassifier.)

#include "core/Params.h"

namespace fp {

struct SegmenterOutput {
    bool inSyllable = false;  // includes a gap that may still turn out shorter than the minimum
    bool onset = false;   // this frame crossed above the start threshold (after a real gap)
    bool offset = false;  // a syllable end was confirmed this frame (see offsetMs)
    bool newIOI = false;  // onset with a previous onset to measure from
    bool newSD = false;   // same as offset
    double ioiMs = 0.0;   // valid when newIOI
    double sdMs = 0.0;    // valid when newSD
    double offsetMs = 0.0;  // valid when offset: when the signal dropped below the end threshold
                            // (up to minGapMs before this frame)
};

// Hysteresis segmentation: a syllable starts when the envelope rises above the
// start threshold and ends when it falls below the (lower) end threshold.
// Gaps shorter than minGapMs are bridged: a syllable, a tiny gap and another syllable
// count as one (one onset, IOI and SD measured across them). A drop below the end
// threshold therefore only becomes an offset once the gap has lasted minGapMs; the
// offset time and SD still use the moment of the drop. Onsets are never delayed.
class SyllableSegmenter {
public:
    void reset();
    SegmenterOutput update(double value, double tMs, double startThresh, double endThresh, double minGapMs = 0.0);
    bool inSyllable() const { return inSyl_; }

private:
    bool inSyl_ = false;
    bool inGap_ = false;      // below the end threshold, but not yet for minGapMs
    double gapStartMs_ = 0.0;
    bool haveOnset_ = false;
    double onsetMs_ = 0.0;
};

// Whether the target syllable was recognized this frame (see SyllableClassifier).
struct MatchResult {
    bool evaluated = false;  // an IOI ended and the syllable list was checked
    bool match = false;      // the first matching syllable is the feedback target
};

}  // namespace fp

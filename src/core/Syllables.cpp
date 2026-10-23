#include "core/Syllables.h"

#include <cmath>

namespace fp {

void SyllableSegmenter::reset() {
    inSyl_ = false;
    inGap_ = false;
    gapStartMs_ = 0.0;
    haveOnset_ = false;
    onsetMs_ = 0.0;
}

SegmenterOutput SyllableSegmenter::update(double value, double tMs, double startThresh, double endThresh,
                                          double minGapMs) {
    SegmenterOutput o;
    auto endSyllable = [&](double atMs) {
        inSyl_ = false;
        inGap_ = false;
        o.offset = true;
        o.newSD = true;
        o.offsetMs = atMs;
        o.sdMs = atMs - onsetMs_;
    };
    if (!inSyl_) {
        if (value > startThresh) {
            inSyl_ = true;
            o.onset = true;
            if (haveOnset_) {
                o.newIOI = true;
                o.ioiMs = tMs - onsetMs_;
            }
            haveOnset_ = true;
            onsetMs_ = tMs;
        }
    } else if (!inGap_) {
        if (value < endThresh) {
            if (minGapMs > 0.0) {
                inGap_ = true;  // maybe the end, maybe a gap to bridge
                gapStartMs_ = tMs;
            } else {
                endSyllable(tMs);
            }
        }
    } else if (value > startThresh) {
        inGap_ = false;  // back within minGapMs: same syllable, no new onset
    } else if (tMs - gapStartMs_ >= minGapMs) {
        endSyllable(gapStartMs_);  // the gap is real: the syllable ended when it dropped
    }
    o.inSyllable = inSyl_;
    return o;
}

}  // namespace fp

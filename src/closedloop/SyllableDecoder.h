#ifndef CLOSEDLOOP_SYLLABLEDECODER_H
#define CLOSEDLOOP_SYLLABLEDECODER_H

// Turns the per-sample syllable code (the niSyllableLines bits, see
// DigitalWordUtils.h extractField) into one onset per syllable pulse, by
// rising edge rather than pulse duration:
//
//   onset   : a nonzero code held for attackSamples consecutive samples
//             (niDebounceSamples). The onset time is the first of those
//             samples; the code is the one that held.
//   re-arm  : after an onset, nothing fires again until the code has been 0
//             continuously for rearmSamples (niRearmMs) -- so dropouts or
//             bounces inside a pulse, and blips as it falls, can't retrigger.
//   refractory: onsets are additionally at least refractorySamples apart
//             (niRefractoryMs), a safety net for edges that bounce longer
//             than the re-arm time.
//
// One onset per pulse: a code change while the lines stay high (no return
// to 0) is treated as the same pulse.
//
// Fed one sample at a time, across fetch boundaries; no SpikeGLX calls, so
// it can be tested offline.
class SyllableDecoder {
public:
    SyllableDecoder( int attackSamples, long long rearmSamples, long long refractorySamples )
        :   attack_( attackSamples < 1 ? 1 : attackSamples ),
            rearm_( rearmSamples < 1 ? 1 : rearmSamples ),
            refractory_( refractorySamples < 0 ? 0 : refractorySamples ) {}

    // Returns true (and fills code/onsetIdx) when sample `idx` completes an onset.
    bool push( int code, long long idx, int &onsetCode, long long &onsetIdx )
    {
        if( code == runCode_ )
            ++runLen_;
        else {
            runCode_ = code; runLen_ = 1; runStart_ = idx;
        }

        if( runCode_ == 0 ) {
            if( !armed_ && runLen_ >= rearm_ )
                armed_ = true;
            return false;
        }

        if( armed_ && runLen_ == attack_ &&
            ( lastOnset_ < 0 || runStart_ - lastOnset_ >= refractory_ ) ) {
            armed_     = false;
            lastOnset_ = runStart_;
            onsetCode  = runCode_;
            onsetIdx   = runStart_;
            return true;
        }
        return false;
    }

private:
    int       attack_;
    long long rearm_, refractory_;
    int       runCode_   = 0;
    long long runLen_    = 0;
    long long runStart_  = 0;
    bool      armed_     = false;   // needs a full rest period first (don't fire mid-pulse at startup)
    long long lastOnset_ = -1;
};

#endif // CLOSEDLOOP_SYLLABLEDECODER_H

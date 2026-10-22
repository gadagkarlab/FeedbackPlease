#ifndef CLOSEDLOOP_DECISIONTHREAD_H
#define CLOSEDLOOP_DECISIONTHREAD_H

#include <string>
#include <deque>
#include <vector>
#include <thread>
#include <atomic>
#include <fstream>
#include <chrono>
#include <algorithm>

#include "ThreadSafeQueue.h"
#include "Events.h"
#include "Stats.h"

// Owns exactly one SpikeGLX handle (hDO), dedicated solely to
// sglx_ni_DO_set calls -- see README.md's concurrency rules for why this
// needs its own handle rather than sharing hIM or hNI.
//
// All timing is "ms relative to syllable onset", computed as
// (spike IMEC sample - syllable onset IMEC sample) / imecSampleRate -- the
// onset was mapped into the IMEC stream by NiFetchThread (sglx_mapSample),
// so both numbers are on the same clock and no sync-edge reference is
// involved.
//
// Only onsets of targetSyllable (a code from NiFetchThread; -1 = any code)
// open a counting window -- other codes are ignored here (they're still in
// syllableTimesPath).
//
// Sub-windows (all times relative to syllable onset):
//   sub-window k = [windowStart + k*step, windowStart + k*step + subWinDur],
//   step = subWinDur - subWinOverlap, k = 0 .. numSubWin-1.
// The whole window spans [windowStart, end of the last sub-window].
//
// Two mutually exclusive decision modes (decisionMode):
//   PushDown: fires when the spike count across any two consecutive
//     sub-windows k, k+1 (their union -- a spike in the overlap counts once)
//     reaches >= spikeCountThreshold. Since that union contains sub-window
//     k, a single sub-window reaching threshold also fires; with
//     numSubWin == 1 it is a plain single window. Decided incrementally:
//     the line rises the instant the qualifying spike arrives.
//   PushUp: fires when EVERY pair of consecutive sub-windows has
//     <= spikeCountThreshold spikes, i.e. the max pair count is <= the
//     threshold. The last pair ends with the window, so it's decided when
//     IMEC detection is final through the window's last sample (see
//     imecDecidedThrough below) -- a few ms after the window ends.
//
// Both modes use the same statistic: the max pair count (spikes in the
// busiest pair of consecutive sub-windows, overlap spikes once). It's what
// the threshold is compared against, what's logged as `count` (the
// whole-window total is logged as `total_count`), and what the adaptive
// threshold tracks.
//
// Adaptive threshold (adaptiveRenditions = N > 0): the threshold starts at
// the configured value and stays there for the first N completed target-
// syllable renditions; after each later one it becomes the median of the
// last N renditions' max pair count. Each rendition uses the
// threshold in effect at its onset. Stalled windows don't enter the history.
// N = 0 keeps the threshold fixed.
//
// Window closing is sample-based in both modes: a syllable's window is
// complete once imecDecidedThrough (published by ImecFetchThread) reaches
// its last sample, i.e. no spike inside it can still arrive. A wall-clock
// timeout only covers a stalled IMEC stream (window closed undecided).
//
// This is the module to change for any different decision policy: all of
// it lives in applySpike()/closeFinishedWindows() below.
class DecisionThread {
public:
    enum class Mode { PushDown, PushUp };

    DecisionThread( void *hSglx, ThreadSafeQueue<SpikeEvent> &spikeQueue,
                     ThreadSafeQueue<SyllableEvent> &syllableQueue,
                     const std::atomic<long long> &imecDecidedThrough,
                     double imecSampleRate,
                     int targetSyllable,
                     double windowStartS, double subWinDurS, double subWinOverlapS,
                     int numSubWin, Mode mode,
                     double initialThreshold, int adaptiveRenditions,
                     const std::string &doLine, int doPulseMs,
                     const std::string &decisionLogPath );

    // Called from this thread for every finished window (before start()).
    void setObserver( DecisionObserver o ) { observer_ = std::move( o ); }

    void start();
    void stop();
    void join();

    static const char *modeName( Mode m ) { return m == Mode::PushUp ? "push_up" : "push_down"; }

private:
    struct PendingSyllable {
        SyllableEvent event;
        long long windowEndSample;          // IMEC sample of the window's last instant
        double threshold;                   // threshold in effect at this rendition's onset
        int  count;                         // unique spikes in the whole span (all sub-windows)
        std::vector<int> subCounts;         // per sub-window (an overlap spike appears in both)
        std::vector<int> pairCounts;        // per consecutive pair k,k+1 (overlap spike counted once)
        bool triggered;
        int  triggerPair;                   // push_down only: pair k = sub-windows k,k+1
        double triggerSpikeMs;              // push_down only
        double pulseMs;                     // onset -> pulse on the IMEC clock; NaN until triggered
        std::vector<double> spikeTimesMs;   // every spike in [logStart, windowEnd], rel. onset
        std::chrono::steady_clock::time_point receivedAt;
    };

    struct BufferedSpike {
        SpikeEvent event;
        std::chrono::steady_clock::time_point receivedAt;
    };

    void runLoop();

    // The swappable decision policy -- see class comment.
    void onSpikeEvent( const SpikeEvent &spk );
    void onSyllableEvent( const SyllableEvent &syl );
    void applySpike( PendingSyllable &p, const SpikeEvent &spk );
    void closeFinishedWindows( long long decidedThrough );
    void logSyllable( const PendingSyllable &p, const char *closedBy );
    // Spikes in the busiest pair of consecutive sub-windows -- the decision
    // statistic for both modes (see class comment).
    static int maxPairCount( const PendingSyllable &p )
    { return p.pairCounts.empty() ? 0 : *std::max_element( p.pairCounts.begin(), p.pairCounts.end() ); }
    void updateAdaptiveThreshold( const PendingSyllable &p );
    // Returns the IMEC stream sample count read right after the line was
    // raised -- the pulse's time on the same clock as onsets and spikes.
    // *preDoSample (if given) gets the count read just before the DO call,
    // so decision latency and DO round-trip time can be told apart.
    long long raiseLine( long long *preDoSample = nullptr );
    void lowerLineIfDue();

    void *hSglx_;
    ThreadSafeQueue<SpikeEvent>    &spikeQueue_;
    ThreadSafeQueue<SyllableEvent> &syllableQueue_;
    const std::atomic<long long>   &imecDecidedThrough_;

    double imecSampleRate_;
    int    targetSyllable_;        // -1 = every nonzero code opens a window
    double windowStartS_;
    double subWinDurS_, stepS_;
    int    numSubWin_, numPairs_;   // numPairs_ = max(1, numSubWin_-1)
    double windowEndS_;            // end of the last sub-window
    double logStartS_;     // min(0, windowStartS_): log every spike from onset (or window start, if earlier)
    Mode   mode_;
    double threshold_;             // current threshold (fixed, or the rolling median)
    int    adaptiveRenditions_;    // 0 = fixed threshold
    std::deque<int> statHistory_;  // last adaptiveRenditions_ renditions' decision statistic
    std::string doLine_;
    int    doPulseMs_;

    std::deque<PendingSyllable> pending_;

    // Recently received spikes, so a spike that reaches this thread just
    // before its syllable's event (the two fetch threads run independently)
    // is still counted and logged when that syllable arrives.
    std::deque<BufferedSpike> recentSpikes_;

    bool lineHigh_;
    std::chrono::steady_clock::time_point lineHighUntil_;

    std::ofstream decisionLog_;
    DecisionObserver observer_;

    std::atomic<bool> stopFlag_;
    std::thread        thread_;
};

#endif // CLOSEDLOOP_DECISIONTHREAD_H

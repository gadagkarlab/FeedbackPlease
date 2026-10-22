#ifndef CLOSEDLOOP_STATS_H
#define CLOSEDLOOP_STATS_H

#include <atomic>
#include <functional>
#include <vector>

// Optional hooks the threads use to report to a host UI. All are write-only from
// the threads' point of view and cost one relaxed atomic add / one callback per
// event, so the real-time path is unchanged when nobody reads them.

struct LiveStats {
    std::atomic<long long> spikes;          // detections above threshold (ImecFetchThread)
    std::atomic<long long> imecSample;      // newest IMEC sample fetched
    std::atomic<long long> syllables;       // syllable onsets mapped into the IMEC stream (NiFetchThread)
    std::atomic<long long> syllablesLost;   // onsets dropped because sglx_mapSample failed
    LiveStats() : spikes( 0 ), imecSample( 0 ), syllables( 0 ), syllablesLost( 0 ) {}
};

// One finished decision window, reported by DecisionThread from its own thread.
struct DecisionRecord {
    int                 code;               // syllable code that opened the window
    long long           imecOnsetSample;
    int                 count;              // max spikes in a pair of neighbouring sub-windows
    int                 totalCount;         // whole window, each spike once
    double              threshold;          // threshold in effect at this onset
    bool                triggered;          // line was raised
    bool                push_up;            // decision mode (false = push_down)
    bool                stalled;            // closed undecided: IMEC stream stalled
    double              pulseMs;            // onset -> pulse (IMEC clock); only if triggered
    std::vector<double> spikesMs;           // spike times relative to onset, sorted
};

typedef std::function<void( const DecisionRecord & )> DecisionObserver;

#endif // CLOSEDLOOP_STATS_H

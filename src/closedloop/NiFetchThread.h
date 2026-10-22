#ifndef CLOSEDLOOP_NIFETCHTHREAD_H
#define CLOSEDLOOP_NIFETCHTHREAD_H

#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include "ThreadSafeQueue.h"
#include "Events.h"
#include "Stats.h"

// Owns exactly one SpikeGLX handle (hNI) -- see README.md's concurrency
// rules. Continuously fetches the NI digital-word (DW) channel, decodes a
// debounced 3-bit syllable code from `syllableLines` (assumed contiguous
// ascending bits, e.g. {5,6,7} -- see decode logic in the .cpp), and
// reports one SyllableEvent per debounced code *onset* (not continuously
// while a code is held -- see fetchLoop()'s debounce/re-arm logic in the
// .cpp for exactly how a repeated syllable after a return to code 0 is
// treated as a fresh event). Each onset is mapped into the IMEC stream via
// sglx_mapSample before being queued. Every emitted event is also appended
// to syllableTimesPath (one "code,niSampleIndex,imecSampleIndex" line per
// event).
class NiFetchThread {
public:
    // syllableTimesPath: pass "" to skip writing this file (queue-only).
    // debounceSamples / rearmMs / refractoryMs: onset detection, see SyllableDecoder.h.
    NiFetchThread( void *hSglx, const std::vector<int> &syllableLines,
                   int debounceSamples, double rearmMs, double refractoryMs,
                   int fetchChunkMs,
                   ThreadSafeQueue<SyllableEvent> &syllableQueue,
                   const std::string &syllableTimesPath = "" );

    // Optional, before start(): stop the whole app (stopAll, streamLost set)
    // if this stream delivers no data for timeoutS seconds -- see FetchHealth.h.
    void watchStream( double timeoutS, std::atomic<bool> &stopAll, std::atomic<bool> &streamLost )
    { streamTimeoutS_ = timeoutS; stopAll_ = &stopAll; streamLost_ = &streamLost; }

    void setStats( LiveStats *stats ) { stats_ = stats; }

    void start();
    void stop();
    void join();

private:
    void fetchLoop();

    void *hSglx_;
    std::vector<int> syllableLines_;
    int    debounceSamples_;
    double rearmMs_, refractoryMs_;
    int    fetchChunkMs_;
    ThreadSafeQueue<SyllableEvent> &syllableQueue_;
    std::string syllableTimesPath_;

    LiveStats *stats_ = nullptr;

    std::atomic<bool> stopFlag_;
    double             streamTimeoutS_ = 0.0;
    std::atomic<bool> *stopAll_    = nullptr;
    std::atomic<bool> *streamLost_ = nullptr;
    std::thread        thread_;
};

#endif // CLOSEDLOOP_NIFETCHTHREAD_H

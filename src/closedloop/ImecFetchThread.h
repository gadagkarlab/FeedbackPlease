#ifndef CLOSEDLOOP_IMECFETCHTHREAD_H
#define CLOSEDLOOP_IMECFETCHTHREAD_H

#include <string>
#include <thread>
#include <atomic>
#include <fstream>

#include "FilterBank.h"
#include "ConvolutionEngine.h"
#include "ThreadSafeQueue.h"
#include "Events.h"
#include "Stats.h"

// Owns exactly one SpikeGLX handle (hIM) -- per the concurrency rules in
// README.md, no other thread may touch this handle. Continuously fetches
// the FULL CAR channel group (see Preprocessor.h -- must match the group
// the filter was trained with, not just its own 5 channels) in small
// chunks (via sglx_fetch, following the exact continuous-polling pattern
// in SDK/CPP/DemoRemoteAPI.cpp's latency_test()), runs highpass+CAR across
// that full group, subsets down to the filter's channels, runs
// ConvolutionEngine, and reports threshold-crossing peaks as SpikeEvents
// (absolute IMEC sample index) to DecisionThread, plus writes every
// detection's sample index to spikeTimesPath.
//
// To change the fetching mechanism itself (e.g. switch to
// sglx_fetchLatest, change what's included in the channel subset): this
// class's fetchLoop() is the only place that matters.
class ImecFetchThread {
public:
    // carChannelIds: the CAR channel group the filter was trained with,
    // resolved at startup from the training .meta's ~snsGeomMap by shank
    // (see SglxMeta::shankChannels). Pass an empty vector to disable CAR
    // (fetches just the filter's own channels, matching --no-car).
    ImecFetchThread( void *hSglx, FilterBank &filterBank,
                      const std::vector<int> &carChannelIds,
                      bool applyHighpass, double highpassCutoffHz,
                      int fetchChunkMs,
                      ThreadSafeQueue<SpikeEvent> &spikeQueue,
                      std::atomic<long long> &decidedThrough,
                      const std::string &spikeTimesPath );

    // Optional, before start(): stop the whole app (stopAll, streamLost set)
    // if this stream delivers no data for timeoutS seconds -- see FetchHealth.h.
    void watchStream( double timeoutS, std::atomic<bool> &stopAll, std::atomic<bool> &streamLost )
    { streamTimeoutS_ = timeoutS; stopAll_ = &stopAll; streamLost_ = &streamLost; }

    void setStats( LiveStats *stats ) { stats_ = stats; }

    void start();
    void stop();  // signals the loop to exit; does not join
    void join();

private:
    void fetchLoop();

    void   *hSglx_;
    FilterBank &filterBank_;   // threshold read each iteration -- Calibration may have just set it
    std::vector<int> carChannelIds_;
    bool    applyHighpass_;
    double  highpassCutoffHz_;
    int     fetchChunkMs_;
    ThreadSafeQueue<SpikeEvent> &spikeQueue_;
    // IMEC sample index through which detection is final (ConvolutionEngine::
    // decidedThrough()), stored *after* that chunk's spikes are pushed -- so a
    // reader that loads it, then drains spikeQueue_, has every spike <= it.
    std::atomic<long long> &decidedThrough_;
    std::string spikeTimesPath_;

    LiveStats *stats_ = nullptr;

    std::atomic<bool> stopFlag_;
    double             streamTimeoutS_ = 0.0;
    std::atomic<bool> *stopAll_    = nullptr;
    std::atomic<bool> *streamLost_ = nullptr;
    std::thread        thread_;
};

#endif // CLOSEDLOOP_IMECFETCHTHREAD_H

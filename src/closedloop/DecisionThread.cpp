#include "DecisionThread.h"
#include "Log.h"

#include <iostream>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "SglxCppClient.h"

namespace {
    // sglx_ni_DO_set( h, lines, bits ): `lines` picks which lines are written
    // (all others are untouched); `bits` is a device-wide pattern where bit
    // 8*port + line gives that line's value (SglxApi.h). So bit 0 is
    // port0/line0's slot -- the old value 0x1 left e.g. port1/line5 LOW.
    // All-ones sets every listed line high whether SpikeGLX reads `bits` by
    // absolute position or relative to `lines`; `lines` (= doLine, one line)
    // keeps it to that line.
    const unsigned int kLineHighBits = 0xFFFFFFFFu;
    const unsigned int kLineLowBits  = 0x0;

    // How long (wall clock) to keep received spikes around for syllables
    // that arrive after them. Only needs to exceed the difference between
    // the NI and IMEC fetch latencies (a few ms); generous margin here.
    const std::chrono::milliseconds kSpikeBufferMs( 500 );

    // Windows normally close on IMEC detection progress (see
    // closeFinishedWindows()). This wall-clock limit past the window's end
    // only catches a stalled IMEC stream -- such a window is closed
    // undecided (never fires push_up), with a warning.
    const std::chrono::milliseconds kStalledWindowMs( 2000 );
}


DecisionThread::DecisionThread( void *hSglx, ThreadSafeQueue<SpikeEvent> &spikeQueue,
                                 ThreadSafeQueue<SyllableEvent> &syllableQueue,
                                 const std::atomic<long long> &imecDecidedThrough,
                                 double imecSampleRate,
                                 int targetSyllable,
                                 double windowStartS, double subWinDurS, double subWinOverlapS,
                                 int numSubWin, Mode mode,
                                 double initialThreshold, int adaptiveRenditions,
                                 const std::string &doLine, int doPulseMs,
                                 const std::string &decisionLogPath )
    :   hSglx_( hSglx ), spikeQueue_( spikeQueue ), syllableQueue_( syllableQueue ),
        imecDecidedThrough_( imecDecidedThrough ),
        imecSampleRate_( imecSampleRate ), targetSyllable_( targetSyllable ),
        windowStartS_( windowStartS ), subWinDurS_( subWinDurS ),
        stepS_( subWinDurS - subWinOverlapS ),
        numSubWin_( numSubWin ), numPairs_( std::max( 1, numSubWin - 1 ) ),
        windowEndS_( windowStartS + ( numSubWin - 1 ) * ( subWinDurS - subWinOverlapS ) + subWinDurS ),
        logStartS_( std::min( 0.0, windowStartS ) ),
        mode_( mode ),
        threshold_( initialThreshold ), adaptiveRenditions_( adaptiveRenditions ),
        doLine_( doLine ), doPulseMs_( doPulseMs ),
        lineHigh_( false ), stopFlag_( false )
{
    if( subWinDurS_ <= 0.0 )
        throw std::runtime_error( "DecisionThread: subWinDurMs must be > 0" );
    if( subWinOverlapS < 0.0 || stepS_ <= 0.0 )
        throw std::runtime_error( "DecisionThread: subWinOverlapMs must be >= 0 and < subWinDurMs" );
    if( numSubWin_ < 1 )
        throw std::runtime_error( "DecisionThread: numSubWin must be >= 1" );
    if( adaptiveRenditions_ < 0 )
        throw std::runtime_error( "DecisionThread: adaptiveRenditions must be >= 0" );

    if( !decisionLogPath.empty() )
        decisionLog_.open( decisionLogPath.c_str(), std::ios::app );
}


void DecisionThread::start() { thread_ = std::thread( &DecisionThread::runLoop, this ); }
void DecisionThread::stop()  { stopFlag_.store( true ); }
void DecisionThread::join()  { if( thread_.joinable() ) thread_.join(); }


void DecisionThread::runLoop()
{
    // Ensure the line starts low.
    sglx_ni_DO_set( hSglx_, doLine_, kLineLowBits );

    while( !stopFlag_.load() ) {

        // Load detection progress BEFORE draining the spike queue:
        // ImecFetchThread pushes a chunk's spikes before publishing its
        // progress, so every spike <= `decided` is already queued now.
        long long decided = imecDecidedThrough_.load( std::memory_order_acquire );

        SpikeEvent spk;
        if( spikeQueue_.waitPop( spk, 1 ) ) {
            onSpikeEvent( spk );
            while( spikeQueue_.waitPop( spk, 0 ) )
                onSpikeEvent( spk );
        }

        SyllableEvent syl;
        while( syllableQueue_.waitPop( syl, 0 ) )
            onSyllableEvent( syl );

        closeFinishedWindows( decided );
        lowerLineIfDue();
    }

    sglx_ni_DO_set( hSglx_, doLine_, kLineLowBits );

    // Log whatever was still open at shutdown, so every syllable gets a line.
    for( size_t i = 0; i < pending_.size(); ++i )
        logSyllable( pending_[i], "shutdown" );
    pending_.clear();
}


void DecisionThread::onSyllableEvent( const SyllableEvent &syl )
{
    if( targetSyllable_ >= 0 && syl.code != targetSyllable_ )
        return;

    PendingSyllable p;
    p.event           = syl;
    p.windowEndSample = syl.imecSampleIndex +
                        static_cast<long long>( std::ceil( windowEndS_ * imecSampleRate_ ) );
    p.threshold       = threshold_;
    p.count           = 0;
    p.subCounts.assign( numSubWin_, 0 );
    p.pairCounts.assign( numPairs_, 0 );
    p.triggered       = false;
    p.triggerPair     = -1;
    p.triggerSpikeMs  = 0.0;
    p.pulseMs         = std::nan( "" );
    p.receivedAt      = std::chrono::steady_clock::now();
    pending_.push_back( p );

    // Spikes that arrived before this syllable's event did.
    for( size_t i = 0; i < recentSpikes_.size(); ++i )
        applySpike( pending_.back(), recentSpikes_[i].event );
}


void DecisionThread::onSpikeEvent( const SpikeEvent &spk )
{
    BufferedSpike b;
    b.event      = spk;
    b.receivedAt = std::chrono::steady_clock::now();
    recentSpikes_.push_back( b );

    for( size_t i = 0; i < pending_.size(); ++i )
        applySpike( pending_[i], spk );
}


void DecisionThread::applySpike( PendingSyllable &p, const SpikeEvent &spk )
{
    // Both indices are IMEC-stream samples -- see class comment.
    double dtS = static_cast<double>( spk.sampleIndex - p.event.imecSampleIndex ) / imecSampleRate_;

    if( dtS < logStartS_ || dtS > windowEndS_ )
        return;

    double dtMs = dtS * 1000.0;
    p.spikeTimesMs.push_back( dtMs );

    if( dtS < windowStartS_ )
        return; // logged, but before the counting window

    ++p.count;

    // Per sub-window (diagnostic only -- an overlap spike lands in both).
    for( int k = 0; k < numSubWin_; ++k ) {
        double s = windowStartS_ + k * stepS_;
        if( dtS >= s && dtS <= s + subWinDurS_ )
            ++p.subCounts[k];
    }

    // Per consecutive pair: pair k spans the union of sub-windows k and k+1
    // (one contiguous interval, so an overlap spike is counted once). With a
    // single sub-window, pair 0 is just sub-window 0.
    for( int k = 0; k < numPairs_; ++k ) {

        double s = windowStartS_ + k * stepS_;
        double e = windowStartS_ + std::min( k + 1, numSubWin_ - 1 ) * stepS_ + subWinDurS_;
        if( dtS < s || dtS > e )
            continue;

        ++p.pairCounts[k];

        // push_down fires here, the instant a pair reaches threshold;
        // push_up is decided only once the whole window is over.
        if( mode_ == Mode::PushDown && !p.triggered && p.pairCounts[k] >= p.threshold ) {
            long long preDoSample = 0;
            long long pulseSample = raiseLine( &preDoSample );
            p.triggered      = true;
            p.triggerPair    = k;
            p.triggerSpikeMs = dtMs;
            p.pulseMs        = ( pulseSample - p.event.imecSampleIndex ) * 1000.0 / imecSampleRate_;

            if( decisionLog_.is_open() ) {
                decisionLog_ << "TRIGGER mode=push_down code=" << p.event.code
                             << " imec_onset_sample=" << p.event.imecSampleIndex
                             << " pair=" << k
                             << " pair_ms=" << s * 1000.0 << "-" << e * 1000.0
                             << " pair_count=" << p.pairCounts[k]
                             << " threshold=" << p.threshold
                             << " trigger_spike_ms=" << dtMs
                             << " decision_imec_sample=" << preDoSample
                             << " pulse_imec_sample=" << pulseSample
                             << " pulse_ms=" << ( pulseSample - p.event.imecSampleIndex ) * 1000.0 / imecSampleRate_
                             << "\n";
                decisionLog_.flush();
            }
        }
    }
}


void DecisionThread::closeFinishedWindows( long long decidedThrough )
{
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::chrono::milliseconds stalledAfter(
        static_cast<long long>( windowEndS_ * 1000.0 ) + kStalledWindowMs.count() );

    for( std::deque<PendingSyllable>::iterator it = pending_.begin(); it != pending_.end(); ) {

        bool complete = decidedThrough >= it->windowEndSample;
        bool stalled  = !complete && now - it->receivedAt >= stalledAfter;

        if( !complete && !stalled ) {
            ++it;
            continue;
        }

        // push_up: every pair of consecutive sub-windows at or below threshold.
        if( complete && mode_ == Mode::PushUp && maxPairCount( *it ) <= it->threshold ) {
            long long preDoSample = 0;
            long long pulseSample = raiseLine( &preDoSample );
            it->triggered = true;
            it->pulseMs   = ( pulseSample - it->event.imecSampleIndex ) * 1000.0 / imecSampleRate_;

            if( decisionLog_.is_open() ) {
                decisionLog_ << "TRIGGER mode=push_up code=" << it->event.code
                             << " imec_onset_sample=" << it->event.imecSampleIndex
                             << " window_ms=" << windowStartS_ * 1000.0 << "-" << windowEndS_ * 1000.0
                             << " count=" << maxPairCount( *it )
                             << " total_count=" << it->count
                             << " threshold=" << it->threshold
                             << " decision_imec_sample=" << preDoSample
                             << " pulse_imec_sample=" << pulseSample
                             << " pulse_ms=" << ( pulseSample - it->event.imecSampleIndex ) * 1000.0 / imecSampleRate_
                             << "\n";
                decisionLog_.flush();
            }
        }

        if( stalled )
            ClLog() << "DecisionThread: IMEC detection hasn't reached the end of the window for "
                         "syllable at IMEC sample " << it->event.imecSampleIndex
                      << " -- closing it undecided\n";

        logSyllable( *it, complete ? "complete" : "stalled" );
        if( complete )
            updateAdaptiveThreshold( *it );
        it = pending_.erase( it );
    }

    while( !recentSpikes_.empty() && now - recentSpikes_.front().receivedAt > kSpikeBufferMs )
        recentSpikes_.pop_front();
}


void DecisionThread::logSyllable( const PendingSyllable &p, const char *closedBy )
{
    std::vector<double> times( p.spikeTimesMs );
    std::sort( times.begin(), times.end() );

    if( observer_ && std::string( closedBy ) != "shutdown" ) {
        DecisionRecord r;
        r.code            = p.event.code;
        r.imecOnsetSample = p.event.imecSampleIndex;
        r.count           = maxPairCount( p );
        r.totalCount      = p.count;
        r.threshold       = p.threshold;
        r.triggered       = p.triggered;
        r.push_up         = mode_ == Mode::PushUp;
        r.stalled         = std::string( closedBy ) == "stalled";
        r.pulseMs         = p.pulseMs;
        r.spikesMs        = times;
        observer_( r );
    }

    if( !decisionLog_.is_open() )
        return;

    decisionLog_ << "SYLLABLE mode=" << modeName( mode_ )
                 << " code=" << p.event.code
                 << " ni_onset_sample=" << p.event.sampleIndex
                 << " imec_onset_sample=" << p.event.imecSampleIndex
                 << " count=" << maxPairCount( p )         // max spikes in a pair of neighbouring sub-windows
                 << " total_count=" << p.count             // whole window, each spike once
                 << " threshold=" << p.threshold
                 << " triggered=" << ( p.triggered ? 1 : 0 )
                 << " closed=" << closedBy;
    if( p.triggered && mode_ == Mode::PushDown )
        decisionLog_ << " trigger_pair=" << p.triggerPair
                     << " trigger_spike_ms=" << p.triggerSpikeMs;
    decisionLog_ << " sub_counts=";
    for( size_t i = 0; i < p.subCounts.size(); ++i )
        decisionLog_ << ( i ? "," : "" ) << p.subCounts[i];
    decisionLog_ << " pair_counts=";
    for( size_t i = 0; i < p.pairCounts.size(); ++i )
        decisionLog_ << ( i ? "," : "" ) << p.pairCounts[i];
    decisionLog_ << " spikes_ms=";
    for( size_t i = 0; i < times.size(); ++i )
        decisionLog_ << ( i ? "," : "" ) << times[i];
    decisionLog_ << "\n";
    decisionLog_.flush();
}


void DecisionThread::updateAdaptiveThreshold( const PendingSyllable &p )
{
    if( adaptiveRenditions_ <= 0 )
        return;

    // The same statistic the threshold is compared against (both modes).
    int stat = maxPairCount( p );

    statHistory_.push_back( stat );
    if( static_cast<int>( statHistory_.size() ) > adaptiveRenditions_ )
        statHistory_.pop_front();
    if( static_cast<int>( statHistory_.size() ) < adaptiveRenditions_ )
        return; // still warming up -- keep the configured threshold

    std::vector<int> v( statHistory_.begin(), statHistory_.end() );
    std::sort( v.begin(), v.end() );
    size_t n      = v.size();
    double median = ( n % 2 ) ? v[n / 2] : 0.5 * ( v[n / 2 - 1] + v[n / 2] );

    if( median != threshold_ ) {
        if( decisionLog_.is_open() ) {
            decisionLog_ << "THRESHOLD old=" << threshold_ << " new=" << median
                         << " (median of last " << n << " renditions)\n";
            decisionLog_.flush();
        }
        ClLog() << "Adaptive threshold: " << threshold_ << " -> " << median << "\n";
        threshold_ = median;
    }
}


long long DecisionThread::raiseLine( long long *preDoSample )
{
    // Stream position when the decision was made, before the DO round trip.
    if( preDoSample )
        *preDoSample = static_cast<long long>( sglx_getStreamSampleCount( hSglx_, 2, 0 ) );

    if( !sglx_ni_DO_set( hSglx_, doLine_, kLineHighBits ) )
        ClLog() << "DecisionThread: sglx_ni_DO_set (raise) failed: " << sglx_getError( hSglx_ ) << "\n";

    lineHigh_ = true;
    lineHighUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds( doPulseMs_ );

    // Queried after the DO command returns, so it bounds the pulse time
    // from above (includes the DO round trip). js=2, ip=0: the IMEC stream.
    return static_cast<long long>( sglx_getStreamSampleCount( hSglx_, 2, 0 ) );
}

void DecisionThread::lowerLineIfDue()
{
    if( lineHigh_ && std::chrono::steady_clock::now() >= lineHighUntil_ ) {
        if( !sglx_ni_DO_set( hSglx_, doLine_, kLineLowBits ) )
            ClLog() << "DecisionThread: sglx_ni_DO_set (lower) failed: " << sglx_getError( hSglx_ ) << "\n";
        lineHigh_ = false;
    }
}

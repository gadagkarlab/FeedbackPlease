#include "ImecFetchThread.h"
#include "FetchHealth.h"
#include "Log.h"

#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <stdexcept>

#include "SglxCppClient.h"
#include "DigitalWordUtils.h"
#include "SglxMetaReader.h"
#include "Preprocessor.h"

namespace {
    const int   kImecJs = 2;   // IMEC AP stream selector, per SglxApi.h
    const int   kImecIp = 0;   // first (only) imec probe
}


ImecFetchThread::ImecFetchThread( void *hSglx, FilterBank &filterBank,
                                   const std::vector<int> &carChannelIds,
                                   bool applyHighpass, double highpassCutoffHz,
                                   int fetchChunkMs,
                                   ThreadSafeQueue<SpikeEvent> &spikeQueue,
                                   std::atomic<long long> &decidedThrough,
                                   const std::string &spikeTimesPath )
    :   hSglx_( hSglx ), filterBank_( filterBank ),
        carChannelIds_( carChannelIds ),
        applyHighpass_( applyHighpass ), highpassCutoffHz_( highpassCutoffHz ),
        fetchChunkMs_( fetchChunkMs ), spikeQueue_( spikeQueue ),
        decidedThrough_( decidedThrough ),
        spikeTimesPath_( spikeTimesPath ), stopFlag_( false )
{}


void ImecFetchThread::start()
{
    thread_ = std::thread( &ImecFetchThread::fetchLoop, this );
}


void ImecFetchThread::stop()
{
    stopFlag_.store( true );
}


void ImecFetchThread::join()
{
    if( thread_.joinable() )
        thread_.join();
}


void ImecFetchThread::fetchLoop()
{
    std::ofstream spikeTimesFile;
    if( !spikeTimesPath_.empty() )
        spikeTimesFile.open( spikeTimesPath_.c_str(), std::ios::app );
    if( !spikeTimesPath_.empty() && !spikeTimesFile.is_open() )
        ClLog() << "ImecFetchThread: could not open '" << spikeTimesPath_ << "' for writing\n";

    double sampleRate = sglx_getStreamSampleRate( hSglx_, kImecJs, kImecIp );
    if( sampleRate <= 0 ) {
        ClLog() << "ImecFetchThread: sglx_getStreamSampleRate failed: "
                  << sglx_getError( hSglx_ ) << "\n";
        return;
    }

    // CAR channel group: the FULL group the filter was trained with (see
    // Preprocessor.h) -- NOT just the filter's own 5 channels. Falls back
    // to just the filter's channels (CAR disabled) if the group is empty.
    bool applyCar = !carChannelIds_.empty();
    std::vector<int> carGroup = applyCar ? carChannelIds_ : filterBank_.channels;

    std::vector<int> filterIndexWithinCarGroup( filterBank_.channels.size() );
    for( size_t j = 0; j < filterBank_.channels.size(); ++j ) {
        std::vector<int>::const_iterator it =
            std::find( carGroup.begin(), carGroup.end(), filterBank_.channels[j] );
        if( it == carGroup.end() ) {
            ClLog() << "ImecFetchThread: filter channel " << filterBank_.channels[j]
                      << " is not part of the CAR channel group\n";
            return;
        }
        filterIndexWithinCarGroup[j] = static_cast<int>( it - carGroup.begin() );
    }

    // Fetch subset = the full CAR group. No SY channel needed: cross-stream
    // alignment is done by NiFetchThread via sglx_mapSample.
    std::vector<int> channelSubset( carGroup.begin(), carGroup.end() );
    const int nFetchChans  = static_cast<int>( channelSubset.size() );
    const int nCarChans    = static_cast<int>( carGroup.size() );
    const int nFilterChans = filterBank_.nChannels();

    const long long chunkSamples = static_cast<long long>( fetchChunkMs_ / 1000.0 * sampleRate + 0.5 );

    ConvolutionEngine engine( filterBank_.templateLength, nFilterChans, filterBank_.taps );
    Preprocessor       preprocessor( nCarChans, highpassCutoffHz_, sampleRate, applyHighpass_, applyCar );

    t_ull fromCt = sglx_getStreamSampleCount( hSglx_, kImecJs, kImecIp );

    std::vector<double> compact;   // reused buffer for the filter-channel subset, post highpass+CAR

    std::atomic<bool> noStopAll( false ), noLost( false );
    FetchHealth health( "ImecFetchThread", stopAll_ ? streamTimeoutS_ : 0.0,
                        stopAll_ ? *stopAll_ : noStopAll, streamLost_ ? *streamLost_ : noLost );

    while( !stopFlag_.load() && !health.timedOut() ) {

        cppClient_sglx_fetch io;
        io.channel_subset = &channelSubset[0];
        io.n_cs           = nFetchChans;
        io.max_samps      = static_cast<int>( chunkSamples );
        io.downsample     = 1;
        io.js             = kImecJs;
        io.ip             = kImecIp;

        t_ull headCt = sglx_fetch( io, hSglx_, fromCt );

        if( headCt == 0 ) {
            health.onError( sglx_getError( hSglx_ ) );
            std::this_thread::sleep_for( std::chrono::milliseconds( fetchChunkMs_ ) );
            continue; // retry from the same fromCt
        }

        long long tpts = static_cast<long long>( io.data.size() ) / nFetchChans;
        health.onSuccess( tpts > 0 );
        if( tpts <= 0 ) {
            fromCt = headCt;
            continue;
        }

        // Highpass+CAR across the FULL group, then subset to the filter's channels.
        std::vector<double> preprocessed = preprocessor.processChunk( &io.data[0], static_cast<size_t>( tpts ) );

        compact.assign( static_cast<size_t>( tpts ) * nFilterChans, 0.0 );
        for( long long t = 0; t < tpts; ++t ) {
            const double *srcRow = &preprocessed[static_cast<size_t>( t ) * nCarChans];
            double       *dstRow = &compact[static_cast<size_t>( t ) * nFilterChans];
            for( int c = 0; c < nFilterChans; ++c )
                dstRow[c] = srcRow[filterIndexWithinCarGroup[c]];
        }

        std::vector<PeakEvent> peaks = engine.processChunk(
            &compact[0], static_cast<size_t>( tpts ), static_cast<long long>( headCt ) );

        long long nSpikes = 0;
        for( size_t p = 0; p < peaks.size(); ++p ) {

            if( peaks[p].score <= filterBank_.threshold )
                continue;

            if( spikeTimesFile.is_open() ) {
                spikeTimesFile << peaks[p].sampleIndex << "\n";
                spikeTimesFile.flush();
            }

            SpikeEvent ev;
            ev.sampleIndex = peaks[p].sampleIndex;
            spikeQueue_.push( ev );
            ++nSpikes;
        }

        decidedThrough_.store( engine.decidedThrough(), std::memory_order_release );

        if( stats_ ) {
            stats_->spikes.fetch_add( nSpikes, std::memory_order_relaxed );
            stats_->imecSample.store( static_cast<long long>( headCt ) + tpts, std::memory_order_relaxed );
        }

        fromCt = headCt + static_cast<t_ull>( tpts );
    }
}

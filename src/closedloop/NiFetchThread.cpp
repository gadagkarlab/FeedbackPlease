#include "NiFetchThread.h"
#include "FetchHealth.h"
#include "Log.h"

#include <iostream>
#include <chrono>
#include <algorithm>
#include <fstream>

#include "SglxCppClient.h"
#include "DigitalWordUtils.h"
#include "SyllableDecoder.h"

namespace {
    const int kNiJs   = 0;  // NI stream selector, per SglxApi.h
    const int kNiIp   = 0;
    const int kImecJs = 2;  // IMEC AP stream -- target of sglx_mapSample (must match ImecFetchThread)
    const int kImecIp = 0;
}


NiFetchThread::NiFetchThread( void *hSglx, const std::vector<int> &syllableLines,
                               int debounceSamples, double rearmMs, double refractoryMs,
                               int fetchChunkMs,
                               ThreadSafeQueue<SyllableEvent> &syllableQueue,
                               const std::string &syllableTimesPath )
    :   hSglx_( hSglx ), syllableLines_( syllableLines ),
        debounceSamples_( debounceSamples ), rearmMs_( rearmMs ), refractoryMs_( refractoryMs ),
        fetchChunkMs_( fetchChunkMs ),
        syllableQueue_( syllableQueue ), syllableTimesPath_( syllableTimesPath ),
        stopFlag_( false )
{}


void NiFetchThread::start() { thread_ = std::thread( &NiFetchThread::fetchLoop, this ); }
void NiFetchThread::stop()  { stopFlag_.store( true ); }
void NiFetchThread::join()  { if( thread_.joinable() ) thread_.join(); }


void NiFetchThread::fetchLoop()
{
    std::ofstream syllableTimesFile;
    if( !syllableTimesPath_.empty() ) {
        syllableTimesFile.open( syllableTimesPath_.c_str(), std::ios::app );
        if( !syllableTimesFile.is_open() )
            ClLog() << "NiFetchThread: could not open '" << syllableTimesPath_ << "' for writing\n";
    }

    double sampleRate = sglx_getStreamSampleRate( hSglx_, kNiJs, kNiIp );
    if( sampleRate <= 0 ) {
        ClLog() << "NiFetchThread: sglx_getStreamSampleRate failed: "
                  << sglx_getError( hSglx_ ) << "\n";
        return;
    }

    // Resolve the DW (digital word) channel's index dynamically.
    // {MN,MA,XA,DW} ordering per SglxApi.h; DW starts right after MN+MA+XA.
    cppClient_sglx_get_ints acqChans;
    if( !sglx_getStreamAcqChans( acqChans, hSglx_, kNiJs, kNiIp ) || acqChans.vint.size() < 4 ) {
        ClLog() << "NiFetchThread: sglx_getStreamAcqChans failed: "
                  << sglx_getError( hSglx_ ) << "\n";
        return;
    }
    int dwChannelIndex = acqChans.vint[0] + acqChans.vint[1] + acqChans.vint[2]; // nMN+nMA+nXA

    std::vector<int> channelSubset( 1, dwChannelIndex );

    // syllableLines_ is assumed contiguous ascending (e.g. {5,6,7}) --
    // extractField() reads them as one multi-bit field starting at the
    // lowest line number.
    int fieldStartBit = *std::min_element( syllableLines_.begin(), syllableLines_.end() );
    int fieldWidth     = static_cast<int>( syllableLines_.size() );

    const long long chunkSamples = static_cast<long long>( fetchChunkMs_ / 1000.0 * sampleRate + 0.5 );

    // Rising-edge decoder: fast attack, slow re-arm, refractory -- see SyllableDecoder.h.
    SyllableDecoder decoder( debounceSamples_,
                             static_cast<long long>( rearmMs_ / 1000.0 * sampleRate + 0.5 ),
                             static_cast<long long>( refractoryMs_ / 1000.0 * sampleRate + 0.5 ) );

    t_ull fromCt = sglx_getStreamSampleCount( hSglx_, kNiJs, kNiIp );

    std::atomic<bool> noStopAll( false ), noLost( false );
    FetchHealth health( "NiFetchThread", stopAll_ ? streamTimeoutS_ : 0.0,
                        stopAll_ ? *stopAll_ : noStopAll, streamLost_ ? *streamLost_ : noLost );

    while( !stopFlag_.load() && !health.timedOut() ) {

        cppClient_sglx_fetch io;
        io.channel_subset = &channelSubset[0];
        io.n_cs           = 1;
        io.max_samps      = static_cast<int>( 4 * chunkSamples );
        io.downsample     = 1;
        io.js             = kNiJs;
        io.ip             = kNiIp;

        t_ull headCt = sglx_fetch( io, hSglx_, fromCt );

        if( headCt == 0 ) {
            health.onError( sglx_getError( hSglx_ ) );
            std::this_thread::sleep_for( std::chrono::milliseconds( fetchChunkMs_ ) );
            continue;
        }

        long long tpts = static_cast<long long>( io.data.size() );  // n_cs == 1
        health.onSuccess( tpts > 0 );
        if( tpts <= 0 ) {
            fromCt = headCt;
            continue;
        }

        for( long long t = 0; t < tpts; ++t ) {

            long long idx = static_cast<long long>( headCt ) + t;
            short     w   = io.data[static_cast<size_t>( t )];

            int code = extractField( w, fieldStartBit, fieldWidth );

            int       onsetCode = 0;
            long long onsetIdx  = 0;
            if( decoder.push( code, idx, onsetCode, onsetIdx ) ) {
                // Map the onset into the IMEC stream so DecisionThread can
                // measure spike times relative to it in IMEC samples.
                t_ull imecIdx = sglx_mapSample( hSglx_, kImecJs, kImecIp,
                                                static_cast<t_ull>( onsetIdx ),
                                                kNiJs, kNiIp );
                if( imecIdx == 0 ) {
                    if( stats_ )
                        stats_->syllablesLost.fetch_add( 1, std::memory_order_relaxed );
                    ClLog() << "NiFetchThread: sglx_mapSample failed for syllable code "
                              << onsetCode << " at NI sample " << onsetIdx
                              << ": " << sglx_getError( hSglx_ ) << " -- event dropped\n";
                }
                else {
                    SyllableEvent ev;
                    ev.code            = onsetCode;
                    ev.sampleIndex     = onsetIdx;
                    ev.imecSampleIndex = static_cast<long long>( imecIdx );
                    syllableQueue_.push( ev );
                    if( stats_ )
                        stats_->syllables.fetch_add( 1, std::memory_order_relaxed );

                    if( syllableTimesFile.is_open() ) {
                        syllableTimesFile << ev.code << "," << ev.sampleIndex << ","
                                          << ev.imecSampleIndex << "\n";
                        syllableTimesFile.flush();
                    }
                }
            }
        }

        fromCt = headCt + static_cast<t_ull>( tpts );
    }
}

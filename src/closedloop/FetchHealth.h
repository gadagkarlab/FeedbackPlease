#ifndef CLOSEDLOOP_FETCHHEALTH_H
#define CLOSEDLOOP_FETCHHEALTH_H

#include <atomic>
#include <chrono>
#include "Log.h"
#include <sstream>
#include <string>

// Shared by ImecFetchThread / NiFetchThread: keeps a failing stream from
// flooding the console (one line on the first error, then a summary every
// kReportEvery while it keeps failing, and a line when it recovers), and
// stops ClosedLoop when a stream has delivered no data for timeoutS seconds
// -- e.g. SpikeGLX stopped or closed mid-session, or a new run reset the
// sample counter so the old read position never fills. Without this the
// app kept running blind (no detections, no pulses) and printed ~170 lines/s
// per thread.
class FetchHealth {
public:
    typedef std::chrono::steady_clock Clock;

    FetchHealth( const char *name, double timeoutS,
                 std::atomic<bool> &stopAll, std::atomic<bool> &streamLost )
        :   name_( name ), timeoutS_( timeoutS ), stopAll_( stopAll ), streamLost_( streamLost ),
            lastData_( Clock::now() ), nErr_( 0 ) {}

    // A fetch call failed outright.
    void onError( const std::string &msg )
    {
        Clock::time_point now = Clock::now();
        std::ostringstream line;
        if( nErr_ == 0 ) {
            errSince_ = lastReport_ = now;
            line << name_ << ": fetch failing: " << msg;
            say( line );
        }
        else if( now - lastReport_ >= kReportEvery ) {
            line << name_ << ": fetch still failing after " << secs( now - errSince_ )
                 << " s (" << nErr_ << " errors): " << msg;
            say( line );
            lastReport_ = now;
        }
        ++nErr_;
        checkTimeout( now );
    }

    // A fetch succeeded; gotSamples = it returned at least one new sample.
    void onSuccess( bool gotSamples )
    {
        Clock::time_point now = Clock::now();
        if( nErr_ > 0 ) {
            std::ostringstream line;
            line << name_ << ": fetch recovered after " << secs( now - errSince_ )
                 << " s (" << nErr_ << " errors)";
            say( line );
            nErr_ = 0;
        }
        if( gotSamples )
            lastData_ = now;
        else
            checkTimeout( now );
    }

    // True once this stream has timed out (the caller should exit its loop).
    bool timedOut() const { return timedOut_; }

private:
    static double secs( Clock::duration d ) { return std::chrono::duration<double>( d ).count(); }

    // Whole line in one write, so the two fetch threads' messages can't
    // interleave mid-line (they often fail at the same moment).
    static void say( const std::ostringstream &line )
    {
        std::cerr << ( line.str() + '\n' );
    }

    void checkTimeout( Clock::time_point now )
    {
        if( timeoutS_ <= 0 || timedOut_ || secs( now - lastData_ ) < timeoutS_ )
            return;
        timedOut_ = true;
        std::ostringstream line;
        line << name_ << ": no data for " << secs( now - lastData_ )
             << " s -- SpikeGLX stopped, closed, or restarted its run. Stopping ClosedLoop.";
        say( line );
        streamLost_.store( true );
        stopAll_.store( true );
    }

    const std::chrono::seconds kReportEvery = std::chrono::seconds( 5 );

    const char        *name_;
    double             timeoutS_;
    std::atomic<bool> &stopAll_;
    std::atomic<bool> &streamLost_;
    Clock::time_point  lastData_, errSince_, lastReport_;
    long long          nErr_;
    bool               timedOut_ = false;
};

#endif // CLOSEDLOOP_FETCHHEALTH_H

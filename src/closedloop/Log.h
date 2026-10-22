#ifndef CLOSEDLOOP_LOG_H
#define CLOSEDLOOP_LOG_H

#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

// Every message the ClosedLoop code used to print with std::cout/std::cerr goes
// through here, so a host (the FeedbackPlease GUI) can route it into its own
// log window. With no sink installed it falls back to std::cerr.
//
// Thread-safe: the fetch/decision threads and the runner all log concurrently.
namespace clog {

inline std::mutex &mutex()
{
    static std::mutex m;
    return m;
}

inline std::function<void( const std::string & )> &sink()
{
    static std::function<void( const std::string & )> s;
    return s;
}

inline void setSink( std::function<void( const std::string & )> f )
{
    std::lock_guard<std::mutex> lock( mutex() );
    sink() = std::move( f );
}

// One message = one line; trailing newlines are dropped.
inline void write( std::string line )
{
    while( !line.empty() && ( line.back() == '\n' || line.back() == '\r' ) )
        line.pop_back();
    std::lock_guard<std::mutex> lock( mutex() );
    if( sink() )
        sink()( line );
    else
        std::cerr << line << '\n';
}

} // namespace clog

// `ClLog() << "a" << 3 << "\n";` -- emitted as one message when the statement ends.
class ClLog {
public:
    ClLog() {}
    ClLog( const ClLog & ) = delete;
    ~ClLog() { clog::write( s_.str() ); }

    template<typename T>
    ClLog &operator<<( const T &v ) { s_ << v; return *this; }

private:
    std::ostringstream s_;
};

#endif // CLOSEDLOOP_LOG_H

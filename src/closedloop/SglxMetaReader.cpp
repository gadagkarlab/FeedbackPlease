#include "SglxMetaReader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstdlib>


std::map<std::string, std::string> parseMetaFile( const std::string &metaPath )
{
    std::ifstream fh( metaPath.c_str() );
    if( !fh.is_open() )
        throw std::runtime_error( "parseMetaFile: could not open '" + metaPath + "'" );

    std::map<std::string, std::string> meta;
    std::string line;

    while( std::getline( fh, line ) ) {

        if( !line.empty() && line[line.size() - 1] == '\r' )
            line.erase( line.size() - 1 );

        size_t eq = line.find( '=' );
        if( eq == std::string::npos )
            continue;

        std::string key = line.substr( 0, eq );
        std::string val = line.substr( eq + 1 );

        if( !key.empty() && key[0] == '~' )
            key = key.substr( 1 );

        meta[key] = val;
    }

    return meta;
}


std::vector<int> parseChanSubset( const std::string &spec )
{
    std::vector<int>   out;
    std::stringstream  ss( spec );
    std::string        part;

    while( std::getline( ss, part, ',' ) ) {

        size_t colon = part.find( ':' );

        if( colon != std::string::npos ) {
            int a = std::atoi( part.substr( 0, colon ).c_str() );
            int b = std::atoi( part.substr( colon + 1 ).c_str() );
            for( int i = a; i <= b; ++i )
                out.push_back( i );
        }
        else if( !part.empty() )
            out.push_back( std::atoi( part.c_str() ) );
    }

    return out;
}


SglxMeta SglxMeta::load( const std::string &metaPath )
{
    std::map<std::string, std::string> meta = parseMetaFile( metaPath );

    SglxMeta m;
    m.sampleRateHz  = std::atof( meta["imSampRate"].c_str() );
    m.nSavedChans   = std::atoi( meta["nSavedChans"].c_str() );
    m.savedChannelIds = parseChanSubset( meta["snsSaveChanSubset"] );

    // snsApLfSy = "<ap>,<lf>,<sy>" -- last field is the sync-channel count.
    std::string apLfSy = meta["snsApLfSy"];
    size_t      lastComma = apLfSy.find_last_of( ',' );
    m.nSyncChans = (lastComma != std::string::npos)
                 ? std::atoi( apLfSy.substr( lastComma + 1 ).c_str() )
                 : 0;

    // ~snsGeomMap: present since SpikeGLX 20230202. Absent on older files,
    // which leaves geomMap empty -- shankChannels() reports that, so a
    // run that never asks for a shank group still works.
    m.geomMap = parseGeomMap( meta["snsGeomMap"] );

    return m;
}


std::vector<GeomEntry> parseGeomMap( const std::string &val )
{
    std::vector<GeomEntry> out;
    size_t pos = 0;
    bool   isHeader = true;

    while( true ) {

        size_t open = val.find( '(', pos );
        if( open == std::string::npos )
            break;

        size_t close = val.find( ')', open );
        if( close == std::string::npos )
            break;

        std::string body = val.substr( open + 1, close - open - 1 );
        pos = close + 1;

        // First tuple is "(partNumber,nShank,shankPitch,shankWidth)" --
        // comma-separated and non-numeric, not an electrode entry.
        if( isHeader ) {
            isHeader = false;
            continue;
        }

        // "s:x:z:u"
        int f[4] = { 0, 0, 0, 0 };
        int n = 0;
        std::stringstream ss( body );
        std::string tok;
        while( n < 4 && std::getline( ss, tok, ':' ) )
            f[n++] = std::atoi( tok.c_str() );

        if( n < 4 )
            throw std::runtime_error( "parseGeomMap: malformed entry '(" + body + ")'" );

        GeomEntry e;
        e.shank = f[0];
        e.x     = f[1];
        e.z     = f[2];
        e.used  = f[3];
        out.push_back( e );
    }

    return out;
}


std::vector<int> SglxMeta::shankChannels( int shank ) const
{
    if( geomMap.empty() ) {
        throw std::runtime_error(
            "SglxMeta::shankChannels: .meta has no ~snsGeomMap -- this "
            "recording predates SpikeGLX 20230202 (it would have the older "
            "~snsShankMap instead), so the CAR channel group can't be "
            "derived from it" );
    }

    // Entry i describes savedChannelIds[i], not raw channel i.
    if( geomMap.size() > savedChannelIds.size() ) {
        throw std::runtime_error(
            "SglxMeta::shankChannels: ~snsGeomMap has more entries than "
            "snsSaveChanSubset has channels -- .meta is inconsistent" );
    }

    std::vector<int> out;
    for( size_t i = 0; i < geomMap.size(); ++i )
        if( geomMap[i].shank == shank )
            out.push_back( savedChannelIds[i] );

    if( out.empty() ) {
        throw std::runtime_error( "SglxMeta::shankChannels: no electrodes on shank " +
            std::to_string( shank ) + " (carShank is 0-based -- shank 0 is the "
            "left-most with the probe tips pointing down)" );
    }

    return out;
}

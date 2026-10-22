#ifndef CLOSEDLOOP_SGLXMETAREADER_H
#define CLOSEDLOOP_SGLXMETAREADER_H

#include <string>
#include <vector>
#include <map>

// One ~snsGeomMap electrode entry, "(s:x:z:u)".
struct GeomEntry {
    int shank;   // 0-based, tips down, shank-0 left-most
    int x;       // microns from the left edge of its own shank
    int z;       // microns from the tip, upward
    int used;    // 0/1 -- SpikeGLX's own "include in CAR" flag
};

// Parses a SpikeGLX .meta file and the handful of fields Calibration needs
// to read a raw .bin file directly (i.e. NOT via the live socket API --
// this is only used for the offline training-file calibration pass).
//
// Ported from FilterGen/generate_filter.py's load_sglx_meta /
// parse_chan_subset / find_sync_channel_count / parse_geom_map -- keep
// these in sync if the Python side changes its parsing.
struct SglxMeta {
    double              sampleRateHz;
    int                 nSavedChans;
    std::vector<int>    savedChannelIds;  // parsed snsSaveChanSubset, e.g. "0:384" -> [0..384]
    int                 nSyncChans;       // from snsApLfSy's last field
    std::vector<GeomEntry> geomMap;       // parsed ~snsGeomMap, one entry per SAVED channel

    static SglxMeta load( const std::string &metaPath );

    // Raw channel ids of every electrode on `shank` -- this is the CAR
    // channel group. Per the README/this project's own live testing, CAR
    // must be computed across the SAME full channel group used when the
    // filter was trained (a whole shank, 96 channels here), not just the
    // filter's own 5 channels: CAR over too few channels doesn't reject
    // the shared noise the filter's statistics were calibrated against,
    // and caused a real, large false-positive blowup found live.
    //
    // Throws std::runtime_error if the shank has no entries (a typo'd
    // carShank must fail loudly, never silently yield a smaller group).
    std::vector<int> shankChannels( int shank ) const;
};

// Parses a MATLAB-range-style channel spec, e.g. "0,1,4:8,10" -> {0,1,4,5,6,7,8,10}.
std::vector<int> parseChanSubset( const std::string &spec );

// Raw key=value line parser shared by SglxMeta -- exposed in case callers
// need a field this struct doesn't surface.
std::map<std::string, std::string> parseMetaFile( const std::string &metaPath );

// Parses a ~snsGeomMap value: a "(part,nShank,shankPitch,shankWidth)"
// header tuple followed by one "(s:x:z:u)" tuple per SAVED channel (the
// header tuple is skipped; entry i corresponds to savedChannelIds[i], NOT
// to raw channel i -- they only coincide when every channel is saved).
std::vector<GeomEntry> parseGeomMap( const std::string &val );

#endif // CLOSEDLOOP_SGLXMETAREADER_H

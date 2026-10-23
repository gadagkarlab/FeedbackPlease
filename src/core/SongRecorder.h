#pragma once
// Song-triggered recording: keeps the last preRollSec of audio in a ring buffer.
// When song starts, a new WAV is opened with that history at the front and recording
// continues until song has been off for postRollSec; song resuming within that time
// extends the same file. Files go to <birdDir>/<YYYYMMDD>/<bird>_YYYYMMDD_HHMMSS_mmm.wav (local
// time of the file's first sample, from the sample clock); each gets a row in the index CSV.
//
// Not thread-safe by itself: the engine hands frames to the recorder thread, which
// calls push() (see AsyncRecorder in app/Engine.cpp). Status getters are atomic.

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "core/WavFile.h"

namespace fp {

struct SongFileConfig {
    std::string indexPath;      // CSV with one row per file
    std::string birdDir;        // day folders are created in here
    std::string bird;           // file name prefix (already safe for file names)
    double startEpochMs = 0.0;  // wall-clock time of the first sample pushed (ms since 1970)
    double sampleRate = 40000.0;
    double preRollSec = 2.0;
    double postRollSec = 2.0;
    bool mic = true, detection = true, markers = true;  // channels, in this order
};

class SongFileWriter {
public:
    ~SongFileWriter() { close(); }

    bool open(const SongFileConfig& cfg, std::string* err = nullptr);
    // One frame: n microphone samples plus the frame's detection value, marker level
    // and whether the song detector is on.
    void push(const float* mic, int n, float detection, float marker, bool song);
    void close();  // finishes a file in progress

    int channels() const { return ch_; }
    int filesDone() const { return filesDone_.load(); }
    bool recording() const { return recording_.load(); }

private:
    void startFile();
    void endFile();

    SongFileConfig cfg_;
    bool open_ = false;
    int ch_ = 0;
    size_t preN_ = 0, postN_ = 0;  // in samples

    std::vector<float> ring_;   // preN_ samples x ch_, interleaved
    size_t ringHead_ = 0;       // next sample slot to write
    size_t ringCount_ = 0;      // valid samples in the ring
    std::vector<float> frame_;  // current frame, interleaved

    WavWriter wav_;
    std::ofstream index_;
    uint64_t sample_ = 0;     // samples pushed since open (same clock as the events CSV)
    uint64_t fileStart_ = 0;  // session sample of the first sample in the current file
    uint64_t lastSong_ = 0;   // session sample just after the last frame with song on
    size_t preInFile_ = 0;    // samples of pre-roll written at the start of the current file
    int fileNo_ = 0;
    std::string fileName_;
    std::string fileRel_;    // day/name.wav, for the index
    double fileTime_ = 0.0;  // wall-clock ms of the current file's first sample

    std::atomic<int> filesDone_{0};
    std::atomic<bool> recording_{false};
};

// Local time of epochMs as "YYYYMMDD" and "YYYYMMDD_HHMMSS_mmm".
std::string localDay(double epochMs);
std::string localStamp(double epochMs);

}  // namespace fp

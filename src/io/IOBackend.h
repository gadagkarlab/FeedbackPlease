#pragma once
// Hardware abstraction: one analog input stream plus the outputs the feedback
// logic drives. DaqBackend talks to NI-DAQmx; SimBackend plays a WAV file or a
// synthetic song in real time so the whole app can run without hardware.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/Params.h"

namespace fp {

struct DigitalOutputs {
    bool feedback = false;
    bool syllable = false;
    bool match = false;
    bool song = false;
    bool operator==(const DigitalOutputs& o) const {
        return feedback == o.feedback && syllable == o.syllable && match == o.match && song == o.song;
    }
    bool operator!=(const DigitalOutputs& o) const { return !(*this == o); }
};

class IOBackend {
public:
    virtual ~IOBackend() = default;

    virtual bool start(const AppParams& p, std::string& err) = 0;
    virtual void stop() = 0;

    // Actual input sample rate (a simulated WAV may differ from the settings).
    virtual double sampleRate() const = 0;

    // Blocks until n samples are available. Returns false on error.
    virtual bool read(float* dst, int n, std::string& err) = 0;

    // Samples acquired but not yet read: how far processing lags the hardware.
    virtual size_t backlog() { return 0; }

    virtual void writeDigital(const DigitalOutputs&) {}
    // Syllable code port (U8). Only called when the value changes.
    virtual void writeSyllableCode(uint8_t /*code*/) {}
    // Preload feedback waveform 1, 2 or 3 so that fireWave() starts with minimal latency.
    virtual void armWave(int /*which*/) {}
    virtual void fireWave(int /*which*/) {}
    // Level of the closed-loop pulse input line: 1 high, 0 low, -1 if there is no such input.
    // Polled once per frame; the engine turns rising edges into triggers.
    virtual int pollPulseInput() { return -1; }
    // Waveform 3 is the pulse-triggered feedback (noise burst or wave file).
    // Called once per frame for housekeeping (e.g. reloading a waveform after playback).
    virtual void service() {}
    // Non-fatal output errors since the last call (empty if none).
    virtual std::string takeOutputError() { return {}; }

    virtual const char* name() const = 0;
};

std::unique_ptr<IOBackend> makeDaqBackend();
std::unique_ptr<IOBackend> makeSimBackend();
bool daqAvailable();  // false when built without NI-DAQmx
std::string describeDaqDevices();  // e.g. "Dev2 (PCIe-6321), Dev3 (PCIe-6321)"

}  // namespace fp

#pragma once
// Where frames come from and where each box's outputs go. One FrameSource feeds one
// acquisition thread:
//   - SingleSource: one box on its own IOBackend (DaqBackend or SimBackend), as in the
//     single-box modes (low-latency committed AO, pulse input).
//   - DaqCard: all boxes whose microphones are on one NI card (multi-box mode): one
//     multi-channel AI task, one DO task, one streaming AO task.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "core/Params.h"
#include "io/IOBackend.h"

namespace fp {

class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual bool start(std::string& err) = 0;
    virtual void stop() = 0;
    virtual int boxCount() const = 0;
    virtual double sampleRate() const = 0;  // actual rate, valid after start()
    virtual int frameSamples() const = 0;   // samples per frame, valid after start()
    // Blocks until the next frame of every box is available.
    virtual bool read(std::string& err) = 0;
    virtual const float* frame(int k) const = 0;  // box k's samples for the frame just read
    virtual IOBackend& out(int k) = 0;            // box k's outputs
    virtual void endFrame() {}                    // after every box has run: flush combined outputs
    virtual size_t backlog() = 0;                 // samples acquired but not yet read
    virtual const char* name() const = 0;
};

// One box on its own IOBackend.
class SingleSource final : public FrameSource {
public:
    SingleSource(std::unique_ptr<IOBackend> io, const AppParams& p) : io_(std::move(io)), p_(p) {}
    bool start(std::string& err) override;
    void stop() override { io_->stop(); }
    int boxCount() const override { return 1; }
    double sampleRate() const override { return fs_; }
    int frameSamples() const override { return n_; }
    bool read(std::string& err) override { return io_->read(buf_.data(), n_, err); }
    const float* frame(int) const override { return buf_.data(); }
    IOBackend& out(int) override { return *io_; }
    size_t backlog() override { return io_->backlog(); }
    const char* name() const override { return io_->name(); }

private:
    std::unique_ptr<IOBackend> io_;
    AppParams p_;
    double fs_ = 0.0;
    int n_ = 0;
    std::vector<float> buf_;
};

// Multi-box: the boxes whose microphones are on one NI card. Built only with NI-DAQmx;
// makeDaqCard returns nullptr otherwise.
std::unique_ptr<FrameSource> makeDaqCard(const std::string& device, const std::vector<AppParams>& boxes);

// Device name of a physical channel: "Dev2/ai0" -> "Dev2", "/PXI1Slot6/port0/line1" -> "PXI1Slot6".
std::string deviceOf(const std::string& channel);

}  // namespace fp

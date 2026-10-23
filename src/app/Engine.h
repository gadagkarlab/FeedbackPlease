#pragma once
// Runs one or more boxes. Each FrameSource (one NI card in multi-box mode, or one box
// on its own backend) gets a dedicated time-critical acquisition thread that reads a
// frame and runs every box on that source. This replaces the LabVIEW top-level
// acquisition loop.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/Box.h"
#include "core/Params.h"
#include "io/FrameSource.h"

namespace fp {

class Engine {
public:
    Engine() = default;
    ~Engine() { stop(); }

    // single = true: exactly one box on its own backend (low-latency committed AO,
    // pulse input). single = false (multi-box): DAQ boxes are grouped by the card of
    // their AI channel (one AI / DO / streaming-AO task per card); simulated boxes
    // each run on their own simulated source.
    bool start(const std::vector<AppParams>& boxes, bool single, std::string& err);
    void stop();
    bool running() const { return alive_.load() > 0; }

    int boxCount() const { return int(boxes_.size()); }
    Box* box(int i) { return i >= 0 && i < boxCount() ? boxes_[size_t(i)].get() : nullptr; }
    const Box* box(int i) const { return i >= 0 && i < boxCount() ? boxes_[size_t(i)].get() : nullptr; }
    std::string error() const;  // first box error, if any

private:
    struct Runner {
        std::unique_ptr<FrameSource> src;
        std::vector<int> boxes;  // engine box index for each source box
        std::thread th;
    };
    void run(Runner& r);

    std::vector<std::unique_ptr<Box>> boxes_;
    std::vector<std::unique_ptr<Runner>> runners_;
    std::atomic<bool> stop_{false};
    std::atomic<int> alive_{0};  // runner threads still running
};

}  // namespace fp

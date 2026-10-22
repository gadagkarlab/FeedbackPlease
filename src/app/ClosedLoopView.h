#pragma once
// The "Closed loop" tab: ClosedLoop's config as a parameter panel (left), calibration and live
// controls, counters, plots and log (right). Runs independently of the microphone Engine.

#include <string>
#include <vector>

#include "app/ClosedLoop.h"

namespace fp {

class ClosedLoopView {
public:
    ClosedLoopView() = default;
    ~ClosedLoopView();

    // Settings file (ClosedLoop's own key=value config.txt format). Missing file = defaults.
    void reset() { p_ = ClParams(); filterKey_.clear(); }  // defaults, e.g. before loading another bird
    void load(const std::string& path, std::string* status = nullptr);
    bool save(const std::string& path, std::string* err = nullptr) const;

    void drawParams();  // the left-hand panel
    void drawMain();    // controls, status, plots, log

    // Stops whatever is running and waits for it. Call before the window closes.
    void shutdown();

    ClosedLoopRunner::State state() const { return runner_.state(); }
    std::string statusText() const;  // for the toolbar, e.g. "Closed loop: Live  12 spikes/s"

private:
    void drawControls();
    void drawCounters();
    void drawPlots(float height);
    void drawDecisions();
    void drawLog();
    void refreshFilterInfo(bool force);

    ClParams p_;
    ClosedLoopRunner runner_;

    std::vector<std::string> log_;
    size_t logCursor_ = 0;
    std::vector<ClDecisionRow> rows_;
    size_t rowCursor_ = 0;
    bool logAutoscroll_ = true;

    ClFilterInfo filterInfo_;
    std::string filterKey_;
    double filterCheckedAt_ = -1e9;

    // Rate readout
    ClCounters last_;
    double lastT_ = 0, spikeRate_ = 0, syllRate_ = 0;

    std::string message_;  // result of the last button press
};

}  // namespace fp

#pragma once

#include <string>
#include <vector>

#include "app/Engine.h"
#include "app/TuneView.h"
#ifdef FP_CLOSEDLOOP
#include "app/ClosedLoopView.h"
#endif
#ifdef FP_MSN
#include "app/MsnFinderView.h"
#endif
#include "core/Params.h"

namespace fp {

class Gui {
public:
    explicit Gui(std::string settingsPath);
    ~Gui();
    void draw();  // one ImGui frame
    void startEngine();
    void openTune(const std::string& folder, bool analyze, bool summary);
#ifdef FP_CLOSEDLOOP
    void openClosedLoop() { selectClTab_ = true; }
#endif
#ifdef FP_MSN
    void openMsn(bool loadNow) {
        selectMsnTab_ = true;
        if (loadNow) msn_.loadSession();
    }
#endif

private:
    void drawToolbar();
    void drawControls();
    void drawHardware();
    void drawSpectrum();
    void drawProcessing();
    void drawTarget();
    void drawFeedback();
    void drawSong();
    void drawCodes();
    void drawRecording();
    void drawDisplay();
    void drawStatus();
    void drawPlots(float height);
    void drawEventLog();

    void pushParams();
    std::string closedLoopPath() const;  // <settings file without .ini>.closedloop.txt
    std::string msnPath() const;  // <settings file without .ini>.msn.txt

    AppParams p_;
    std::string settingsPath_;
    Engine engine_;
    EngineStats stats_;  // copied once per frame in draw()
    TuneView tune_;
    int activeTab_ = 0;  // tab shown last frame: 0 Live, 1 Tune, 2 Closed loop, 3 MSN finder (picks the left panel)
#ifdef FP_CLOSEDLOOP
    ClosedLoopView closed_;  // spike-triggered closed loop; independent of engine_
    bool selectClTab_ = false;
#endif
#ifdef FP_MSN
    MsnFinderView msn_;  // unit finder for a Kilosort run; drives a Python helper
    bool selectMsnTab_ = false;
#endif
    std::string status_;
    bool selectTuneTab_ = false;
    int editSyl_ = 0;         // syllable being edited (shared with the Tune tab)
    int lastOpenedSyl_ = -1;  // to open the edited syllable's panel once when it changes
    ScopeSnapshot snap_;
    std::vector<double> xs_, ys_;
};

}  // namespace fp

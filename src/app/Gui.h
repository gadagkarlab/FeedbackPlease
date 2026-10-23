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

// What the app was launched as (see the launch screen in main.cpp).
enum class AppMode {
    Single,      // one box: Live + Tune
    ClosedLoop,  // one box: Live + Tune + Closed loop + MSN finder, closed-loop pulse feedback
    Multi,       // several boxes recording at once: Live (all boxes) + Tune
};
const char* toString(AppMode m);
const char* modeKey(AppMode m);  // "single" / "closedloop" / "multi"

class Gui {
public:
    Gui(std::string settingsPath, AppMode mode);
    ~Gui();
    void draw();  // one ImGui frame
    void startEngine();
    void openTune(const std::string& folder, bool analyze, bool summary);
    bool switchModeRequested() const { return switchMode_; }
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
    // Which parts of the settings "Apply to box" / "Load from box" copy.
    struct CopyParts {
        bool detection = true, syllables = true, song = true, feedback = false;
    };

    void drawToolbar();
    void drawBoxBar();
    void drawOverview();
    void drawTuneHeader();
    void drawControls();
    void drawHardware();
    void drawSpectrum();
    void drawProcessing();
    void drawTarget();
    void drawFeedback();
    void drawPulseFeedback();
    void drawSong();
    void drawCodes();
    void drawRecording();
    void drawDisplay();
    void drawStatus();
    void drawPlots(float height);
    void drawEventLog();

    bool loadSettings(const std::string& path, std::string* err);
    bool saveSettings(const std::string& path, std::string* err);
    Box* engineBox(int guiIndex);  // the running Box for boxes_[guiIndex], or nullptr
    void pushParams();
    void pushBox(int guiIndex);
    static void copyParts(const AppParams& from, AppParams& to, const CopyParts& c);
    std::string closedLoopPath() const;  // <settings file without .ini>.closedloop.txt
    std::string msnPath() const;         // <settings file without .ini>.msn.txt

    // The parameter set the left panel is editing: the selected box, or the tuning set.
    AppParams& P() { return *pp_; }
    bool tuning() const { return pp_ == &tuneP_; }

    std::string settingsPath_;
    AppMode mode_;
    std::vector<AppParams> boxes_;  // one in the single-box modes
    int selBox_ = 0;
    AppParams tuneP_;               // the Tune tab's own parameters
    AppParams* pp_ = nullptr;
    std::vector<int> engineIdx_;    // per box: index in the running engine, -1 if not running
    CopyParts applyParts_, loadParts_;
    std::vector<char> applyTo_;     // multi-box "Apply to box" choices
    bool switchMode_ = false;

    Engine engine_;
    EngineStats stats_;  // the selected box, copied once per frame in draw()
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
    int editTmpl_ = 0;        // spectral template being edited (shared with the Tune tab)
    void drawTemplates();
    void drawBirdName();
    int lastOpenedSyl_ = -1;  // to open the edited syllable's panel once when it changes
    ScopeSnapshot snap_;
    std::vector<double> xs_, ys_;
};

}  // namespace fp

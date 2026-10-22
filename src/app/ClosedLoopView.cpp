#include "app/ClosedLoopView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#include "app/Widgets.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "implot.h"

namespace fp {

using namespace ui;

namespace {

constexpr size_t kMaxLogShown = 3000;
constexpr size_t kMaxRowsShown = 5000;

const char* kBinFilter = "Binary recordings (*.bin)\0*.bin\0All files\0*.*\0";
const char* kExeFilter = "Programs (*.exe)\0*.exe\0All files\0*.*\0";
const char* kPyFilter = "Python scripts (*.py)\0*.py\0All files\0*.*\0";

enum class PathKind { File, Folder, SaveFile };

// Text box + "..." browse button. The label is shown to the right, like the other fields.
bool pathField(const char* label, std::string* s, PathKind kind, const char* filter = "All files\0*.*\0",
               float widthEm = 16.0f, const char* hint = nullptr) {
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(widthEm * em());
    bool r = hint ? ImGui::InputTextWithHint("##path", hint, s) : ImGui::InputText("##path", s);
    ImGui::SameLine();
    if (ImGui::Button("...")) {
        std::string f;
        if (kind == PathKind::Folder) f = pickFolderDialog("Select folder");
        else if (kind == PathKind::SaveFile) f = saveFileDialog(filter, "txt");
        else f = openFileDialog(filter);
        if (!f.empty()) {
            *s = f;
            r = true;
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    changed() |= r;
    return r;
}

void openPath(const std::string& path) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)path;
#endif
}

ImVec4 stateColor(ClosedLoopRunner::State s) {
    switch (s) {
        case ClosedLoopRunner::State::Live: return kGreen;
        case ClosedLoopRunner::State::Calibrating:
        case ClosedLoopRunner::State::Connecting: return kAmber;
        default: return kGrey;
    }
}

}  // namespace

ClosedLoopView::~ClosedLoopView() { shutdown(); }

void ClosedLoopView::shutdown() {
    runner_.stop();
    // The destructor joins; spin until the job has noticed (a calibration child is killed by stop()).
    for (int i = 0; i < 600 && runner_.busy(); ++i) {
#ifdef _WIN32
        Sleep(10);
#endif
    }
}

void ClosedLoopView::load(const std::string& path, std::string* status) {
    if (!std::filesystem::exists(path)) return;
    std::string err;
    ClParams np = p_;
    if (loadClParams(path, np, &err)) {
        p_ = np;
        if (status) *status = "Loaded " + path;
    } else if (status) {
        *status = "Closed loop settings: " + err;
    }
    filterKey_.clear();
}

bool ClosedLoopView::save(const std::string& path, std::string* err) const { return saveClParams(path, p_, err); }

std::string ClosedLoopView::statusText() const {
    const auto s = runner_.state();
    if (s == ClosedLoopRunner::State::Idle) return std::string();
    std::string t = std::string("Closed loop: ") + ClosedLoopRunner::stateName(s);
    if (s == ClosedLoopRunner::State::Live) {
        char b[64];
        std::snprintf(b, sizeof b, "  %.0f spikes/s", spikeRate_);
        t += b;
    }
    return t;
}

void ClosedLoopView::refreshFilterInfo(bool force) {
    const std::string key = p_.filterDir + "|" + std::to_string(p_.targetId) + "|" + std::to_string(p_.templateLength);
    const double now = ImGui::GetTime();
    if (!force && key == filterKey_ && now - filterCheckedAt_ < 2.0) return;
    filterKey_ = key;
    filterCheckedAt_ = now;
    filterInfo_ = inspectClFilter(p_);
}

// ---------------------------------------------------------------------------------------------
// Left panel: the config file

void ClosedLoopView::drawParams() {
    const bool busy = runner_.busy();
    const ImGuiTreeNodeFlags open = ImGuiTreeNodeFlags_DefaultOpen;
    if (busy) ImGui::TextColored(kAmber, "Settings are read at Start; stop to change them.");
    ImGui::BeginDisabled(busy);

    if (ImGui::CollapsingHeader("SpikeGLX connection")) {
        InputStr("Host", &p_.host, 12);
        InputI("Port", &p_.port);
    }

    if (ImGui::CollapsingHeader("Filter", open)) {
        pathField("Filter folder", &p_.filterDir, PathKind::Folder);
        HelpMarker("Where channels_<id>.bin, filter_<id>.bin and threshold_<id>.bin are read from and written to.");
        InputI("Target unit id", &p_.targetId);
        HelpMarker("Kilosort cluster id of the neuron to detect.");
        InputI("Template length", &p_.templateLength);
        p_.templateLength = std::max(1, p_.templateLength);
    }

    if (ImGui::CollapsingHeader("Preprocessing")) {
        InputStr("CAR shank", &p_.carShank, 6);
        HelpMarker("Common-average-reference group: 'auto' (the shank holding the target's template peak, saved by the "
                   "Python calibration), a 0-based shank index, or -1 to turn CAR off. CAR runs across the whole shank, not "
                   "just the filter's channels. Must match how the filter was trained.");
        Check("High-pass", &p_.applyHighpass);
        ImGui::BeginDisabled(!p_.applyHighpass);
        InputD("Cutoff (Hz)", &p_.highpassCutoffHz, "%.0f");
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Calibration", open)) {
        ImGui::SetNextItemWidth(8 * em());
        if (ImGui::BeginCombo("Backend", p_.calibrationBackend.c_str())) {
            for (const char* b : {"python", "cpp"})
                if (ImGui::Selectable(b, p_.calibrationBackend == b)) p_.calibrationBackend = b;
            ImGui::EndCombo();
        }
        HelpMarker("python: runs FilterGen/calibrate_for_closedloop.py (fits the noise-whitened filter and sweeps the threshold). "
                   "cpp: only re-sweeps the threshold of the saved filter; kept as a fallback.");
        pathField("Training .bin", &p_.trainingBinPath, PathKind::File, kBinFilter);
        HelpMarker("The recording Kilosort was run on (the pass without -apfilter). Must be the exact file behind the "
                   "Kilosort folder: ground-truth spike times are matched to it by sample index.");
        pathField("Kilosort folder", &p_.trainingKsDir, PathKind::Folder);
        const bool py = p_.calibrationBackend == "python";
        if (py) {
            pathField("Python", &p_.pythonExe, PathKind::File, kExeFilter, 16.0f, "path to python.exe");
            HelpMarker("Full path to the interpreter that has numpy and scipy (e.g. a conda env's python.exe). A bare "
                       "'python' may resolve to the wrong one.");
            const std::string def = clDefaultCalibrationScript();
            pathField("Script", &p_.calibrationScript, PathKind::File, kPyFilter, 16.0f, def.c_str());
            HelpMarker("Empty = the default shown. Relative paths are taken from the folder of FeedbackPlease.exe.");
            InputI("Nearest channels", &p_.nearestChans);
            HelpMarker("Fit and score on this many channels closest to the target's peak (Kilosort's default is 10).");
            InputI("Filter channels", &p_.filterNChannels);
            HelpMarker("How many of those the filter itself uses.");
            InputI("Template offset", &p_.templateOffset);
            InputD("Train fraction", &p_.calibrationTrainFrac, "%.2f");
            p_.calibrationTrainFrac = std::clamp(p_.calibrationTrainFrac, 0.05, 0.95);
            HelpMarker("The filter is fit on this first share of the recording; the threshold is swept on the rest.");
            InputD("Max minutes", &p_.calibrationMaxMinutes, "%.1f");
            HelpMarker("Only use the first N minutes of the recording. 0 = all of it.");
            InputD("Ridge", &p_.calibrationRidge, "%.3g");
            InputI("Max spikes", &p_.calibrationMaxSpikes);
            InputI("Seed", &p_.calibrationSeed);
        } else {
            pathField("Log CSV", &p_.calibrationLogPath, PathKind::SaveFile);
            ImGui::SetNextItemWidth(14 * em());
            if (ImGui::BeginCombo("Criterion", p_.calibrationCriterion.c_str())) {
                for (const char* c : {"best_f1", "best_recall_under_fp_rate"})
                    if (ImGui::Selectable(c, p_.calibrationCriterion == c)) p_.calibrationCriterion = c;
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(p_.calibrationCriterion != "best_recall_under_fp_rate");
            InputD("Max FP rate (Hz)", &p_.maxFalsePositiveRateHz, "%.3g");
            ImGui::EndDisabled();
        }
    }

    if (ImGui::CollapsingHeader("Fetch cadence")) {
        InputI("Chunk (ms)", &p_.fetchChunkMs);
        p_.fetchChunkMs = std::max(1, p_.fetchChunkMs);
        InputD("Stream timeout (s)", &p_.streamTimeoutSec, "%.1f");
        HelpMarker("Stop if a SpikeGLX stream delivers no data for this long (SpikeGLX stopped or restarted its run). 0 = keep retrying.");
    }

    if (ImGui::CollapsingHeader("NI syllable lines")) {
        InputStr("Lines", &p_.niSyllableLines, 8);
        HelpMarker("NI digital lines carrying the syllable code, contiguous and ascending, e.g. 5,6,7. Wire them to "
                   "the syllable code port that the Live tab writes (Hardware / Source > Syllable code port).");
        InputI("Debounce (samples)", &p_.niDebounceSamples);
        HelpMarker("An onset fires once a nonzero code has held this many NI samples.");
        InputD("Re-arm (ms)", &p_.niRearmMs, "%.2f");
        HelpMarker("After an onset the lines must be back at 0 for this long before another can fire. Keep it below the shortest gap between pulses.");
        InputD("Refractory (ms)", &p_.niRefractoryMs, "%.1f");
        HelpMarker("Minimum time between onsets. Keep it below the shortest real inter-onset interval.");
    }

    if (ImGui::CollapsingHeader("Decision", open)) {
        InputI("Target syllable code", &p_.targetSyllable);
        p_.targetSyllable = std::max(-1, p_.targetSyllable);
        HelpMarker("Only onsets of this code open a decision window. -1 = every nonzero code.");
        InputD("Window centre (ms)", &p_.windowCenterMs, "%.1f");
        HelpMarker("Centre of the whole span of sub-windows, in ms after syllable onset.");
        InputD("Sub-window (ms)", &p_.subWinDurMs, "%.1f");
        InputD("Overlap (ms)", &p_.subWinOverlapMs, "%.1f");
        InputI("# sub-windows", &p_.numSubWin);
        p_.numSubWin = std::max(1, p_.numSubWin);
        const ClWindow w = clWindow(p_);
        if (w.valid) {
            std::string s;
            char b[64];
            for (int k = 0; k < p_.numSubWin && k < 12; ++k) {
                std::snprintf(b, sizeof b, "%s[%.0f, %.0f]", k ? " " : "", w.startMs + k * w.stepMs,
                              w.startMs + k * w.stepMs + p_.subWinDurMs);
                s += b;
            }
            ImGui::TextDisabled("span %.0f ms: %.0f to %.0f ms after onset", w.spanMs, w.startMs, w.endMs);
            ImGui::TextDisabled("%s ms", s.c_str());
        } else {
            ImGui::TextColored(kRed, "Overlap must be >= 0 and < sub-window length.");
        }
        ImGui::SetNextItemWidth(8 * em());
        if (ImGui::BeginCombo("Mode", p_.decisionMode.c_str())) {
            for (const char* m : {"push_down", "push_up"})
                if (ImGui::Selectable(m, p_.decisionMode == m)) p_.decisionMode = m;
            ImGui::EndCombo();
        }
        HelpMarker("push_down: pulse as soon as the spike count in any 2 consecutive sub-windows reaches the threshold.\n"
                   "push_up: pulse when EVERY pair of consecutive sub-windows has <= threshold spikes; decided right after the window ends.");
        InputD("Spike count threshold", &p_.spikeCountThreshold, "%.2f");
        InputI("Adaptive renditions", &p_.adaptiveRenditions);
        p_.adaptiveRenditions = std::max(0, p_.adaptiveRenditions);
        HelpMarker("Keep the threshold above for the first N renditions of the target syllable, then use the median of the last N "
                   "renditions' counts. 0 = fixed.");
        InputStr("Digital line", &p_.doLine, 14);
        HelpMarker("SpikeGLX NI line raised on a decision, e.g. Dev4/port0/line5.");
        InputI("Pulse (ms)", &p_.doPulseMs);
        p_.doPulseMs = std::max(1, p_.doPulseMs);
    }

    if (ImGui::CollapsingHeader("Output files")) {
        pathField("Spike times", &p_.spikeTimesPath, PathKind::SaveFile);
        HelpMarker("Sample index of every detected spike. Appended to. Empty = not written.");
        pathField("Syllable times", &p_.syllableTimesPath, PathKind::SaveFile);
        HelpMarker("One 'code,niSample,imecSample' line per syllable onset. Empty = not written.");
        pathField("Decision log", &p_.decisionLogPath, PathKind::SaveFile);
        HelpMarker("One line per syllable with every spike's time relative to onset, plus a TRIGGER line for each pulse. Empty = not written.");
        InputD("Run for (s)", &p_.runSeconds, "%.0f");
        HelpMarker("Stop by itself after this long. 0 = until Stop.");
    }

    ImGui::Separator();
    if (ImGui::Button("Load config...")) {
        const std::string f = openFileDialog("ClosedLoop config (*.txt)\0*.txt\0All files\0*.*\0");
        if (!f.empty()) {
            std::string st;
            load(f, &st);
            message_ = st;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Export config...")) {
        const std::string f = saveFileDialog("ClosedLoop config (*.txt)\0*.txt\0All files\0*.*\0", "txt");
        if (!f.empty()) {
            std::string e;
            message_ = save(f, &e) ? "Exported " + f : e;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(same format as ClosedLoop.exe)");

    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------------------------
// Right side

void ClosedLoopView::drawMain() {
    // New log lines / decisions since the last frame.
    {
        std::vector<std::string> add;
        logCursor_ = runner_.fetchLog(logCursor_, add);
        for (auto& l : add) log_.push_back(std::move(l));
        if (log_.size() > kMaxLogShown) log_.erase(log_.begin(), log_.begin() + (log_.size() - kMaxLogShown));
        std::vector<ClDecisionRow> rows;
        const size_t end = runner_.fetchDecisions(rowCursor_, rows);
        if (end < rowCursor_) {  // a new run restarted the numbering
            rows_.clear();
            rows.clear();
            rowCursor_ = runner_.fetchDecisions(0, rows);
        } else {
            rowCursor_ = end;
        }
        for (auto& r : rows) rows_.push_back(r);
        if (rows_.size() > kMaxRowsShown) rows_.erase(rows_.begin(), rows_.begin() + (rows_.size() - kMaxRowsShown));
    }
    if (runner_.takeCalibrationDone()) refreshFilterInfo(true);
    refreshFilterInfo(false);

    // Spike / syllable rates, over ~1 s.
    {
        const double now = ImGui::GetTime();
        if (now - lastT_ >= 1.0) {
            const ClCounters c = runner_.counters();
            if (runner_.state() == ClosedLoopRunner::State::Live && lastT_ > 0 && c.spikes >= last_.spikes) {
                spikeRate_ = (c.spikes - last_.spikes) / (now - lastT_);
                syllRate_ = (c.syllables - last_.syllables) / (now - lastT_);
            } else {
                spikeRate_ = syllRate_ = 0;
            }
            last_ = c;
            lastT_ = now;
        }
    }

    drawControls();
    ImGui::Separator();
    drawCounters();
    const float avail = ImGui::GetContentRegionAvail().y;
    drawPlots(std::max(10.0f * em(), avail * 0.34f));
    drawDecisions();
    drawLog();
}

void ClosedLoopView::drawControls() {
    const auto st = runner_.state();
    const bool busy = st != ClosedLoopRunner::State::Idle;

    ImGui::BeginDisabled(busy);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.5f, 0.2f, 1));
    if (ImGui::Button("  Start live  ")) {
        std::string err;
        if (runner_.startLive(p_, !p_.skipCalibration, err))
            message_.clear();
        else
            message_ = err;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Run calibration")) {
        std::string err;
        if (runner_.startCalibration(p_, err))
            message_.clear();
        else
            message_ = err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Test pulse")) {
        std::string err;
        if (runner_.startPulseTest(p_, err))
            message_.clear();
        else
            message_ = err;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!busy || st == ClosedLoopRunner::State::Stopping);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.15f, 1));
    if (ImGui::Button("  Stop  ")) runner_.stop();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextColored(stateColor(st), "%s", ClosedLoopRunner::stateName(st));

    ImGui::BeginDisabled(busy);
    bool calFirst = !p_.skipCalibration;
    if (ImGui::Checkbox("Calibrate before going live", &calFirst)) p_.skipCalibration = !calFirst;
    ImGui::EndDisabled();
    HelpMarker("On: Start live first refits the filter and threshold from the training data, then connects. "
               "Off: uses the filter already saved in the filter folder (starts in about a second).");

    ImGui::TextUnformatted("Saved filter:");
    ImGui::SameLine();
    ImGui::TextColored(filterInfo_.ok ? kGreen : kAmber, "%s", filterInfo_.text.c_str());
    if (!p_.filterDir.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Open folder")) openPath(p_.filterDir);
        const std::string id = std::to_string(p_.targetId);
        const std::string pngs[2] = {p_.filterDir + "/template_filter_" + id + ".png",
                                     p_.filterDir + "/threshold_sweep_" + id + ".png"};
        const char* names[2] = {"Template/filter plot", "Threshold sweep plot"};
        for (int i = 0; i < 2; ++i) {
            if (!std::filesystem::exists(pngs[i])) continue;
            ImGui::SameLine();
            if (ImGui::SmallButton(names[i])) openPath(pngs[i]);
        }
    }

    const std::string err = runner_.lastError();
    if (!err.empty() && !busy) ImGui::TextColored(kRed, "%s", err.c_str());
    else if (!message_.empty()) ImGui::TextColored(kAmber, "%s", message_.c_str());
}

void ClosedLoopView::drawCounters() {
    const ClCounters c = runner_.counters();
    const bool live = runner_.state() == ClosedLoopRunner::State::Live;
    if (!live && c.decisions == 0 && c.spikes == 0 && c.syllables == 0) {
        ImGui::TextDisabled("Not live. Start live connects to SpikeGLX (it must be acquiring) and starts the loop.");
        return;
    }
    ImGui::Text("SpikeGLX %s  IMEC %.0f Hz  t = %.1f s", c.sglxVersion.c_str(), c.imecRate, c.elapsedSec);
    ImGui::Text("spikes %lld (%.1f/s)   syllables %lld (%.1f/s)", c.spikes, spikeRate_, c.syllables, syllRate_);
    if (c.syllablesLost) {
        ImGui::SameLine();
        ImGui::TextColored(kRed, " lost %lld (sync mapping failed)", c.syllablesLost);
    }
    ImGui::Text("windows %lld   pulses %lld (%.0f%%)   stalled %lld   threshold now %.2f", c.decisions, c.triggers,
                c.decisions ? 100.0 * c.triggers / c.decisions : 0.0, c.stalled, c.threshold);
    if (live && c.syllables == 0 && c.elapsedSec > 5.0)
        ImGui::TextColored(kAmber, "No syllable onsets seen yet: check that the code port is wired to the NI lines %s.",
                           p_.niSyllableLines.c_str());
}

void ClosedLoopView::drawPlots(float height) {
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const ClWindow win = clWindow(p_);

    // ---- spike count of every rendition against the threshold
    if (ImPlot::BeginPlot("Spike count per rendition", ImVec2(w, height))) {
        ImPlot::SetupAxes("rendition", "spikes", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);
        const size_t n = std::min<size_t>(rows_.size(), 200);
        std::vector<double> x, y, th, fx, fy;
        for (size_t i = rows_.size() - n; i < rows_.size(); ++i) {
            const ClDecisionRow& r = rows_[i];
            x.push_back(double(r.seq));
            y.push_back(r.count);
            th.push_back(r.threshold);
            if (r.triggered) {
                fx.push_back(double(r.seq));
                fy.push_back(r.count);
            }
        }
        if (!x.empty()) {
            ImPlot::SetNextLineStyle(kGrey, 1.0f);
            ImPlot::PlotStairs("threshold", x.data(), th.data(), int(x.size()));
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 4, kBlue, -1, kBlue);
            ImPlot::PlotScatter("count", x.data(), y.data(), int(x.size()));
            if (!fx.empty()) {
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6, ImVec4(0, 0, 0, 0), 2.0f, kRed);
                ImPlot::PlotScatter("pulse", fx.data(), fy.data(), int(fx.size()));
            }
        }
        ImPlot::EndPlot();
    }
    ImGui::SameLine();

    // ---- raster: one row per rendition, spike times relative to the syllable onset
    if (ImPlot::BeginPlot("Spikes after syllable onset", ImVec2(-1, height))) {
        ImPlot::SetupAxes("ms after onset", "rendition", 0, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupLegend(ImPlotLocation_NorthEast, ImPlotLegendFlags_Horizontal);
        double lo = 0, hi = 100;
        if (win.valid) {
            lo = std::min(0.0, win.startMs) - 10;
            hi = win.endMs + 10;
        }
        ImPlot::SetupAxisLimits(ImAxis_X1, lo, hi, ImPlotCond_Once);
        std::vector<double> sx, sy, px, py;
        const size_t n = std::min<size_t>(rows_.size(), 100);
        for (size_t i = rows_.size() - n; i < rows_.size(); ++i) {
            const ClDecisionRow& r = rows_[i];
            for (double ms : r.spikesMs) {
                sx.push_back(ms);
                sy.push_back(double(r.seq + 1));
            }
            if (r.triggered && std::isfinite(r.pulseMs)) {
                px.push_back(r.pulseMs);
                py.push_back(double(r.seq + 1));
            }
        }
        if (win.valid) {
            std::vector<double> edges;
            for (int k = 0; k < p_.numSubWin; ++k) {
                edges.push_back(win.startMs + k * win.stepMs);
                edges.push_back(win.startMs + k * win.stepMs + p_.subWinDurMs);
            }
            ImPlot::SetNextLineStyle(ImVec4(kAmber.x, kAmber.y, kAmber.z, 0.5f), 1.0f);
            ImPlot::PlotInfLines("sub-windows", edges.data(), int(edges.size()));
            const double ends[2] = {win.startMs, win.endMs};
            ImPlot::SetNextLineStyle(kGreen, 2.0f);
            ImPlot::PlotInfLines("window", ends, 2);
        }
        if (!sx.empty()) {
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Square, 3, kBlue, -1, kBlue);
            ImPlot::PlotScatter("spikes", sx.data(), sy.data(), int(sx.size()));
        }
        if (!px.empty()) {
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Diamond, 8, kRed, 1.0f, kRed);
            ImPlot::PlotScatter("pulse sent", px.data(), py.data(), int(px.size()));
        }
        ImPlot::EndPlot();
    }
}

void ClosedLoopView::drawDecisions() {
    const float h = std::min(9.0f * em(), ImGui::GetContentRegionAvail().y * 0.3f);
    if (!ImGui::BeginTable("decisions", 7,
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                               ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, h)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("code");
    ImGui::TableSetupColumn("count");
    ImGui::TableSetupColumn("window total");
    ImGui::TableSetupColumn("threshold");
    ImGui::TableSetupColumn("result");
    ImGui::TableSetupColumn("onset to pulse (ms)");
    ImGui::TableHeadersRow();
    for (auto it = rows_.rbegin(); it != rows_.rend(); ++it) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%lld", it->seq + 1);
        ImGui::TableNextColumn();
        ImGui::Text("%d", it->code);
        ImGui::TableNextColumn();
        ImGui::Text("%d", it->count);
        ImGui::TableNextColumn();
        ImGui::Text("%d", it->totalCount);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", it->threshold);
        ImGui::TableNextColumn();
        if (it->stalled) ImGui::TextColored(kAmber, "stalled");
        else if (it->triggered) ImGui::TextColored(kRed, "PULSE");
        else ImGui::TextDisabled("no pulse");
        ImGui::TableNextColumn();
        if (it->triggered && std::isfinite(it->pulseMs)) ImGui::Text("%.1f", it->pulseMs);
    }
    ImGui::EndTable();
}

void ClosedLoopView::drawLog() {
    if (ImGui::SmallButton("Clear log")) log_.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy log")) {
        std::string all;
        for (const auto& l : log_) all += l + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &logAutoscroll_);
    ImGui::BeginChild("cl_log", ImVec2(0, 0), ImGuiChildFlags_Border);
    ImGuiListClipper clip;
    clip.Begin(int(log_.size()));
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const std::string& l = log_[size_t(i)];
            const bool bad = l.rfind("ERROR", 0) == 0 || l.find("FAILED") != std::string::npos ||
                             l.find("failed") != std::string::npos;
            const bool head = l.rfind("===", 0) == 0;
            ImGui::PushStyleColor(ImGuiCol_Text, bad ? kRed : head ? kAmber : ImVec4(0.8f, 0.8f, 0.8f, 1));
            ImGui::TextUnformatted(l.c_str());
            ImGui::PopStyleColor();
        }
    }
    clip.End();
    if (logAutoscroll_) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

}  // namespace fp

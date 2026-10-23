#include "app/Gui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "app/Widgets.h"
#include "core/NoiseBurst.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "implot.h"

namespace fp {

using namespace ui;

namespace {

ImVec4 stateColor(FbState s) {
    switch (s) {
        case FbState::Listening: return kGreen;
        case FbState::RefractoryPeriod: return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        case FbState::SendingTrigger1:
        case FbState::SendingTrigger2: return kRed;
        default: return kAmber;
    }
}

const ImVec4 kDim(0.6f, 0.6f, 0.6f, 1.0f);

}  // namespace

const char* toString(AppMode m) {
    switch (m) {
        case AppMode::Single: return "Single box";
        case AppMode::ClosedLoop: return "Single box + closed loop";
        case AppMode::Multi: return "Multi-box";
    }
    return "?";
}

const char* modeKey(AppMode m) {
    switch (m) {
        case AppMode::Single: return "single";
        case AppMode::ClosedLoop: return "closedloop";
        case AppMode::Multi: return "multi";
    }
    return "single";
}

Gui::Gui(std::string settingsPath, AppMode mode) : settingsPath_(std::move(settingsPath)), mode_(mode) {
    boxes_.resize(1);
    tuneP_ = boxes_[0];
    if (std::filesystem::exists(std::filesystem::u8path(settingsPath_))) {
        std::string err;
        if (loadSettings(settingsPath_, &err))
            status_ = "Loaded " + settingsPath_;
        else
            status_ = "Settings: " + err;
    }
    if (!daqAvailable())
        for (auto& b : boxes_) b.source.kind = SourceKind::Simulated;
#ifdef FP_CLOSEDLOOP
    if (mode_ == AppMode::ClosedLoop) {
        std::string clStatus;
        closed_.load(closedLoopPath(), &clStatus);
        if (status_.empty()) status_ = clStatus;
    }
#endif
#ifdef FP_MSN
    if (mode_ == AppMode::ClosedLoop) msn_.load(msnPath(), nullptr);
#endif
}

Gui::~Gui() {
    engine_.stop();
#ifdef FP_CLOSEDLOOP
    if (mode_ == AppMode::ClosedLoop) {
        closed_.shutdown();
        closed_.save(closedLoopPath());
    }
#endif
#ifdef FP_MSN
    if (mode_ == AppMode::ClosedLoop) {
        msn_.shutdown();
        msn_.save(msnPath());
    }
#endif
    saveSettings(settingsPath_, nullptr);
}

// ---- settings files
// Single-box modes: the box's keys unprefixed (as before) + the tuning set under "tune.".
// Multi-box: "boxes.count", each box under "box1.", "box2.", ... + "tune.". Either kind
// of file can be opened in either mode (a single-box file becomes box 1, a multi-box
// file's box 1 becomes the single box).
bool Gui::loadSettings(const std::string& path, std::string* err) {
    ParamMap kv;
    if (!readParamFile(path, kv, err)) return false;
    std::string e;
    std::vector<AppParams> boxes;
    const bool multiFile = kv.count("boxes.count") > 0;
    if (mode_ == AppMode::Multi && multiFile) {
        int n = 1;
        try {
            n = std::clamp(std::stoi(kv["boxes.count"]), 1, 64);
        } catch (...) {
        }
        boxes.resize(size_t(n));
        for (int i = 0; i < n; ++i) {
            boxes[size_t(i)].boxName = "Box " + std::to_string(i + 1);
            e += readParams(kv, boxes[size_t(i)], "box" + std::to_string(i + 1) + ".");
        }
    } else {
        boxes.resize(1);
        e += readParams(kv, boxes[0], multiFile ? "box1." : "");
    }
    // A file without a tuning set (older files) starts tuning from its first box.
    AppParams tune = boxes[0];
    if (hasSection(kv, "tune.")) e += readParams(kv, tune, "tune.");
    boxes_ = std::move(boxes);
    tuneP_ = std::move(tune);
    selBox_ = std::clamp(selBox_, 0, int(boxes_.size()) - 1);
    if (err) *err = e;
    return e.empty();
}

bool Gui::saveSettings(const std::string& path, std::string* err) {
    std::ofstream out(std::filesystem::u8path(path));
    if (!out) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    out << "# FeedbackPlease settings (" << toString(mode_) << ")\n";
    if (mode_ == AppMode::Multi) {
        out << "boxes.count = " << boxes_.size() << "\n";
        for (size_t i = 0; i < boxes_.size(); ++i) writeParams(out, boxes_[i], "box" + std::to_string(i + 1) + ".");
    } else {
        writeParams(out, boxes_[0]);
    }
    writeParams(out, tuneP_, "tune.");
    return true;
}

// ---- engine

Box* Gui::engineBox(int i) {
    return i >= 0 && i < int(engineIdx_.size()) && engineIdx_[size_t(i)] >= 0 ? engine_.box(engineIdx_[size_t(i)])
                                                                               : nullptr;
}

void Gui::startEngine() {
    std::vector<AppParams> ps;
    engineIdx_.assign(boxes_.size(), -1);
    for (size_t i = 0; i < boxes_.size(); ++i) {
        if (mode_ == AppMode::Multi && !boxes_[i].boxEnabled) continue;
        AppParams p = boxes_[i];
        if (mode_ != AppMode::ClosedLoop) p.pulse.enabled = false;  // the pulse input is a closed-loop feature
        engineIdx_[i] = int(ps.size());
        ps.push_back(p);
    }
    std::string err;
    if (ps.empty())
        status_ = "Start failed: no box is enabled";
    else if (engine_.start(ps, mode_ != AppMode::Multi, err))
        status_ = mode_ == AppMode::Multi ? "Running " + std::to_string(ps.size()) + " box(es)" : "Running";
    else
        status_ = "Start failed: " + err;
    if (!engine_.running()) engineIdx_.assign(boxes_.size(), -1);
}

void Gui::pushBox(int i) {
    if (Box* b = engineBox(i)) b->updateParams(boxes_[size_t(i)]);
}

void Gui::pushParams() {
    if (tuning()) return;  // the tuning set never reaches a running box directly
    pushBox(selBox_);
}

void Gui::openTune(const std::string& folder, bool analyze, bool summary) {
    tune_.openFolder(folder, analyze || summary, tuneP_, summary);
    selectTuneTab_ = true;
}

std::string Gui::closedLoopPath() const {
    std::string base = settingsPath_;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".ini") == 0) base.resize(base.size() - 4);
    return base + ".closedloop.txt";
}

#ifdef FP_MSN
std::string Gui::msnPath() const {
    std::string base = settingsPath_;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".ini") == 0) base.resize(base.size() - 4);
    return base + ".msn.txt";
}
#endif

// ---- tuning set <-> boxes

void Gui::copyParts(const AppParams& from, AppParams& to, const CopyParts& c) {
    if (c.detection) {
        to.spectrum = from.spectrum;
        to.proc = from.proc;
    }
    if (c.syllables) {
        to.codes.list = from.codes.list;  // the box keeps its own code-port settings
        to.codes.target = from.codes.target;
        to.tmpl = from.tmpl;  // detection method + spectral templates
    }
    if (c.song) to.song = from.song;
    if (c.feedback) {
        const std::string w1 = to.fb.wave1, w2 = to.fb.wave2;  // stimulus files belong to the box
        to.fb = from.fb;
        to.fb.wave1 = w1;
        to.fb.wave2 = w2;
    }
}

void Gui::draw() {
    changed() = false;
    selBox_ = std::clamp(selBox_, 0, int(boxes_.size()) - 1);
    pp_ = activeTab_ == 1 ? &tuneP_ : &boxes_[size_t(selBox_)];
    const Box* box = engineBox(selBox_);
    stats_ = box ? box->stats() : EngineStats{};
    if (!engine_.running() && !engineIdx_.empty() && engine_.boxCount() == 0) engineIdx_.clear();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    drawToolbar();
    ImGui::Separator();

    ImGui::BeginChild("controls", ImVec2(31.0f * em(), 0), ImGuiChildFlags_Border);
    // The left panel follows the tab. Edits in the closed-loop and MSN panels are not engine parameters,
    // so they must not trigger a parameter push.
    const bool beforeChanged = changed();
    if (false) {
    }
#ifdef FP_CLOSEDLOOP
    else if (activeTab_ == 2) {
        closed_.drawParams();
        changed() = beforeChanged;
    }
#endif
#ifdef FP_MSN
    else if (activeTab_ == 3) {
        msn_.drawParams();
        changed() = beforeChanged;
    }
#endif
    else if (activeTab_ == 1) {
        drawTuneHeader();
        drawControls();
    } else {
        if (mode_ == AppMode::Multi) drawBoxBar();
        drawControls();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("right");
    if (ImGui::BeginTabBar("views")) {
        int tab = activeTab_;
        if (ImGui::BeginTabItem("Live")) {
            tab = 0;
            if (mode_ == AppMode::Multi) drawOverview();
            drawStatus();
            const float h = ImGui::GetContentRegionAvail().y;
            drawPlots(h * 0.80f);
            drawEventLog();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Tune on recordings", nullptr, selectTuneTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            tab = 1;
            tune_.draw(tuneP_, editSyl_, editTmpl_);
            ImGui::EndTabItem();
        }
#ifdef FP_CLOSEDLOOP
        if (mode_ == AppMode::ClosedLoop) {
            const std::string label =
                std::string("Closed loop") + (closed_.state() == ClosedLoopRunner::State::Live ? " [live]" : "") + "###cltab";
            if (ImGui::BeginTabItem(label.c_str(), nullptr, selectClTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
                tab = 2;
                const bool before = changed();
                closed_.drawMain();
                changed() = before;
                ImGui::EndTabItem();
            }
        }
        selectClTab_ = false;
#endif
#ifdef FP_MSN
        if (mode_ == AppMode::ClosedLoop) {
            if (ImGui::BeginTabItem("MSN finder", nullptr, selectMsnTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
                tab = 3;
                const bool before = changed();
                msn_.drawMain();
                changed() = before;
                ImGui::EndTabItem();
            }
        }
        selectMsnTab_ = false;
#endif
        activeTab_ = tab;
        selectTuneTab_ = false;
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::End();

    if (changed()) pushParams();
}

void Gui::drawToolbar() {
    const bool running = engine_.running();
    // The engine stops itself on a fatal read error; surface it once.
    const std::string err = engine_.error();
    if (!running && !err.empty() && status_.rfind("Stopped", 0) != 0) status_ = "Stopped: " + err;

    ImGui::PushStyleColor(ImGuiCol_Button, running ? ImVec4(0.65f, 0.15f, 0.15f, 1) : ImVec4(0.15f, 0.5f, 0.2f, 1));
    const char* startLabel = mode_ == AppMode::Multi ? (running ? " Stop all " : " Start all ") : (running ? "  Stop  " : "  Start  ");
    if (ImGui::Button(startLabel, ImVec2(6.5f * em(), 0))) {
        if (running) {
            engine_.stop();
            status_ = "Stopped";
        } else {
            startEngine();
        }
    }
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::TextColored(kBlue, "%s", toString(mode_));
    if (mode_ != AppMode::Multi) {
        ImGui::SameLine();
        ImGui::BeginDisabled(running);
        EnumCombo("Source", &boxes_[0].source.kind, 2, 14.0f);
        ImGui::EndDisabled();
    }

    // Test buttons act on the selected box.
    Box* box = engineBox(selBox_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!box);
    if (ImGui::Button("Test feedback 1")) box->manualFire(1);
    ImGui::SameLine();
    if (ImGui::Button("Test feedback 2")) box->manualFire(2);
    if (mode_ == AppMode::ClosedLoop) {
        ImGui::SameLine();
        if (ImGui::Button("Test pulse feedback")) box->manualFire(3);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset counters")) box->resetCounters();
    ImGui::EndDisabled();
    if (mode_ == AppMode::Multi && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Test and reset act on the selected box: %s", boxes_[size_t(selBox_)].boxName.c_str());

    // Settings files, e.g. one per bird (single box) or per rig (multi-box). The file in use is saved again on exit.
    const char* iniFilter = "Settings (*.ini)\0*.ini\0All files\0*.*\0";
    auto saveSide = [&](std::string& e) {
#ifdef FP_CLOSEDLOOP
        if (mode_ == AppMode::ClosedLoop && !closed_.save(closedLoopPath(), &e)) status_ = e;
#endif
#ifdef FP_MSN
        if (mode_ == AppMode::ClosedLoop && !msn_.save(msnPath(), &e)) status_ = e;
#endif
        (void)e;
    };
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        std::string e;
        status_ = saveSettings(settingsPath_, &e) ? "Saved " + settingsPath_ : e;
        saveSide(e);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) {
        const std::string f = saveFileDialog(iniFilter, "ini");
        std::string e;
        if (!f.empty()) {
            if (saveSettings(f, &e)) {
                settingsPath_ = f;
                status_ = "Saved " + f;
                saveSide(e);
            } else {
                status_ = e;
            }
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Load...")) {
        const std::string f = openFileDialog(iniFilter);
        if (!f.empty()) {
            std::string e;
            if (loadSettings(f, &e)) {
                settingsPath_ = f;
                status_ = "Loaded " + f;
#ifdef FP_CLOSEDLOOP
                if (mode_ == AppMode::ClosedLoop) {
                    closed_.reset();
                    closed_.load(closedLoopPath(), &status_);
                }
#endif
#ifdef FP_MSN
                if (mode_ == AppMode::ClosedLoop) {
                    msn_.reset();
                    msn_.load(msnPath());
                }
#endif
            } else {
                status_ = "Load failed: " + e;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Switch mode...")) switchMode_ = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Save these settings and go back to the launch screen (stop acquisition first).");

#ifdef FP_CLOSEDLOOP
    if (mode_ == AppMode::ClosedLoop) {
        const std::string cl = closed_.statusText();
        if (!cl.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(closed_.state() == ClosedLoopRunner::State::Live ? kGreen : kAmber, "| %s", cl.c_str());
        }
    }
#endif
    ImGui::SameLine();
    const bool bad = status_.find("fail") != std::string::npos || status_.rfind("Stopped:", 0) == 0;
    ImGui::TextColored(bad ? kRed : ImVec4(0.7f, 0.7f, 0.7f, 1), "%s", status_.c_str());
}

// Multi-box: pick / add / remove / name the box shown in the panel below.
void Gui::drawBoxBar() {
    const bool running = engine_.running();
    ImGui::SetNextItemWidth(12 * em());
    if (ImGui::BeginCombo("Box", boxes_[size_t(selBox_)].boxName.c_str())) {
        for (int i = 0; i < int(boxes_.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(boxes_[size_t(i)].boxName.c_str(), i == selBox_)) selBox_ = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(running);
    ImGui::SameLine();
    if (ImGui::Button("+ Add")) {
        AppParams b = boxes_[size_t(selBox_)];  // a copy of the current box is the quickest start
        b.boxName = "Box " + std::to_string(boxes_.size() + 1);
        boxes_.push_back(b);
        selBox_ = int(boxes_.size()) - 1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove") && boxes_.size() > 1) {
        boxes_.erase(boxes_.begin() + selBox_);
        selBox_ = std::min(selBox_, int(boxes_.size()) - 1);
    }
    ImGui::EndDisabled();
    AppParams& b = boxes_[size_t(selBox_)];
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(12 * em());
    ImGui::InputText("Name", &b.boxName);
    ImGui::SameLine();
    ImGui::Checkbox("Include in Start", &b.boxEnabled);
    ImGui::EndDisabled();
    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Stop to add, remove or rename boxes. Detection settings apply live.");
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

// Multi-box Live tab: one row per box; click a row to show that box below.
void Gui::drawOverview() {
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const float h = std::min(ImGui::GetContentRegionAvail().y * 0.35f, rowH * float(boxes_.size() + 2));
    if (!ImGui::BeginTable("boxes", 10,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, h)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* c : {"box", "state", "mic", "song", "onsets", "target", "feedback", "code", "backlog", "error"})
        ImGui::TableSetupColumn(c);
    ImGui::TableHeadersRow();
    for (int i = 0; i < int(boxes_.size()); ++i) {
        const AppParams& p = boxes_[size_t(i)];
        const Box* b = engineBox(i);
        const EngineStats s = b ? b->stats() : EngineStats{};
        const std::string e = b ? b->error() : std::string();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(i);
        if (ImGui::Selectable(p.boxName.c_str(), i == selBox_, ImGuiSelectableFlags_SpanAllColumns)) selBox_ = i;
        ImGui::PopID();
        ImGui::TableNextColumn();
        if (!p.boxEnabled)
            ImGui::TextColored(kDim, "excluded");
        else if (s.running)
            ImGui::TextColored(kGreen, "running");
        else
            ImGui::TextColored(kDim, "stopped");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.source.kind == SourceKind::Daq ? p.hw.aiChannel.c_str() : "simulated");
        ImGui::TableNextColumn();
        ImGui::TextColored(s.songActive ? kBlue : kDim, "%s%s", s.songActive ? "SONG" : "-", s.songFileOpen ? " REC" : "");
        ImGui::TableNextColumn();
        ImGui::Text("%llu", (unsigned long long)s.onsets);
        ImGui::TableNextColumn();
        ImGui::TextColored(s.matches ? kPurple : kDim, "%llu", (unsigned long long)s.matches);
        ImGui::TableNextColumn();
        ImGui::TextColored(s.fires1 + s.fires2 ? kRed : kDim, "%llu", (unsigned long long)(s.fires1 + s.fires2));
        ImGui::TableNextColumn();
        ImGui::Text("%d", s.currentCode);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f ms", s.backlogMs);
        ImGui::TableNextColumn();
        if (!e.empty()) ImGui::TextColored(kRed, "%s", e.c_str());
    }
    ImGui::EndTable();
    ImGui::TextColored(kBlue, "%s", boxes_[size_t(selBox_)].boxName.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(selected; its settings are in the left panel)");
}

// Tune tab: the tuning set is separate from the boxes; copy it to / from them explicitly.
void Gui::drawTuneHeader() {
    ImGui::TextColored(kBlue, "Tuning parameters");
    HelpMarker("These settings belong to the Tune tab only: changing them never affects a box, running or not. "
               "Use 'Apply to box' to copy what you tuned to a box, and 'Load from box' to start from a box's "
               "current settings.");
    auto partsUi = [&](CopyParts& c) {
        ImGui::Checkbox("Detection (bands, gains, thresholds)", &c.detection);
        ImGui::Checkbox("Syllables / templates & target", &c.syllables);
        ImGui::Checkbox("Song detector", &c.song);
        ImGui::Checkbox("Feedback timing (not the wave files)", &c.feedback);
    };
    if (ImGui::Button("Apply to box...")) ImGui::OpenPopup("applyto");
    ImGui::SameLine();
    if (ImGui::Button("Load from box...")) ImGui::OpenPopup("loadfrom");
    if (ImGui::BeginPopup("applyto")) {
        ImGui::TextUnformatted("Copy from the tuning set:");
        partsUi(applyParts_);
        ImGui::Separator();
        if (applyTo_.size() != boxes_.size()) applyTo_.assign(boxes_.size(), mode_ != AppMode::Multi);
        if (mode_ == AppMode::Multi) {
            ImGui::TextUnformatted("To boxes:");
            for (size_t i = 0; i < boxes_.size(); ++i) {
                ImGui::PushID(int(i));
                bool v = applyTo_[i];
                if (ImGui::Checkbox(boxes_[i].boxName.c_str(), &v)) applyTo_[i] = v;
                ImGui::PopID();
            }
        }
        if (ImGui::Button("Apply")) {
            int n = 0;
            for (size_t i = 0; i < boxes_.size(); ++i) {
                if (!applyTo_[i] && mode_ == AppMode::Multi) continue;
                copyParts(tuneP_, boxes_[i], applyParts_);
                pushBox(int(i));
                ++n;
            }
            status_ = "Applied tuning to " + std::to_string(n) + " box(es)";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("loadfrom")) {
        ImGui::TextUnformatted("Replace in the tuning set:");
        partsUi(loadParts_);
        ImGui::Separator();
        for (int i = 0; i < int(boxes_.size()); ++i) {
            ImGui::PushID(i);
            const std::string label = "From " + boxes_[size_t(i)].boxName;
            if (ImGui::Button(label.c_str())) {
                copyParts(boxes_[size_t(i)], tuneP_, loadParts_);
                status_ = "Tuning set loaded from " + boxes_[size_t(i)].boxName;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();
}

void Gui::drawControls() {
    const ImGuiTreeNodeFlags open = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Syllables & Target", open)) {
        if (EnumCombo("Detection method", &P().tmpl.method, 2, 14)) changed() = true;
        HelpMarker("Syllable sequence: syllables are segmented with the start / end thresholds and recognized by "
                   "their IOI and SD.\nSpectral templates: the spectrum of the last few ms is compared with stored "
                   "templates every frame; no segmentation needed (like EvTAF).");
        if (P().tmpl.method == DetectMethod::Template) {
            drawTemplates();
        } else {
            drawTarget();
            drawCodes();
        }
    }
    if (ImGui::CollapsingHeader("Spectrum Selection")) drawSpectrum();
    if (ImGui::CollapsingHeader("Processing Params")) drawProcessing();
    if (ImGui::CollapsingHeader("Song Detection (DetectBirdSong)")) drawSong();
    if (ImGui::CollapsingHeader("Feedback", open)) drawFeedback();
    if (tuning()) return;  // the rest is hardware and files, which the tuning set doesn't have
    if (mode_ == AppMode::ClosedLoop && ImGui::CollapsingHeader("Closed-loop pulse feedback", open)) drawPulseFeedback();
    if (ImGui::CollapsingHeader("Hardware / Source")) drawHardware();
    if (ImGui::CollapsingHeader("Recording")) drawRecording();
    if (ImGui::CollapsingHeader("Display")) drawDisplay();
}

void Gui::drawHardware() {
    const bool running = engine_.running();
    if (running) ImGui::TextColored(kAmber, "Stop acquisition to change hardware settings.");
    if (!daqAvailable()) ImGui::TextColored(kRed, "Built without NI-DAQmx: simulated source only.");
    static const std::string devices = describeDaqDevices();  // queried once
    ImGui::TextWrapped("Devices: %s", devices.c_str());
    ImGui::BeginDisabled(running);
    if (mode_ == AppMode::Multi) {
        EnumCombo("Source", &P().source.kind, 2, 14.0f);
        HelpMarker("Simulated boxes each play their own song; NI-DAQmx boxes are grouped by the card of their AI "
                   "channel. Boxes on one card share its AI task (same sample rate and frame size), one DO task (their "
                   "TTL lines must be on that card) and one streaming AO task (each box its own AO channel on that "
                   "card). The syllable code port can be on any card.");
    }

    ImGui::SeparatorText("Analog input (microphone)");
    InputStr("AI channel", &P().hw.aiChannel, 10);
    EnumCombo("Terminal config", &P().hw.terminalConfig, 5, 10);
    InputD("Min (V)", &P().hw.aiMin, "%.3g", 4);
    ImGui::SameLine();
    InputD("Max (V)", &P().hw.aiMax, "%.3g", 4);
    InputD("Sample rate (Hz)", &P().hw.sampleRate, "%.0f");
    InputI("Samples per frame", &P().hw.samplesPerFrame);
    P().hw.samplesPerFrame = std::max(1, P().hw.samplesPerFrame);
    ImGui::SameLine();
    ImGui::TextDisabled("= %.2f ms", P().hw.samplesPerFrame * 1000.0 / std::max(1.0, P().hw.sampleRate));
    InputD("Input buffer (s)", &P().hw.inputBufferSec, "%.2g");

    ImGui::SeparatorText("Analog output (feedback waveform)");
    InputStr("AO channel", &P().hw.aoChannel, 10);
    HelpMarker("Plays Feedback 1/2 wave files. Leave empty to disable. The AO rate is the wave file's sample rate.");
    InputD("AO min (V)", &P().hw.aoMin, "%.3g", 4);
    ImGui::SameLine();
    InputD("AO max (V)", &P().hw.aoMax, "%.3g", 4);
    InputD("Volts at full scale", &P().hw.aoScale, "%.4g");
    HelpMarker("A wave sample of +1.0 is output as this many volts (clipped to the AO range).");
    if (mode_ == AppMode::Multi) {
        InputD("AO stream buffer (ms)", &P().hw.aoStreamMs, "%.3g", 4);
        P().hw.aoStreamMs = std::clamp(P().hw.aoStreamMs, 2.0, 100.0);
        HelpMarker("Multi-box plays each card's speakers from one continuous output stream so the boxes are "
                   "independent. This much audio is queued ahead, so it is the feedback output latency; if the PC "
                   "stalls longer than this, the stream underflows (logged, and it restarts). The first box on a card "
                   "sets it. Wave files are resampled to the card's rate.");
    }

    ImGui::SeparatorText("Digital outputs (TTL)");
    InputStr("Feedback line", &P().hw.doFeedbackLine, 12);
    HelpMarker("Pulses for 'TTL pulse' ms each time feedback is delivered. One line per field, e.g. Dev1/port0/line0. Empty = off.");
    InputStr("Syllable line", &P().hw.doSyllableLine, 12);
    HelpMarker("High while a syllable is detected (start/end thresholds). Useful as a sync signal for the recording system.");
    InputStr("Match line", &P().hw.doMatchLine, 12);
    HelpMarker("Pulses when the target sequence matches, whether or not feedback follows (catch trials, refractory, no song).");
    InputStr("Song line", &P().hw.doSongLine, 12);
    HelpMarker("High while the song detector says the bird is singing. Empty = off.");
    InputStr("Syllable code port", &P().hw.sylCodeOutput, 12);
    HelpMarker("Where the matched syllable's U8 code is written. Either the name of a task saved in NI MAX "
               "(e.g. P2_SyllOut, like the LabVIEW VI) or a port / lines, e.g. Dev2/port1 or Dev2/port1/line0:7. "
               "Empty = off.");

    ImGui::SeparatorText("Simulated source");
    PathInput("Song WAV", &P().source.simWavPath);
    HelpMarker("Played back in real time, looped. Leave empty for a built-in synthetic song that matches the default setpoints.");
    InputD("Synthetic noise (V)", &P().source.simNoiseVolts, "%.4g");

    ImGui::EndDisabled();
}

void Gui::drawSpectrum() {
    InputD("# previous ms included in calcs", &P().spectrum.windowMs, "%.3g");
    P().spectrum.windowMs = std::clamp(P().spectrum.windowMs, 1.0, 200.0);
    const double fs = stats_.running ? stats_.sampleRate : P().hw.sampleRate;
    const double n = std::round(P().spectrum.windowMs * fs / 1000.0);
    ImGui::TextDisabled("%.0f samples, %.1f Hz bins", n, fs / std::max(1.0, n));

    auto bandTable = [&](const char* id, std::array<Band, kBandsPerGroup>& bands, const ImVec4& color) {
        if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) return;
        ImGui::TableSetupColumn("Gain");
        ImGui::TableSetupColumn("Low Hz");
        ImGui::TableSetupColumn("High Hz");
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        for (int i = 0; i < kBandsPerGroup; ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            InputD("##g", &bands[i].gain, "%.4g", 5);
            ImGui::TableNextColumn();
            InputD("##lo", &bands[i].lowHz, "%.0f", 5);
            ImGui::TableNextColumn();
            InputD("##hi", &bands[i].highHz, "%.0f", 5);
            ImGui::PopID();
        }
        ImGui::EndTable();
    };
    ImGui::TextColored(kGreen, "Include bands");
    HelpMarker("Band RMS amplitudes are weighted by Gain and summed. Gain 0 disables a row.");
    bandTable("include", P().spectrum.include, kGreen);
    ImGui::TextColored(kRed, "Subtract bands");
    HelpMarker("Typically low-frequency / cage noise. Combined with the include sum using Band Combination F(x).");
    bandTable("subtract", P().spectrum.subtract, kRed);

    EnumCombo("Band Combination F(x)", &P().spectrum.combination, 4, 8);
    InputD("Syl Start Thresh", &P().spectrum.sylStartThresh, "%.6g");
    InputD("End Thresh", &P().spectrum.sylEndThresh, "%.6g");
    if (P().spectrum.sylEndThresh > P().spectrum.sylStartThresh)
        ImGui::TextColored(kAmber, "End threshold is above start threshold.");
    InputD("Min gap (ms)", &P().spectrum.minGapMs, "%.3g");
    P().spectrum.minGapMs = std::clamp(P().spectrum.minGapMs, 0.0, 500.0);
    HelpMarker("Gaps shorter than this are bridged: a syllable, a tiny gap and another syllable count as one "
               "syllable (one onset; IOI and SD measured across the gap). 0 = off (LabVIEW behaviour).\n"
               "Onsets are not delayed. A syllable end is confirmed once the gap has lasted this long, so offset "
               "events and the falling edge of the syllable TTL line come up to this much later; the offset time "
               "and SD still use the moment the signal dropped.");
}

void Gui::drawProcessing() {
    Check("Mean Subtract?", &P().proc.meanSubtract);
    Check("Software Gain?", &P().proc.softwareGain);
    ImGui::SameLine();
    ImGui::BeginDisabled(!P().proc.softwareGain);
    InputD("Gain", &P().proc.gain, "%.4g", 4);
    ImGui::EndDisabled();
    Check("Low Cutoff?", &P().proc.lowCutoff);
    ImGui::SameLine();
    ImGui::BeginDisabled(!P().proc.lowCutoff);
    InputD("Low Cutoff Threshold", &P().proc.lowCutoffThresh, "%.3g", 5);
    ImGui::EndDisabled();
    HelpMarker("Detection signal values below this are set to 0.");
    Check("display dB?", &P().proc.displayDb);
    HelpMarker("Display only. Thresholds always apply to the linear detection signal.");
}

void Gui::drawTarget() {
    auto& c = P().codes;
    const SyllableCode* t = targetSyllable(c);
    const std::string current = t ? t->name + "  (code " + std::to_string(t->code) + ")" : "none (no feedback)";
    ImGui::SetNextItemWidth(14 * em());
    if (ImGui::BeginCombo("Feedback target", current.c_str())) {
        if (ImGui::Selectable("none (no feedback)", c.target < 0)) {
            c.target = -1;
            changed() = true;
        }
        for (int i = 0; i < int(c.list.size()); ++i) {
            ImGui::PushID(i);
            const std::string label = c.list[i].name + "  (code " + std::to_string(c.list[i].code) + ")";
            if (ImGui::Selectable(label.c_str(), i == c.target)) {
                c.target = i;
                changed() = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    HelpMarker("Feedback is triggered when an IOI end is classified as this syllable (it must be the first match "
               "in the list below).");
    if (t && !t->enabled) ImGui::TextColored(kAmber, "The target syllable is disabled, so no feedback will fire.");
}

void Gui::drawFeedback() {
    Check("Feedback enabled", &P().fb.enabled);
    HelpMarker("When off, detection and the match TTL keep running but no feedback trials start.");

    if (ImGui::BeginTable("fb", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Feedback 1");
        ImGui::TableSetupColumn("Feedback 2");
        ImGui::TableHeadersRow();
        auto row = [&](const char* name, double* a, double* b, const char* fmt) {
            ImGui::PushID(name);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            InputD("##1", a, fmt, 6);
            ImGui::TableNextColumn();
            InputD("##2", b, fmt, 6);
            ImGui::PopID();
        };
        row("Probability", &P().fb.prob1, &P().fb.prob2, "%.3g");
        row("Delay (ms)", &P().fb.delay1Ms, &P().fb.delay2Ms, "%.4g");
        row("Detect Signal Thresh", &P().fb.thresh1, &P().fb.thresh2, "%.4g");
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Delay 2 = -1 disables feedback 2.");
    if (!tuning()) {  // stimulus files belong to a box, not to the tuning set
        PathInput("Wave 1", &P().fb.wave1, 15);
        PathInput("Wave 2", &P().fb.wave2, 15);
        if (engine_.running()) ImGui::TextDisabled("Wave files are loaded at Start.");
    }
    EnumCombo("Feedback 2 mode", &P().fb.mode2, 2, 12);
    HelpMarker(
        "Alternative: each detection gives feedback 1 with Probability 1, else feedback 2 with Probability 2, "
        "else a catch trial.\nFollows feedback 1: after feedback 1 is sent, feedback 2 follows Delay 2 ms later "
        "with Probability 2.");
    InputD("Refractory Period (ms)", &P().fb.refractoryMs, "%.4g");
    InputD("Feedback Delay (ms)", &P().fb.extraDelayMs, "%.4g");
    HelpMarker("Added to Delay 1 (and to Delay 2 in Alternative mode).");
    InputD("TTL pulse (ms)", &P().fb.ttlPulseMs, "%.4g");
}

// A digital pulse on an input line (raised by the closed loop when its spike count crosses threshold) plays a
// bandpassed white-noise burst on the AO feedback channel: LabVIEW's "read the pulse, make white noise" loop.
void Gui::drawPulseFeedback() {
    auto& q = P().pulse;
    const bool running = engine_.running();
    ImGui::TextWrapped("A rising edge on the input line plays a noise burst on the AO channel (Hardware / Source). "
                       "Wire the closed loop's digital output line to this input.");
    if (running) ImGui::TextColored(kAmber, "Stop acquisition to change these settings.");
    ImGui::BeginDisabled(running);  // applied at Start: the DI task and the AO waveform are built then
    Check("Enabled", &q.enabled);
    InputStr("Pulse input line", &q.diLine, 12);
    HelpMarker("Digital input that receives the pulse, e.g. Dev1/port0/line4. Sampled once per 1 ms frame, so the "
               "pulse must be at least 2 ms wide. Must not be one of the output lines above.");
    EnumCombo("Stimulus", &q.source, 2, 10);
    if (q.source == PulseSource::WavFile) {
        PathInput("Wave file", &q.wavPath, 15);
        HelpMarker("Played instead of noise. Its sample rate sets the AO rate and must match the Wave 1/2 files if those are used.");
    }
    InputD("Duration (ms)", &q.durationMs, "%.4g", 6);
    q.durationMs = std::clamp(q.durationMs, 1.0, 5000.0);
    HelpMarker("Length of the noise burst (Feedback Duration in LabVIEW).");
    InputD("Delay (ms)", &q.delayMs, "%.4g", 6);
    q.delayMs = std::clamp(q.delayMs, 0.0, 5000.0);
    HelpMarker("Silence between the pulse and the start of the burst (Feedback Delay in LabVIEW). Timed on the AO clock.");
    InputD("Amplitude gain", &q.gain, "%.4g", 6);
    q.gain = std::max(0.0, q.gain);
    if (q.source == PulseSource::WhiteNoise) {
        const double rate = running ? stats_.sampleRate : q.aoRateHz;
        ImGui::SameLine();
        ImGui::TextDisabled("~%.2f V rms", expectedNoiseRms(q, q.aoRateHz > 0 ? q.aoRateHz : rate));
        HelpMarker("Standard deviation (V) of the unfiltered white noise; the bandpass lowers it (the figure shown is the "
                   "expected rms after filtering). Output is clipped to the AO range. For a wave file: volts at full scale.");
        InputD("Band low (Hz)", &q.lowHz, "%.5g", 6);
        InputD("Band high (Hz)", &q.highHz, "%.5g", 6);
        InputI("Filter order", &q.filterOrder, 6);
        q.filterOrder = std::clamp(q.filterOrder, 1, 20);
        HelpMarker("Butterworth bandpass, LabVIEW's order convention (order N = 2N poles).");
        InputD("AO rate (Hz)", &q.aoRateHz, "%.0f", 6);
        q.aoRateHz = std::max(1000.0, q.aoRateHz);
        HelpMarker("AO sample clock for the burst (LabVIEW: 40000). If Wave 1/2 files are loaded, their rate is used instead.");
        if (!(q.lowHz > 0 && q.highHz > q.lowHz && q.highHz < q.aoRateHz / 2))
            ImGui::TextColored(kRed, "Band must satisfy 0 < low < high < rate/2.");
    }
    InputD("Refractory (ms)", &q.refractoryMs, "%.4g", 6);
    q.refractoryMs = std::max(0.0, q.refractoryMs);
    HelpMarker("Pulses within this time of one that played are counted but ignored.");
    if (q.enabled && P().hw.aoChannel.empty()) ImGui::TextColored(kRed, "Set the AO channel under Hardware / Source.");
    if (q.enabled && q.diLine.empty()) ImGui::TextColored(kRed, "Set the pulse input line.");
    ImGui::EndDisabled();
}

void Gui::drawTemplates() {
    auto& t = P().tmpl;
    const int bands = templateBands(t);

    // Feedback target
    const std::string current = t.target >= 0 && t.target < int(t.list.size())
                                    ? t.list[size_t(t.target)].name + "  (code " + std::to_string(t.list[size_t(t.target)].code) + ")"
                                    : std::string("none (no feedback)");
    ImGui::SetNextItemWidth(14 * em());
    if (ImGui::BeginCombo("Feedback target##tmpl", current.c_str())) {
        if (ImGui::Selectable("none (no feedback)", t.target < 0)) t.target = -1, changed() = true;
        for (int i = 0; i < int(t.list.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(t.list[size_t(i)].name.c_str(), i == t.target)) t.target = i, changed() = true;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    HelpMarker("Feedback is triggered when this template is detected (and passes its timing gate, if it has one).");

    ImGui::SeparatorText("Spectrum compared");
    InputD("window (ms)##tmpl", &t.windowMs, "%.3g", 4);
    t.windowMs = std::clamp(t.windowMs, 2.0, 50.0);
    ImGui::SameLine();
    InputD("band (Hz)##tmpl", &t.bandHz, "%.0f", 4);
    t.bandHz = std::clamp(t.bandHz, 20.0, 2000.0);
    InputD("from (Hz)##tmpl", &t.lowHz, "%.0f", 4);
    ImGui::SameLine();
    InputD("to (Hz)##tmpl", &t.highHz, "%.0f", 4);
    t.lowHz = std::clamp(t.lowHz, 0.0, 50000.0);
    t.highHz = std::clamp(t.highHz, t.lowHz + 100.0, 50000.0);
    ImGui::TextDisabled("%d bands", bands);
    HelpMarker("Every frame, the spectrum of the last 'window' ms is cut into bands of 'band' Hz between 'from' and "
               "'to', in log power, and correlated with each template (1 = identical shape, regardless of "
               "loudness). Changing these rebuilds the templates from their examples (in the Tune tab).");
    InputD("min level (dB)##tmpl", &t.minLevelDb, "%.0f", 4);
    HelpMarker("Nothing is detected while the RMS between 'from' and 'to' is below this (dB re 1 V at the input). "
               "Keeps background noise from matching. Hover the detection plot in the Tune tab to read levels.");
    ImGui::SameLine();
    InputD("re-arm (ms)##tmpl", &t.rearmMs, "%.0f", 4);
    t.rearmMs = std::clamp(t.rearmMs, 0.0, 2000.0);
    HelpMarker("A template is detected once per stretch above its threshold, and not again within this time.");

    ImGui::SeparatorText("Templates");
    HelpMarker("Make one in the Tune tab: Ctrl+click on the spectrogram or detection plot just after the part of the "
               "syllable you want (the template is the window before the cursor). Ctrl+Shift+click adds more "
               "examples, which are averaged: a few examples from different renditions make it more robust.\n\n"
               "A template is detected when its correlation stays at or above the threshold for 'frames' frames in "
               "a row. A timing gate makes it count only if another template was detected the given time earlier, "
               "to tell apart syllables that look alike.");
    int remove = -1;
    for (int i = 0; i < int(t.list.size()); ++i) {
        auto& e = t.list[size_t(i)];
        ImGui::PushID(i);
        const std::string header = std::string(i == t.target ? "* " : "  ") + e.name + "  (code " +
                                   std::to_string(e.code) + ")" + (e.enabled ? "" : "  [off]") + "###thdr";
        const bool open = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed |
                                                                (i == editTmpl_ ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemClicked()) editTmpl_ = i;
        if (!open) {
            ImGui::PopID();
            continue;
        }
        Check("##en", &e.enabled);
        ImGui::SameLine();
        InputStr("##name", &e.name, 6);
        ImGui::SameLine();
        if (InputI("code", &e.code, 3)) e.code = std::clamp(e.code, 1, 255);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = i;
        InputD("threshold", &e.thresh, "%.3f", 4);
        e.thresh = std::clamp(e.thresh, 0.0, 1.0);
        ImGui::SameLine();
        if (InputI("frames", &e.minFrames, 3)) e.minFrames = std::clamp(e.minFrames, 1, 100);
        // Timing gate
        const std::string gate = e.gateIndex >= 0 && e.gateIndex < int(t.list.size()) && e.gateIndex != i
                                     ? "after " + t.list[size_t(e.gateIndex)].name
                                     : std::string("no timing gate");
        ImGui::SetNextItemWidth(9 * em());
        if (ImGui::BeginCombo("##gate", gate.c_str())) {
            if (ImGui::Selectable("no timing gate", e.gateIndex < 0)) e.gateIndex = -1, changed() = true;
            for (int j = 0; j < int(t.list.size()); ++j) {
                if (j == i) continue;
                ImGui::PushID(j);
                if (ImGui::Selectable(("after " + t.list[size_t(j)].name).c_str(), j == e.gateIndex))
                    e.gateIndex = j, changed() = true;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (e.gateIndex >= 0) {
            ImGui::SameLine();
            InputD("to##gmin", &e.gateMinMs, "%.0f", 3.5f);
            ImGui::SameLine();
            InputD("ms earlier##gmax", &e.gateMaxMs, "%.0f", 3.5f);
            e.gateMaxMs = std::max(e.gateMaxMs, e.gateMinMs);
        }
        ImGui::TextDisabled("%zu example(s)", e.examples.size());
        if (int(e.shape.size()) != bands) {
            ImGui::SameLine();
            ImGui::TextColored(kAmber, e.shape.empty() ? "empty: Ctrl+click in the Tune tab" : "spectrum settings changed");
            if (!e.examples.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Rebuild")) {
                    std::string err;
                    if (rebuildTemplateShape(e, P(), &err)) changed() = true;
                    else status_ = err;
                }
            }
        }
        ImGui::TreePop();
        ImGui::PopID();
    }
    if (remove >= 0) {
        t.list.erase(t.list.begin() + remove);
        // Indices after the removed one shift down; references to it are dropped.
        auto fix = [&](int& idx) { idx = idx == remove ? -1 : idx > remove ? idx - 1 : idx; };
        fix(t.target);
        for (auto& e : t.list) fix(e.gateIndex);
        if (editTmpl_ >= int(t.list.size())) editTmpl_ = int(t.list.size()) - 1;
        changed() = true;
    }
    if (t.list.size() < size_t(kMaxTemplates) && ImGui::Button("+ New template")) {
        editTmpl_ = -1;  // the next Ctrl+click in the Tune tab creates it
        status_ = "Ctrl+click a sound in the Tune tab to create the template.";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The next Ctrl+click in the Tune tab creates a new template from that sound.");
}

void Gui::drawCodes() {
    auto& c = P().codes;
    ImGui::SeparatorText("Syllables");
    HelpMarker(
        "Each syllable has a trigger type and a window for IOI, SD and previous IOI, as setpoint +/- eps or "
        "min / max. With onsets A, B, C: at C's onset IOI = C-B, SD = duration of B, previous IOI = B-A.\n\n"
        "At every IOI end the list is checked top to bottom and the FIRST syllable that matches is reported: its "
        "code is written to the syllable code port and counted, and if it is the target, feedback is triggered.\n\n"
        "Click a syllable's header to edit it here and in the Tune tab's folder summary. In the Tune tab, "
        "Ctrl+click a syllable to set the edited syllable's windows from it, Ctrl+Shift+click to add a new one.");
    if (!tuning()) {  // port output belongs to a box
        Check("Write codes to port##codes", &c.enabled);
        ImGui::SameLine();
        EnumCombo("Hold", &c.hold, 3, 9);
        if (c.hold == SylCodeHold::Pulse) {
            ImGui::SameLine();
            InputD("ms##codepulse", &c.pulseMs, "%.3g", 3);
        }
    }
    const EngineStats st = tuning() ? EngineStats{} : stats_;  // live counts belong to the box too
    if (st.running) {
        ImGui::Text("Port now: %d", st.currentCode);
        ImGui::SameLine();
        ImGui::TextDisabled("  unmatched IOI ends: %llu", (unsigned long long)st.codeUnmatched);
    }

    int remove = -1, moveUp = -1;
    const bool editChanged = editSyl_ != lastOpenedSyl_;
    lastOpenedSyl_ = editSyl_;
    for (int i = 0; i < int(c.list.size()); ++i) {
        auto& e = c.list[i];
        ImGui::PushID(i);
        // Header: star for the target, highlighted when it is the syllable being edited.
        std::string header = std::string(i == c.target ? "* " : "  ") + e.name + "  (code " +
                             std::to_string(e.code) + ")" + (e.enabled ? "" : "  [off]");
        if (st.running && i < int(st.codeCounts.size()))
            header += "  n=" + std::to_string(st.codeCounts[i]);
        header += "###hdr";
        if (editChanged) ImGui::SetNextItemOpen(i == editSyl_);
        const bool open = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth |
                                                                ImGuiTreeNodeFlags_Framed |
                                                                (i == editSyl_ ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemClicked()) editSyl_ = lastOpenedSyl_ = i;
        if (!open) {
            ImGui::PopID();
            continue;
        }
        Check("##en", &e.enabled);
        ImGui::SameLine();
        InputStr("##name", &e.name, 6);
        ImGui::SameLine();
        if (InputI("code", &e.code, 3)) e.code = std::clamp(e.code, 1, 255);
        ImGui::SameLine();
        if (ImGui::ArrowButton("up", ImGuiDir_Up)) moveUp = i;
        ImGui::SameLine();
        if (ImGui::ArrowButton("down", ImGuiDir_Down) && i + 1 < int(c.list.size())) moveUp = i + 1;
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = i;
        // Trigger type: only the types evaluated at an onset (= an IOI end).
        ImGui::SetNextItemWidth(8 * em());
        if (ImGui::BeginCombo("##type", toString(e.type))) {
            for (TriggerType t : {TriggerType::IOI, TriggerType::SD_IOI, TriggerType::IOI_SD_IOI})
                if (ImGui::Selectable(toString(t), t == e.type)) {
                    e.type = t;
                    changed() = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const RangeMode before = e.mode;
        if (EnumCombo("##mode", &e.mode, 2, 11) && e.mode != before)
            for (TimeWindow* w : {&e.ioi, &e.sd, &e.prevIoi}) w->convertTo(e.mode);

        const bool minMax = e.mode == RangeMode::MinMax;
        if (ImGui::BeginTable("win", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("");
            ImGui::TableSetupColumn(minMax ? "min (ms)" : "setpoint (ms)");
            ImGui::TableSetupColumn(minMax ? "max (ms)" : "+/- eps (ms)");
            ImGui::TableHeadersRow();
            auto row = [&](const char* label, TimeWindow& w, bool used) {
                ImGui::PushID(label);
                ImGui::TableNextRow();
                ImGui::BeginDisabled(!used);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(label);
                ImGui::TableNextColumn();
                InputD("##a", minMax ? &w.minMs : &w.setpointMs, "%.4g", 4.5f);
                ImGui::TableNextColumn();
                InputD("##b", minMax ? &w.maxMs : &w.epsMs, "%.4g", 4.5f);
                ImGui::EndDisabled();
                ImGui::PopID();
            };
            row("IOI", e.ioi, usesIoi(e.type));
            row("SD (prev syllable)", e.sd, usesSd(e.type));
            row("Prev IOI", e.prevIoi, usesPrevIoi(e.type));
            ImGui::EndTable();
        }
        ImGui::TreePop();
        ImGui::PopID();
    }
    // Keep the target and the edited syllable pointing at the same entries.
    auto remap = [](int& idx, auto f) { idx = f(idx); };
    if (remove >= 0) {
        c.list.erase(c.list.begin() + remove);
        auto afterRemove = [&](int idx) { return idx == remove ? -1 : idx > remove ? idx - 1 : idx; };
        remap(c.target, afterRemove);
        remap(editSyl_, afterRemove);
        if (editSyl_ < 0) editSyl_ = std::min(remove, int(c.list.size()) - 1);
        changed() = true;
    }
    if (moveUp > 0) {
        std::swap(c.list[moveUp], c.list[moveUp - 1]);
        auto afterSwap = [&](int idx) { return idx == moveUp ? moveUp - 1 : idx == moveUp - 1 ? moveUp : idx; };
        remap(c.target, afterSwap);
        remap(editSyl_, afterSwap);
        lastOpenedSyl_ = editSyl_;
        changed() = true;
    }
    ImGui::Separator();
    ImGui::BeginDisabled(int(c.list.size()) >= kMaxCodes);
    if (ImGui::Button("+ Add syllable")) {
        // Start from the syllable being edited (or the LabVIEW default target).
        SyllableCode e = editSyl_ >= 0 && editSyl_ < int(c.list.size()) ? c.list[editSyl_] : defaultTargetSyllable();
        int code = 1;
        for (const auto& x : c.list) code = std::max(code, x.code + 1);
        e.code = std::min(code, 255);
        e.name = "syl" + std::to_string(c.list.size() + 1);
        e.enabled = true;
        c.list.push_back(e);
        editSyl_ = int(c.list.size()) - 1;
        changed() = true;
    }
    ImGui::EndDisabled();
}

void Gui::drawSong() {
    auto& s = P().song;
    Check("Song detector enabled", &s.enabled);
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.enabled);
    Check("Gate feedback on song", &s.gateFeedback);
    HelpMarker("When on, sequence matches only start feedback trials while song is detected. Blocked matches are "
               "logged and shown as grey dashed lines.");

    ImGui::SeparatorText("Song Detect Params");
    InputD("Ratio Threshold", &s.ratioThresh, "%.4g");
    EnumCombo("Ratio", &s.ratioMode, 2, 12);
    HelpMarker("Per spectrogram column: mean power per bin in the bird band divided by the mean power per bin in "
               "the non-bird band (or by bird + non-bird).");
    InputD("Song Duration (s)", &s.songDurationSec, "%.3g");
    InputD("Duration Threshold (0-1)", &s.durationThresh, "%.3g");
    HelpMarker("Song is detected when more than this fraction of the last 'Song Duration' is above the ratio "
               "threshold (LabVIEW: box-kernel convolution > threshold).");
    InputD("Non-bird Freq Cutoff (Hz)", &s.nonBirdCutoffHz, "%.0f");
    InputD("Bird Low Frequency (Hz)", &s.birdLowHz, "%.0f");
    InputD("Bird High Frequency (Hz)", &s.birdHighHz, "%.0f");
    InputD("Min bird-band RMS (V)", &s.minBirdRms, "%.4g");
    HelpMarker("Optional: also require this much signal in the bird band, so quiet broadband hiss can't count as "
               "song. 0 = off (LabVIEW behaviour).");
    InputD("Hold (s)", &s.holdSec, "%.3g");
    HelpMarker("Keep 'song' on this long after the detector drops. 0 = LabVIEW behaviour.");

    ImGui::SeparatorText("Spectrogram");
    InputI("Window length (samples)", &s.windowLength);
    InputI("Frequency bins", &s.frequencyBins);
    InputD("Column hop (ms)", &s.hopMs, "%.3g");
    s.windowLength = std::clamp(s.windowLength, 16, 65536);
    s.hopMs = std::max(0.1, s.hopMs);
    const auto& st = stats_;
    if (st.running && st.songFftSize > 0)
        ImGui::TextDisabled("Hann, FFT %d, %.1f Hz bins, column every %.1f ms", st.songFftSize, st.songBinHz, s.hopMs);

    ImGui::SeparatorText("Pre-Processing Params");
    Check("Mean Subtract?##song", &s.meanSubtract);
    InputD("Gain##song", &s.gain, "%.4g", 5);
    ImGui::SameLine();
    InputD("offset##song", &s.offset, "%.4g", 5);
    Check("Butterworth Filter? (Bandpass)", &s.butterworth);
    ImGui::BeginDisabled(!s.butterworth);
    InputI("order", &s.filterOrder, 4);
    s.filterOrder = std::clamp(s.filterOrder, 1, 20);
    InputD("low cutoff (Hz)", &s.filterLowHz, "%.0f");
    InputD("high cutoff (Hz)", &s.filterHighHz, "%.0f");
    ImGui::EndDisabled();
    if (s.butterworth && s.filterLowHz > s.nonBirdCutoffHz)
        ImGui::TextColored(kAmber, "The bandpass removes the non-bird band, so the ratio will be very large.");
    ImGui::EndDisabled();
}

void Gui::drawRecording() {
    auto& r = P().rec;
    const bool running = engine_.running();
    ImGui::BeginDisabled(running);  // all of this is applied at Start
    InputStr("##outdir", &r.outputDir, 12);
    ImGui::SameLine();
    if (ImGui::Button("...##outdir")) {
        const std::string f = pickFolderDialog("Recording output folder");
        if (!f.empty()) {
            r.outputDir = f;
            changed() = true;
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Output folder");
    Check("Events CSV", &r.saveEvents);
    Check("Continuous detection signal WAV (frame rate)", &r.saveEnvelope);

    ImGui::SeparatorText("Song-triggered WAVs");
    Check("Record a WAV for each song", &r.songFiles);
    HelpMarker("When the song detector turns on, a new WAV starts with the last 'Pre-roll' seconds of audio "
               "(so the start of the song is included) and keeps recording until song has been off for "
               "'Post-roll' seconds. Song coming back within the post-roll extends the same file.\n"
               "Files: <session>_song001.wav, ... plus <session>_songs.csv listing each file's start/end sample "
               "(same clock as the events CSV).");
    ImGui::BeginDisabled(!r.songFiles);
    InputD("Pre-roll (s)", &r.preRollSec, "%.3g", 5);
    r.preRollSec = std::clamp(r.preRollSec, 0.0, 60.0);
    InputD("Post-roll (s)", &r.postRollSec, "%.3g", 5);
    r.postRollSec = std::clamp(r.postRollSec, 0.0, 600.0);
    ImGui::TextUnformatted("Channels:");
    ImGui::SameLine();
    Check("raw mic", &r.chMic);
    ImGui::SameLine();
    Check("detection signal", &r.chDetection);
    ImGui::SameLine();
    Check("marker track", &r.chMarkers);
    HelpMarker("Each enabled channel is one channel of the WAV, in this order, at the microphone sample rate.\n"
               "raw mic: volts.\n"
               "detection signal: the linear detection value, held for each 1 ms frame.\n"
               "marker track: the TTL line states added up: 0.1 song + 0.2 syllable + 0.3 target match + "
               "0.4 feedback (match and feedback last 'TTL pulse' ms). Every combination gives a distinct level.");
    if (r.songFiles && !r.chMic && !r.chDetection && !r.chMarkers)
        ImGui::TextColored(kAmber, "No channels selected: no song files will be written.");
    if (r.songFiles && !P().song.enabled)
        ImGui::TextColored(kAmber, "The song detector is off, so no song files will be written.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::TextDisabled("Applied at the next Start. A copy of the settings is saved with each session.");
    const auto& s = stats_;
    if (!s.sessionPrefix.empty()) ImGui::TextWrapped("Session: %s", s.sessionPrefix.c_str());
    if (s.running && r.songFiles)
        ImGui::TextColored(s.songFileOpen ? kGreen : ImVec4(0.7f, 0.7f, 0.7f, 1), "%s  |  %d song file(s) written",
                           s.songFileOpen ? "RECORDING SONG" : "waiting for song", s.songFilesDone);
}

void Gui::drawDisplay() {
    float w = float(P().display.windowSec);
    ImGui::SetNextItemWidth(12 * em());
    if (ImGui::SliderFloat("Plot window (s)", &w, 0.2f, 10.0f, "%.1f")) P().display.windowSec = w;
    ImGui::Checkbox("Raw autoscale Y", &P().display.rawAutoY);
    if (!P().display.rawAutoY) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(5 * em());
        ImGui::InputDouble("+/- V", &P().display.rawYRange, 0, 0, "%.3g");
    }
    ImGui::Checkbox("Detection autoscale Y", &P().display.envAutoY);
}

void Gui::drawStatus() {
    const auto& s = stats_;
    if (!s.running) {
        ImGui::TextDisabled("Not running. Pick a source and press Start.");
        return;
    }
    ImGui::Text("%s  %.0f Hz  frame %d (%.2f ms)  window %d  |  t = %.1f s", s.backend.c_str(), s.sampleRate,
                s.frameSamples, s.frameSamples * 1000.0 / s.sampleRate, s.windowSamples, s.elapsedSec);
    ImGui::SameLine();
    const bool lagging = s.backlogMs > 5.0 * s.frameSamples * 1000.0 / s.sampleRate;
    ImGui::TextColored(lagging ? kRed : ImVec4(0.7f, 0.7f, 0.7f, 1), "|  backlog %.1f ms (max %.1f)  proc %.0f us (max %.0f)",
                       s.backlogMs, s.maxBacklogMs, s.procUsAvg, s.procUsMax);

    ImGui::TextColored(stateColor(s.fbState), "%-18s", toString(s.fbState));
    ImGui::SameLine();
    ImGui::TextColored(s.inSyllable ? kGreen : ImVec4(0.5f, 0.5f, 0.5f, 1), s.inSyllable ? "SYLLABLE" : "silence ");
    ImGui::SameLine();
    ImGui::TextColored(s.songActive ? kBlue : ImVec4(0.5f, 0.5f, 0.5f, 1), s.songActive ? "SONG" : "no song");
    if (s.songFileOpen) {
        ImGui::SameLine();
        ImGui::TextColored(kRed, "REC");
    }
    ImGui::SameLine();
    ImGui::Text("| last IOI %.0f ms  SD %.0f ms  | onsets %llu  matches %llu (blocked %llu)  trials %llu  fb1 %llu  "
                "fb2 %llu  suppressed %llu  catch %llu  bouts %llu",
                s.lastIoiMs, s.lastSdMs, (unsigned long long)s.onsets, (unsigned long long)s.matches,
                (unsigned long long)s.blocked, (unsigned long long)s.trials, (unsigned long long)s.fires1,
                (unsigned long long)s.fires2, (unsigned long long)s.suppressed, (unsigned long long)s.catches,
                (unsigned long long)s.songBouts);
    if (P().pulse.enabled) {
        ImGui::TextColored(s.pulseInput ? kRed : ImVec4(0.7f, 0.7f, 0.7f, 1), "closed-loop pulses in %llu  noise bursts played %llu%s",
                           (unsigned long long)s.pulseTrigs, (unsigned long long)s.pulseFires, s.pulseInput ? "   (input HIGH)" : "");
    }
}

void Gui::drawPlots(float height) {
    if (const Box* b = engineBox(selBox_))
        b->snapshot(P().display.windowSec, snap_);
    else
        snap_ = ScopeSnapshot{};
    const double win = P().display.windowSec;
    const float gap = ImGui::GetStyle().ItemSpacing.y;
    const float plotH = (height - 2 * gap) * 0.36f;
    const float songH = (height - 2 * gap) * 0.28f;

    auto relTimes = [&](const std::vector<double>& t) {
        std::vector<double> r;
        for (double v : t)
            if (v >= snap_.tEndSec - win) r.push_back(v - snap_.tEndSec);
        return r;
    };
    const auto fireX = relTimes(snap_.fireT), matchX = relTimes(snap_.matchT), supX = relTimes(snap_.suppressT);
    const auto blockedX = relTimes(snap_.blockedT);
    auto markers = [&] {
        auto lines = [](const char* name, const std::vector<double>& x, const ImVec4& c, float w) {
            if (x.empty()) return;
            ImPlot::SetNextLineStyle(c, w);
            ImPlot::PlotInfLines(name, x.data(), int(x.size()));
        };
        lines("match", matchX, ImVec4(0.6f, 0.4f, 1.0f, 0.8f), 1.0f);
        lines("blocked (no song)", blockedX, ImVec4(0.5f, 0.5f, 0.5f, 0.9f), 1.0f);
        lines("feedback", fireX, kRed, 2.0f);
        lines("suppressed", supX, ImVec4(0.6f, 0.6f, 0.6f, 0.8f), 1.0f);
    };
    // Frame-rate time axis shared by the detection and song plots.
    auto frameTimes = [&](size_t m) {
        std::vector<double> x(m);
        for (size_t i = 0; i < m; ++i) x[i] = -double(m - 1 - i) / std::max(1.0, snap_.envRate);
        return x;
    };

    // ---- raw microphone voltage
    if (ImPlot::BeginPlot("Microphone", ImVec2(-1, plotH))) {
        ImPlot::SetupAxes(nullptr, "V", 0, P().display.rawAutoY ? ImPlotAxisFlags_AutoFit : 0);
        ImPlot::SetupAxisLimits(ImAxis_X1, -win, 0, ImPlotCond_Always);
        if (!P().display.rawAutoY)
            ImPlot::SetupAxisLimits(ImAxis_Y1, -P().display.rawYRange, P().display.rawYRange, ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);
        const int buckets = std::max(100, int(ImPlot::GetPlotSize().x));
        const size_t nr = snap_.raw.size();
        decimate(snap_.raw.data(), nr, nr > 0 ? -double(nr - 1) / snap_.rawRate : 0.0, snap_.rawRate, buckets, xs_,
                 ys_);
        if (!xs_.empty()) {
            ImPlot::SetNextLineStyle(kBlue, 1.0f);
            ImPlot::PlotLine("mic", xs_.data(), ys_.data(), int(xs_.size()));
        }
        markers();
        ImPlot::EndPlot();
    }

    // ---- detection signal with thresholds and syllable shading
    if (ImPlot::BeginPlot("Detection signal", ImVec2(-1, plotH))) {
        const bool dB = P().proc.displayDb;
        ImPlot::SetupAxes("time (s)", dB ? "dB" : "amplitude", 0, P().display.envAutoY ? ImPlotAxisFlags_AutoFit : 0);
        ImPlot::SetupAxis(ImAxis_Y2, nullptr,
                          ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks |
                              ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, 1.0, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_X1, -win, 0, ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);

        const size_t m = snap_.env.size();
        xs_.resize(m);
        ys_.resize(m);
        std::vector<double> syl(m);
        for (size_t i = 0; i < m; ++i) {
            xs_[i] = -double(m - 1 - i) / std::max(1.0, snap_.envRate);
            // In dB mode clamp the floor so silent periods don't squash the plot.
            ys_[i] = dB ? std::max<double>(snap_.env[i], -100.0) : snap_.env[i];
            syl[i] = snap_.syl[i];
        }
        // ImPlot 0.16 reads element 0 even for empty series, so only plot real data.
        if (m > 0) {
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::SetNextFillStyle(kGreen, 0.18f);
            ImPlot::PlotShaded("syllable", xs_.data(), syl.data(), int(m), 0.0);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::SetNextLineStyle(kAmber, 1.5f);
            ImPlot::PlotLine("signal", xs_.data(), ys_.data(), int(m));
        }
        // Template method: the target template's correlation (right axis, 0..1) and its threshold.
        const auto& tp = P().tmpl;
        if (tp.method == DetectMethod::Template && snap_.tmplScore.size() == m && m > 0) {
            std::vector<double> sc(m);
            for (size_t i = 0; i < m; ++i) sc[i] = snap_.tmplScore[i];
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::SetNextLineStyle(kPurple, 1.5f);
            ImPlot::PlotLine("template score", xs_.data(), sc.data(), int(m));
            if (tp.target >= 0 && tp.target < int(tp.list.size())) {
                const double th = tp.list[size_t(tp.target)].thresh;
                ImPlot::SetNextLineStyle(ImVec4(0.65f, 0.45f, 1.0f, 0.6f), 1.0f);
                ImPlot::PlotInfLines("template thresh", &th, 1, ImPlotInfLinesFlags_Horizontal);
            }
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        }
        const double th[2] = {Detector::toDisplay(P().spectrum.sylStartThresh, dB),
                              Detector::toDisplay(P().spectrum.sylEndThresh, dB)};
        ImPlot::SetNextLineStyle(kGreen, 1.0f);
        ImPlot::PlotInfLines("start", &th[0], 1, ImPlotInfLinesFlags_Horizontal);
        ImPlot::SetNextLineStyle(kRed, 1.0f);
        ImPlot::PlotInfLines("end", &th[1], 1, ImPlotInfLinesFlags_Horizontal);
        markers();
        ImPlot::EndPlot();
    }

    // ---- song detector: kernel fraction vs. duration threshold, ratio (log) vs. ratio threshold
    if (ImPlot::BeginPlot("Song detector", ImVec2(-1, songH))) {
        ImPlot::SetupAxes("time (s)", "fraction", 0, ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.05, ImPlotCond_Always);
        ImPlot::SetupAxis(ImAxis_Y2, "ratio", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisScale(ImAxis_Y2, ImPlotScale_Log10);
        ImPlot::SetupAxisLimits(ImAxis_X1, -win, 0, ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);

        const size_t m = std::min({snap_.songFrac.size(), snap_.songRatio.size(), snap_.songOn.size()});
        if (m > 0 && P().song.enabled) {
            const auto x = frameTimes(m);
            std::vector<double> frac(m), ratio(m), on(m);
            for (size_t i = 0; i < m; ++i) {
                frac[i] = snap_.songFrac[i];
                ratio[i] = std::max<double>(snap_.songRatio[i], 1e-6);  // log axis
                on[i] = snap_.songOn[i] * 1.05;
            }
            ImPlot::SetNextFillStyle(kBlue, 0.30f);
            ImPlot::PlotShaded("song", x.data(), on.data(), int(m), 0.0);
            ImPlot::SetNextLineStyle(kBlue, 1.5f);
            ImPlot::PlotLine("fraction", x.data(), frac.data(), int(m));
            const double dth = P().song.durationThresh;
            ImPlot::SetNextLineStyle(ImVec4(0.35f, 0.6f, 1.0f, 0.6f), 1.0f);
            ImPlot::PlotInfLines("duration thresh", &dth, 1, ImPlotInfLinesFlags_Horizontal);

            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::SetNextLineStyle(ImVec4(0.8f, 0.8f, 0.8f, 0.7f), 1.0f);
            ImPlot::PlotLine("ratio", x.data(), ratio.data(), int(m));
            const double rth = P().song.ratioThresh;
            ImPlot::SetNextLineStyle(ImVec4(0.8f, 0.8f, 0.8f, 0.5f), 1.0f);
            ImPlot::PlotInfLines("ratio thresh", &rth, 1, ImPlotInfLinesFlags_Horizontal);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        }
        markers();
        ImPlot::EndPlot();
    }
}

void Gui::drawEventLog() {
    const Box* box = engineBox(selBox_);
    const auto ev = box ? box->events(300) : std::vector<LogEvent>{};
    if (!ImGui::BeginTable("events", 3,
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                               ImGuiTableFlags_SizingStretchProp))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("time (s)", ImGuiTableColumnFlags_WidthFixed, 6 * em());
    ImGui::TableSetupColumn("event", ImGuiTableColumnFlags_WidthFixed, 7 * em());
    ImGui::TableSetupColumn("detail");
    ImGui::TableHeadersRow();
    for (auto it = ev.rbegin(); it != ev.rend(); ++it) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", it->tMs / 1000.0);
        ImGui::TableNextColumn();
        ImVec4 c(0.8f, 0.8f, 0.8f, 1);
        if (it->type.rfind("feedback", 0) == 0) c = kRed;
        else if (it->type.rfind("song", 0) == 0) c = kBlue;
        else if (it->type == "match") c = ImVec4(0.6f, 0.4f, 1.0f, 1);
        else if (it->type == "error") c = kRed;
        else if (it->type == "onset") c = kGreen;
        ImGui::TextColored(c, "%s", it->type.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(it->detail.c_str());
    }
    ImGui::EndTable();
}

}  // namespace fp

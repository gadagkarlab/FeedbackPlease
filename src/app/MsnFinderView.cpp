#include "app/MsnFinderView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "app/Widgets.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "implot.h"

namespace fp {

using namespace ui;

namespace {

constexpr int kSpecOut = 128;  // spectrogram is shown at this many rows / columns

ImU32 col(const ImVec4& c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, a)); }

// Shade [lo, hi] on x over the full height of the current plot.
void shadeX(double lo, double hi, ImU32 c) {
    const ImPlotRect r = ImPlot::GetPlotLimits();
    const ImVec2 a = ImPlot::PlotToPixels(lo, r.Y.Max), b = ImPlot::PlotToPixels(hi, r.Y.Min);
    ImPlot::PushPlotClipRect();
    ImPlot::GetPlotDrawList()->AddRectFilled(a, b, c);
    ImPlot::PopPlotClipRect();
}

double percentile(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double pos = q * (v.size() - 1);
    const size_t i = static_cast<size_t>(pos);
    const double f = pos - i;
    return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}

}  // namespace

MsnFinderView::~MsnFinderView() { shutdown(); }

// ---------------------------------------------------------------------------------------------
// Settings file (key=value)

void MsnFinderView::load(const std::string& path, std::string* status) {
    std::ifstream f(path);
    if (!f) return;
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        kv[line.substr(0, eq)] = v;
    }
    auto S = [&](const char* k, std::string& v) { if (kv.count(k)) v = kv[k]; };
    auto I = [&](const char* k, int& v) { if (kv.count(k)) v = std::atoi(kv[k].c_str()); };
    auto D = [&](const char* k, double& v) { if (kv.count(k)) v = std::atof(kv[k].c_str()); };
    S("pythonExe", p_.pythonExe);
    S("script", p_.script);
    S("runDir", p_.runDir);
    S("ksName", p_.ksName);
    S("cacheDir", p_.cacheDir);
    if (kv.count("goodOnly")) p_.goodOnly = kv["goodOnly"] == "1";
    D("maxRateHz", p_.maxRateHz);
    I("label", p_.label);
    I("nbins", p_.nbins);
    I("minSpikes", p_.minSpikes);
    I("nUnits", p_.nUnits);
    D("pad", p_.pad);
    D("winMs", p_.winMs);
    I("roll", p_.roll);
    if (status) *status = "Loaded " + path;
}

bool MsnFinderView::save(const std::string& path, std::string* err) const {
    std::ofstream f(path);
    if (!f) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    f << "# FeedbackPlease MSN finder settings\n"
      << "pythonExe=" << p_.pythonExe << "\nscript=" << p_.script << "\nrunDir=" << p_.runDir << "\nksName=" << p_.ksName
      << "\ncacheDir=" << p_.cacheDir << "\ngoodOnly=" << (p_.goodOnly ? 1 : 0) << "\nmaxRateHz=" << p_.maxRateHz
      << "\nlabel=" << p_.label << "\nnbins=" << p_.nbins << "\nminSpikes=" << p_.minSpikes << "\nnUnits=" << p_.nUnits
      << "\npad=" << p_.pad << "\nwinMs=" << p_.winMs << "\nroll=" << p_.roll << "\n";
    return bool(f);
}

// ---------------------------------------------------------------------------------------------
// Left panel

void MsnFinderView::drawParams() {
    const bool busy = backend_.busy();

    auto pathField = [&](const char* label, std::string* s, int kind, const char* hint = nullptr) {
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(16 * em());
        if (hint) ImGui::InputTextWithHint("##p", hint, s);
        else ImGui::InputText("##p", s);
        ImGui::SameLine();
        if (ImGui::Button("...")) {
            std::string f;
            if (kind == 0) f = pickFolderDialog("Select folder");
            else if (kind == 1) f = openFileDialog("Programs (*.exe)\0*.exe\0All files\0*.*\0");
            else f = openFileDialog("Python scripts (*.py)\0*.py\0All files\0*.*\0");
            if (!f.empty()) *s = f;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::PopID();
    };

    if (ImGui::CollapsingHeader("Data", ImGuiTreeNodeFlags_DefaultOpen)) {
        pathField("Run folder", &p_.runDir, 0);
        HelpMarker("The CatGT output folder of one run, e.g. X:/Choc57_out/catgt_Choc57_d2.0_g0. It holds the tcat nidq "
                   "files, the sync edge files and the <run>_imec0 folder with the Kilosort output.");
        ImGui::SetNextItemWidth(10 * em());
        ImGui::InputText("Kilosort folder", &p_.ksName);
        HelpMarker("Name of the Kilosort output folder inside <run>_imec0.");
        Check("Good units only", &p_.goodOnly);
        HelpMarker("Only clusters Kilosort labelled 'good'.");
        ImGui::BeginDisabled(!p_.goodOnly);
        InputD("Max rate (Hz)", &p_.maxRateHz, "%.1f", 6);
        HelpMarker("Drop clusters firing faster than this (noise-like). 0 = no cutoff.");
        ImGui::EndDisabled();
        pathField("Cache folder", &p_.cacheDir, 0, "default: next to the script");
        HelpMarker("Where the decoded syllable labels and sound pulses are kept so the next load of a run is fast. "
                   "Point it at the msnFinder notebook's cache folder to reuse what it already decoded. No figures are saved.");
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Load session")) {
            std::string err;
            if (backend_.requestLoad(p_, err)) message_.clear();
            else message_ = err;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(reads spikes + syllables; first time per run can take a minute)");
    }

    if (ImGui::CollapsingHeader("Syllable and ranking", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto s = backend_.session();
        std::string cur = "label " + std::to_string(p_.label);
        if (s)
            for (size_t i = 0; i < s->labels.size(); ++i)
                if (s->labels[i] == p_.label) cur += "  (n=" + std::to_string(s->labelCounts[i]) + ")";
        ImGui::SetNextItemWidth(12 * em());
        if (ImGui::BeginCombo("Syllable", cur.c_str())) {
            if (s) {
                for (size_t i = 0; i < s->labels.size(); ++i) {
                    const std::string item = "label " + std::to_string(s->labels[i]) + "  (n=" + std::to_string(s->labelCounts[i]) + ")";
                    if (ImGui::Selectable(item.c_str(), s->labels[i] == p_.label)) {
                        p_.label = s->labels[i];
                        std::string err;
                        if (!backend_.requestRank(p_.label, p_.nbins, p_.minSpikes, p_.nUnits, err)) message_ = err;
                    }
                }
            } else {
                ImGui::TextDisabled("Load a session first");
            }
            ImGui::EndCombo();
        }
        HelpMarker("The syllable label (value of the 3-bit code on the NI lines).");
        InputI("Histogram bins", &p_.nbins, 6);
        p_.nbins = std::clamp(p_.nbins, 2, 500);
        HelpMarker("Each unit's spike times from onset to the median syllable duration go into this many bins; the "
                   "variance of that distribution is the ranking criterion (low = spikes concentrated at one time).");
        InputI("Min spikes", &p_.minSpikes, 6);
        HelpMarker("Skip units with fewer spikes than this in the syllable window.");
        InputI("Show lowest-variance units", &p_.nUnits, 6);
        HelpMarker("How many units to list. 0 = all that pass.");
        ImGui::BeginDisabled(busy || !s);
        if (ImGui::Button("Rank units")) {
            std::string err;
            if (backend_.requestRank(p_.label, p_.nbins, p_.minSpikes, p_.nUnits, err)) message_.clear();
            else message_ = err;
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Figure", ImGuiTreeNodeFlags_DefaultOpen)) {
        InputD("Raster margin", &p_.pad, "%.2f", 6);
        p_.pad = std::clamp(p_.pad, 0.0, 3.0);
        HelpMarker("Raster extends this fraction of the median syllable duration before onset and after the end. "
                   "Changing it reloads the figure.");
        InputD("Window (ms)", &p_.winMs, "%.1f", 6);
        p_.winMs = std::clamp(p_.winMs, 1.0, 1000.0);
        HelpMarker("Width of the counting window (gray), centred on the peak of the unit's firing histogram.");
        InputI("Rolling mean (renditions)", &p_.roll, 6);
        p_.roll = std::max(1, p_.roll);
    }

    if (ImGui::CollapsingHeader("Python")) {
        pathField("Python", &p_.pythonExe, 1);
        HelpMarker("python.exe with numpy and scipy.");
        pathField("Script", &p_.script, 2, msnDefaultScript().c_str());
        HelpMarker("Empty = the default server script. Relative paths are taken from the folder of FeedbackPlease.exe.");
        if (ImGui::Button("Restart server")) {
            backend_.restart();
            message_ = "Server stopped; it restarts on the next load.";
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Synchronisation with the back end

void MsnFinderView::selectRank(int r) {
    if (!ranking_ || ranking_->rows.empty()) return;
    sel_ = std::clamp(r, 0, int(ranking_->rows.size()) - 1);
}

void MsnFinderView::prepareSpec(const MsnSpec& s) {
    const int nf = int(s.freqHz.size()), nt = int(s.timeFrac.size());
    specRows_ = specCols_ = 0;
    specVals_.clear();
    if (nf < 2 || nt < 2 || int(s.db.size()) != nf * nt) return;
    specRows_ = std::min(kSpecOut, nf);
    specCols_ = std::min(kSpecOut, nt);
    specVals_.assign(size_t(specRows_) * specCols_, 0.f);
    // Box-average down to the display size; row 0 of the result is the highest frequency (ImPlot draws row 0 on top).
    for (int r = 0; r < specRows_; ++r) {
        const int f0 = r * nf / specRows_, f1 = std::max(f0 + 1, (r + 1) * nf / specRows_);
        for (int c = 0; c < specCols_; ++c) {
            const int t0 = c * nt / specCols_, t1 = std::max(t0 + 1, (c + 1) * nt / specCols_);
            double sum = 0;
            for (int i = f0; i < f1; ++i)
                for (int j = t0; j < t1; ++j) sum += s.db[size_t(i) * nt + j];
            specVals_[size_t(specRows_ - 1 - r) * specCols_ + c] = float(sum / ((f1 - f0) * (t1 - t0)));
        }
    }
    specLo_ = float(percentile(specVals_, 0.02));
    specHi_ = float(percentile(specVals_, 0.995));
    if (specHi_ <= specLo_) specHi_ = specLo_ + 1;
}

void MsnFinderView::sync() {
    {
        std::vector<std::string> add;
        logCursor_ = backend_.fetchLog(logCursor_, add);
        for (auto& l : add) log_.push_back(std::move(l));
        if (log_.size() > 1500) log_.erase(log_.begin(), log_.begin() + (log_.size() - 1500));
    }

    if (backend_.sessionVersion() != seenSession_) {
        seenSession_ = backend_.sessionVersion();
        session_ = backend_.session();
        ranking_.reset();
        fig_.reset();
        reqLabel_ = reqUnit_ = -1;
        sel_ = 0;
        if (session_ && !session_->labels.empty()) {
            if (std::find(session_->labels.begin(), session_->labels.end(), p_.label) == session_->labels.end())
                p_.label = session_->labels.front();
            std::string err;
            if (!backend_.requestRank(p_.label, p_.nbins, p_.minSpikes, p_.nUnits, err)) message_ = err;
        }
    }
    if (backend_.rankingVersion() != seenRanking_) {
        seenRanking_ = backend_.rankingVersion();
        ranking_ = backend_.ranking();
        sel_ = 0;
        reqLabel_ = reqUnit_ = -1;
    }
    if (backend_.figureVersion() != seenFigure_) {
        seenFigure_ = backend_.figureVersion();
        fig_ = backend_.figure();
        const auto sp = backend_.spec();
        if (sp && sp != specSrc_) {
            specSrc_ = sp;
            prepareSpec(*sp);
        }
        if (!fig_) {
            specSrc_.reset();
            specVals_.clear();
            specRows_ = specCols_ = 0;
        }
    }

    // Ask for the figure of the selected unit (one request at a time; flipping fast just skips ahead).
    if (ranking_ && !ranking_->rows.empty() && !backend_.busy()) {
        sel_ = std::clamp(sel_, 0, int(ranking_->rows.size()) - 1);
        const int unit = ranking_->rows[sel_].unit;
        if (unit != reqUnit_ || ranking_->label != reqLabel_ || p_.pad != reqPad_) {
            std::string err;
            if (backend_.requestFigure(ranking_->label, unit, p_.pad, err)) {
                reqUnit_ = unit;
                reqLabel_ = ranking_->label;
                reqPad_ = p_.pad;
            } else {
                message_ = err;
                reqUnit_ = unit;  // don't retry every frame
                reqLabel_ = ranking_->label;
                reqPad_ = p_.pad;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Main area

void MsnFinderView::drawStatus() {
    const std::string err = backend_.lastError();
    if (backend_.busy()) {
        ImGui::TextColored(kAmber, "Working...");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel (restart server)")) backend_.restart();
    } else if (!session_) {
        ImGui::TextDisabled("No session loaded. Set the run folder and press Load session.");
    } else {
        ImGui::Text("%s  |  %d units  |  %.0f s", session_->name.c_str(), session_->nUnits, session_->durationSec);
    }
    if (!err.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kRed, "%s", err.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("dismiss")) backend_.clearError();
    } else if (!message_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "%s", message_.c_str());
    }
}

void MsnFinderView::drawRankTable(float height) {
    if (!ranking_ || ranking_->rows.empty()) return;
    if (!ImGui::BeginTable("msn_rank", 5,
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                               ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, height)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("rank");
    ImGui::TableSetupColumn("unit");
    ImGui::TableSetupColumn("variance (ms^2)");
    ImGui::TableSetupColumn("spikes");
    ImGui::TableSetupColumn("peak (ms)");
    ImGui::TableHeadersRow();
    for (int i = 0; i < int(ranking_->rows.size()); ++i) {
        const MsnRankRow& r = ranking_->rows[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char lab[32];
        std::snprintf(lab, sizeof lab, "%d##r%d", i + 1, i);
        if (ImGui::Selectable(lab, i == sel_, ImGuiSelectableFlags_SpanAllColumns)) sel_ = i;
        ImGui::TableNextColumn();
        ImGui::Text("%d", r.unit);
        ImGui::TableNextColumn();
        ImGui::Text("%.0f", r.variance);
        ImGui::TableNextColumn();
        ImGui::Text("%.0f", r.nSpikes);
        ImGui::TableNextColumn();
        ImGui::Text("%.0f", r.peakMs);
    }
    ImGui::EndTable();
}

void MsnFinderView::drawMain() {
    sync();
    drawStatus();

    if (ranking_ && !ranking_->rows.empty()) {
        const int n = int(ranking_->rows.size());
        // Page through units: arrow keys (when not typing) or the buttons.
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) selectRank(sel_ + 1);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) selectRank(sel_ - 1);
        }
        if (ImGui::Button("<  prev")) selectRank(sel_ - 1);
        ImGui::SameLine();
        if (ImGui::Button("next  >")) selectRank(sel_ + 1);
        ImGui::SameLine();
        int rank = sel_ + 1;
        ImGui::SetNextItemWidth(14 * em());
        if (ImGui::SliderInt("##rank", &rank, 1, n, "rank %d")) selectRank(rank - 1);
        const MsnRankRow& r = ranking_->rows[std::clamp(sel_, 0, n - 1)];
        ImGui::SameLine();
        ImGui::Text("of %d   unit %d   variance %.0f   %0.f spikes   (arrow keys also page)", n, r.unit, r.variance, r.nSpikes);
    }

    const float avail = ImGui::GetContentRegionAvail().y;
    const bool haveRank = ranking_ && !ranking_->rows.empty();
    if (haveRank && ImGui::CollapsingHeader("Ranking table")) drawRankTable(std::min(10 * em(), avail * 0.3f));
    const float logH = showLog_ ? 7 * em() : 0;
    drawFigure(std::max(12 * em(), ImGui::GetContentRegionAvail().y - logH - 2 * em()));

    ImGui::Checkbox("Log", &showLog_);
    if (showLog_) {
        ImGui::BeginChild("msn_log", ImVec2(0, 0), ImGuiChildFlags_Border);
        for (const auto& l : log_) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
}

void MsnFinderView::drawFigure(float height) {
    if (!fig_) {
        ImGui::Dummy(ImVec2(0, height));
        return;
    }
    const MsnFigure& f = *fig_;
    const bool stale = ranking_ && !ranking_->rows.empty() &&
                       (f.unit != ranking_->rows[std::clamp(sel_, 0, int(ranking_->rows.size()) - 1)].unit ||
                        f.label != ranking_->label);

    char title[256];
    std::snprintf(title, sizeof title, "syllable %d (n=%d, median %.0f ms)  |  unit %d  |  rank %d  |  peak %.0f ms%s", f.label,
                  f.nRenditions, f.medianMs, f.unit, sel_ + 1, f.peakMs, stale ? "   (loading...)" : "");
    ImGui::TextColored(stale ? kGrey : ImVec4(1, 1, 1, 1), "%s", title);
    height -= ImGui::GetTextLineHeightWithSpacing();

    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float leftW = std::floor((ImGui::GetContentRegionAvail().x - gap) * 0.76f);
    const bool haveWin = std::isfinite(f.peakMs);
    const double lo = f.peakMs - p_.winMs / 2, hi = f.peakMs + p_.winMs / 2;

    // x limits shared by the spectrogram, raster and rolling plots
    std::vector<float> ends = f.endMs;
    const double q25 = percentile(ends, 0.25), q50 = percentile(ends, 0.5), q75 = percentile(ends, 0.75),
                 q95 = percentile(ends, 0.95);
    const double x0 = -p_.pad * f.medianMs, x1 = q95 + p_.pad * f.medianMs;
    ImPlotCond xCond = ImPlotCond_Once;
    if (f.label != limLabel_ || p_.pad != limPad_) {  // re-fit when the syllable or margin changes
        xMin_ = x0;
        xMax_ = x1;
        limLabel_ = f.label;
        limPad_ = p_.pad;
        xCond = ImPlotCond_Always;
    }

    ImGui::BeginChild("msn_left", ImVec2(leftW, height));
    const float hs = ImGui::GetStyle().ItemSpacing.y;
    const float hSpec = (height - 2 * hs) * 0.27f, hRas = (height - 2 * hs) * 0.50f, hRoll = (height - 2 * hs) * 0.23f;

    // ---- mean spectrogram of the renditions, warped to the median duration
    if (ImPlot::BeginPlot("##msn_spec", ImVec2(-1, hSpec), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, "kHz", ImPlotAxisFlags_NoTickLabels, 0);
        ImPlot::SetupAxisLinks(ImAxis_X1, &xMin_, &xMax_);
        ImPlot::SetupAxisLimits(ImAxis_X1, xMin_, xMax_, xCond);
        if (specRows_ > 0 && specSrc_) {
            const double fLo = specSrc_->freqHz.front() / 1e3, fHi = specSrc_->freqHz.back() / 1e3;
            ImPlot::SetupAxisLimits(ImAxis_Y1, fLo, fHi, ImPlotCond_Once);
            ImPlot::PushColormap(ImPlotColormap_Hot);
            ImPlot::PlotHeatmap("##h", specVals_.data(), specRows_, specCols_, specLo_, specHi_, nullptr, ImPlotPoint(0, fLo),
                                ImPlotPoint(f.medianMs, fHi));
            ImPlot::PopColormap();
        }
        if (haveWin) shadeX(lo, hi, col(ImVec4(0.7f, 0.7f, 0.7f, 1), 0.3f));
        ImPlot::EndPlot();
    }

    // ---- raster: one row per rendition, unwarped ms after onset
    if (ImPlot::BeginPlot("##msn_raster", ImVec2(-1, hRas), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes("ms after syllable onset (unwarped)", "rendition", 0, ImPlotAxisFlags_Invert);
        ImPlot::SetupAxisLinks(ImAxis_X1, &xMin_, &xMax_);
        ImPlot::SetupAxisLimits(ImAxis_X1, xMin_, xMax_, xCond);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -1, std::max(1, f.nRenditions), ImPlotCond_Once);
        if (haveWin) shadeX(lo, hi, col(ImVec4(0.7f, 0.7f, 0.7f, 1), 0.3f));
        shadeX(q25, q75, col(kRed, 0.15f));
        const double zero = 0, medEnd = q50;
        ImPlot::SetNextLineStyle(kRed, 1.0f);
        ImPlot::PlotInfLines("##zero", &zero, 1);
        ImPlot::SetNextLineStyle(kRed, 1.0f);
        ImPlot::PlotInfLines("##end", &medEnd, 1);
        const double ey = -0.5, en = q50 - q25, ep = q75 - q50;
        ImPlot::SetNextLineStyle(kRed, 1.0f);
        ImPlot::PlotErrorBars("##iqr", &medEnd, &ey, &en, &ep, 1, ImPlotErrorBarsFlags_Horizontal);
        {
            // ticks drawn straight into the plot: one short vertical line per spike
            const float row = std::fabs(ImPlot::PlotToPixels(0, 1).y - ImPlot::PlotToPixels(0, 0).y);
            const float half = std::clamp(row * 0.45f, 1.0f, 6.0f);
            ImDrawList* dl = ImPlot::GetPlotDrawList();
            ImPlot::PushPlotClipRect();
            const ImU32 black = ImGui::ColorConvertFloat4ToU32(ImVec4(0.92f, 0.92f, 0.92f, 1));
            const size_t n = std::min(f.spikeMs.size(), f.spikeRendition.size());
            for (size_t i = 0; i < n; ++i) {
                const ImVec2 p = ImPlot::PlotToPixels(f.spikeMs[i], f.spikeRendition[i]);
                dl->AddLine(ImVec2(p.x, p.y - half), ImVec2(p.x, p.y + half), black, 1.0f);
            }
            ImPlot::PopPlotClipRect();
        }
        ImPlot::EndPlot();
    }

    // ---- spikes in the window per rendition, averaged over runs of `roll` renditions
    if (ImPlot::BeginPlot("##msn_roll", ImVec2(-1, hRoll), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        char ylab[48];
        std::snprintf(ylab, sizeof ylab, "spikes (%g ms)", p_.winMs);
        char xlab[64];
        std::snprintf(xlab, sizeof xlab, "start rendition (mean over %d renditions)", p_.roll);
        ImPlot::SetupAxes(xlab, ylab, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        if (haveWin && f.nRenditions >= p_.roll) {
            std::vector<double> counts(f.nRenditions, 0.0);
            const size_t n = std::min(f.spikeMs.size(), f.spikeRendition.size());
            for (size_t i = 0; i < n; ++i)
                if (f.spikeMs[i] >= lo && f.spikeMs[i] < hi && f.spikeRendition[i] >= 0 && f.spikeRendition[i] < f.nRenditions)
                    counts[f.spikeRendition[i]] += 1;
            const int m = f.nRenditions - p_.roll + 1;
            std::vector<double> xs(m), ys(m);
            double run = 0;
            for (int i = 0; i < p_.roll; ++i) run += counts[i];
            for (int i = 0; i < m; ++i) {
                xs[i] = i + 1;
                ys[i] = run / p_.roll;
                if (i + p_.roll < f.nRenditions) run += counts[i + p_.roll] - counts[i];
            }
            ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.9f, 0.9f, 1), 1.5f);
            ImPlot::PlotLine("##mean", xs.data(), ys.data(), m);
        }
        ImPlot::EndPlot();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("msn_right", ImVec2(0, height));
    const float hw = (height - hs) * 0.36f;

    if (ImPlot::BeginPlot("##msn_wf", ImVec2(-1, hw), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes("ms from peak deflection", "template (a.u.)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        if (!f.wf.empty() && f.wfT.size() == f.wf.size()) {
            ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.9f, 0.9f, 1), 1.5f);
            ImPlot::PlotLine("##wf", f.wfT.data(), f.wf.data(), int(f.wf.size()));
        }
        ImPlot::EndPlot();
    }

    // ---- where the unit sits on its shank: channels, those the template reaches (area ~ amplitude), peak channel
    char ptitle[96];
    std::snprintf(ptitle, sizeof ptitle, "shank %d, peak ch %d, y=%.0f um##msn_pos", f.peakShank, f.peakChannel, f.peakYum);
    if (ImPlot::BeginPlot(ptitle, ImVec2(-1, -1), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes("x (um)", "depth y (um)");
        const size_t nch = f.fpAmp.size();
        if (nch > 0 && f.fpXY.size() >= 2 * nch && f.fpShank.size() >= nch && int(nch) > f.peakChannel) {
            const int shank = f.fpShank[f.peakChannel];
            const float ampPeak = std::max(1e-9f, f.fpAmp[f.peakChannel]);
            double xa = 1e18, xb = -1e18, ya = 1e18, yb = -1e18;
            std::vector<double> gx, gy;
            for (size_t i = 0; i < nch; ++i) {
                if (f.fpShank[i] != shank) continue;
                gx.push_back(f.fpXY[2 * i]);
                gy.push_back(f.fpXY[2 * i + 1]);
                xa = std::min<double>(xa, f.fpXY[2 * i]);
                xb = std::max<double>(xb, f.fpXY[2 * i]);
                ya = std::min<double>(ya, f.fpXY[2 * i + 1]);
                yb = std::max<double>(yb, f.fpXY[2 * i + 1]);
            }
            if (!gx.empty()) ImPlot::SetupAxisLimits(ImAxis_X1, xa - 40, xb + 40, ImPlotCond_Once);
            if (!gx.empty()) ImPlot::SetupAxisLimits(ImAxis_Y1, ya - 20, yb + 20, ImPlotCond_Once);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Square, 3, ImVec4(0.6f, 0.6f, 0.6f, 1), -1, ImVec4(0.6f, 0.6f, 0.6f, 1));
            if (!gx.empty()) ImPlot::PlotScatter("##ch", gx.data(), gy.data(), int(gx.size()));
            ImDrawList* dl = ImPlot::GetPlotDrawList();
            ImPlot::PushPlotClipRect();
            for (size_t i = 0; i < nch; ++i) {
                if (f.fpShank[i] != shank || f.fpAmp[i] <= 0.1f * ampPeak) continue;
                const ImVec2 p = ImPlot::PlotToPixels(f.fpXY[2 * i], f.fpXY[2 * i + 1]);
                const float h = 2.0f + 8.0f * std::sqrt(f.fpAmp[i] / ampPeak);
                dl->AddRectFilled(ImVec2(p.x - h, p.y - h), ImVec2(p.x + h, p.y + h), col(kRed, 0.5f));
            }
            ImPlot::PopPlotClipRect();
            const double px = f.fpXY[2 * f.peakChannel], py = f.fpXY[2 * f.peakChannel + 1];
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Asterisk, 8, ImVec4(1, 1, 1, 1), 1.5f, ImVec4(1, 1, 1, 1));
            ImPlot::PlotScatter("##peak", &px, &py, 1);
        }
        ImPlot::EndPlot();
    }
    ImGui::EndChild();
}

}  // namespace fp

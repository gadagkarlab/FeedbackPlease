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
    if (kv.count("includeLow")) p_.includeLow = kv["includeLow"] == "1";
    D("pad", p_.pad);
    D("winMs", p_.winMs);
    I("roll", p_.roll);
    I("rankMode", p_.rankMode);
    D("minSparseness", p_.minSparseness);
    D("boutGapS", p_.boutGapS);
    D("nonSongMarginS", p_.nonSongMarginS);
    D("maxSongRate", p_.maxSongRate);
    D("maxNonSongRate", p_.maxNonSongRate);
    D("minWidthMs", p_.minWidthMs);
    I("minSongSpikes", p_.minSongSpikes);
    I("widthSource", p_.widthSource);
    I("rawSpikes", p_.rawSpikes);
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
      << "\nlabel=" << p_.label << "\nnbins=" << p_.nbins << "\nminSpikes=" << p_.minSpikes << "\nincludeLow=" << (p_.includeLow ? 1 : 0)
      << "\npad=" << p_.pad << "\nwinMs=" << p_.winMs << "\nroll=" << p_.roll << "\nrankMode=" << p_.rankMode
      << "\nminSparseness=" << p_.minSparseness
      << "\nboutGapS=" << p_.boutGapS << "\nnonSongMarginS=" << p_.nonSongMarginS << "\nmaxSongRate=" << p_.maxSongRate
      << "\nmaxNonSongRate=" << p_.maxNonSongRate << "\nminWidthMs=" << p_.minWidthMs << "\nminSongSpikes=" << p_.minSongSpikes
      << "\nwidthSource=" << p_.widthSource << "\nrawSpikes=" << p_.rawSpikes << "\n";
    return bool(f);
}

// ---------------------------------------------------------------------------------------------
// Left panel

void MsnFinderView::drawParams() {
    const bool busy = backend_.busy();
    const auto s0 = backend_.session();

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
        {
            const char* modes[3] = {"Variance (syllable-locked)", "Type-1 likeness", "Sparseness (paper)"};
            ImGui::SetNextItemWidth(14 * em());
            if (ImGui::BeginCombo("Rank by", modes[std::clamp(p_.rankMode, 0, 2)])) {
                for (int m = 0; m < 3; ++m)
                    if (ImGui::Selectable(modes[m], p_.rankMode == m)) p_.rankMode = m;
                ImGui::EndCombo();
            }
            HelpMarker("Variance: units whose spikes fall at one time within the chosen syllable come first.\n"
                       "Type-1 likeness: units that look like the medium spiny neurons of Goldberg & Fee 2010 come first: low "
                       "firing rate during song, nearly silent outside song, wide spike. The syllable only sets the figure shown.\n"
                       "Sparseness: the paper's entropy index of syllable-aligned firing (10 ms bins, all syllables with 30+ "
                       "renditions together); units that fire in a few brief moments of the song come first.");
        }
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
                        reqLabel_ = -1;
                        std::string err;
                        if (!backend_.requestRank(p_.label, p_.nbins, p_.minSpikes, err)) message_ = err;
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
        if (p_.rankMode != 0) {
            Check("Include low-spike units", &p_.includeLow);
            HelpMarker("Also list the units that fail the spike-count criteria (Min spikes; Min spikes in song in type-1 mode), "
                       "after the ranked ones and in grey. Their rates and indices rest on very few spikes. Either way, a grey dot "
                       "in the scatters can be clicked to view that unit.");
        }
        ImGui::BeginDisabled(busy || !s);
        if (ImGui::Button("Rank units")) {
            std::string err;
            if (backend_.requestRank(p_.label, p_.nbins, p_.minSpikes, err)) message_.clear();
            else message_ = err;
        }
        ImGui::EndDisabled();
    }

    if (p_.rankMode == 2 && ImGui::CollapsingHeader("Sparseness", ImGuiTreeNodeFlags_DefaultOpen)) {
        InputD("Sparse above", &p_.minSparseness, "%.2f", 6);
        p_.minSparseness = std::clamp(p_.minSparseness, 0.0, 1.0);
        HelpMarker("Units at or above this index are drawn green in the scatters. Paper: type-1 0.45 +- 0.14, types 2-4 about 0.02. "
                   "1 = all firing in one 10 ms bin, 0 = spread evenly over the song. Units with fewer than 'Min spikes' spikes "
                   "in the syllable windows are not listed (the index is meaningless on a handful of spikes).");
        if (const auto cl = backend_.classification()) {
            int n = 0;
            for (double v : cl->sparseness) n += std::isfinite(v);
            ImGui::TextDisabled("%d units have an index", n);
        }
    }

    if (p_.rankMode == 1 && ImGui::CollapsingHeader("Type-1 criteria and song bouts", ImGuiTreeNodeFlags_DefaultOpen)) {
        InputD("Song rate below (Hz)", &p_.maxSongRate, "%.3g", 6);
        HelpMarker("Mean instantaneous rate (1 / enclosing inter-spike interval, averaged over time) during song. Paper: < 10 Hz.");
        InputD("Non-song rate below (Hz)", &p_.maxNonSongRate, "%.3g", 6);
        HelpMarker("Same measure far from song. Paper: < 0.01 Hz, i.e. essentially silent.");
        InputD("Spike half-width above (ms)", &p_.minWidthMs, "%.3g", 6);
        HelpMarker("Half-width of the unit's dominant deflection (a peak-dominant unit is flipped; marked (+) in the table). "
                   "Paper: > 0.06 ms on mean waveforms; check the scatter for where your units actually fall.");
        {
            const char* src[2] = {"Kilosort template", "Mean raw waveform"};
            ImGui::SetNextItemWidth(14 * em());
            if (ImGui::BeginCombo("Width from", src[std::clamp(p_.widthSource, 0, 1)])) {
                for (int m = 0; m < 2; ++m)
                    if (ImGui::Selectable(src[m], p_.widthSource == m)) p_.widthSource = m;
                ImGui::EndCombo();
            }
            HelpMarker("Template: from the Kilosort template of the unit's peak channel. Mean raw waveform: average of the unit's "
                       "spikes read from the .ap.bin (30 kHz, upsampled 8x, each spike aligned on its own trough or peak before "
                       "averaging); measure it first with the button below.");
            InputI("Spikes per unit", &p_.rawSpikes, 6);
            p_.rawSpikes = std::clamp(p_.rawSpikes, 10, 2000);
            ImGui::BeginDisabled(busy || !s0);
            if (ImGui::Button("Measure raw waveforms")) {
                std::string err;
                if (backend_.requestRawWidths(p_.rawSpikes, err)) message_.clear();
                else message_ = err;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (const auto rw = backend_.rawWidths()) ImGui::TextDisabled("%d units measured", int(rw->units.size()));
            else ImGui::TextDisabled("not measured (about a minute)");
            if (p_.widthSource == 1 && !backend_.rawWidths()) ImGui::TextColored(kAmber, "Using template widths until the raw waveforms are measured.");
        }
        InputI("Min spikes in song", &p_.minSongSpikes, 6);
        p_.minSongSpikes = std::max(0, p_.minSongSpikes);
        HelpMarker("Units with fewer spikes during song are not listed (they are not singing-related).");
        ImGui::Separator();
        InputD("Bout gap (s)", &p_.boutGapS, "%.3g", 6);
        p_.boutGapS = std::max(0.0, p_.boutGapS);
        HelpMarker("Sound pulses (digital line high while the mic is over threshold) closer together than this belong to one song bout.");
        InputD("Non-song margin (s)", &p_.nonSongMarginS, "%.3g", 6);
        p_.nonSongMarginS = std::max(0.0, p_.nonSongMarginS);
        HelpMarker("Non-song time is further than this from every bout. The paper uses 10 s; a run with little silence "
                   "may have no such time (see below).");
        ImGui::BeginDisabled(busy || !s0);
        if (ImGui::Button("Recompute rates")) {
            std::string err;
            if (backend_.requestClassify(p_.boutGapS, p_.nonSongMarginS, err)) message_.clear();
            else message_ = err;
        }
        ImGui::EndDisabled();
        if (const auto cl = backend_.classification()) {
            ImGui::TextDisabled("%d bouts, %.0f s of song, %.0f s of non-song", cl->nBouts, cl->songSec, cl->nonSongSec);
            if (cl->nonSongSec < 30.0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
                ImGui::TextWrapped("Little or no non-song time: the outside-song rate is %s.",
                                   cl->nonSongSec <= 0 ? "unavailable (ignored in the ranking)" : "unreliable");
                ImGui::PopStyleColor();
            }
        }
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
    if (cands_.empty()) return;
    sel_ = std::clamp(r, 0, int(cands_.size()) - 1);
    extraUnit_ = -1;
    selUnit_ = cands_[sel_].unit;
}

int MsnFinderView::curUnit() const {
    if (extraUnit_ >= 0) return extraUnit_;
    return cands_.empty() ? -1 : cands_[std::clamp(sel_, 0, int(cands_.size()) - 1)].unit;
}

int MsnFinderView::curLabel() const {
    if (extraUnit_ >= 0 && p_.rankMode != 0) return p_.label;
    if (cands_.empty()) return -1;
    if (p_.rankMode != 0) return p_.label;
    return ranking_ ? ranking_->label : -1;
}

namespace {
double finiteOr(double v, double d) { return std::isfinite(v) ? v : d; }
// How far a unit is from meeting every type-1 criterion, in decades (<= 0: meets them all; the more negative,
// the more clearly). A criterion that cannot be evaluated (no non-song time) is skipped.
double type1Score(const MsnClassification& c, size_t i, const MsnParams& p, double width) {
    const double floorRate = 1e-4;
    double worst = -1e9;
    if (std::isfinite(c.songRate[i])) worst = std::max(worst, std::log10(std::max(c.songRate[i], floorRate) / p.maxSongRate));
    else worst = std::max(worst, 5.0);
    if (std::isfinite(c.nonSongRate[i])) worst = std::max(worst, std::log10(std::max(c.nonSongRate[i], floorRate) / p.maxNonSongRate));
    if (std::isfinite(width)) worst = std::max(worst, std::log10(p.minWidthMs / std::max(width, 1e-3)));
    else worst = std::max(worst, 5.0);
    return worst;
}
// Ranked units first (best first), then the ones that fail the spike-count criteria (if listed).
template <class V>
void sortCands(V& v) {
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.low != b.low ? !a.low : a.score < b.score; });
}
}  // namespace

// The list of units to page through for the current ranking mode, best first. Rebuilt only when its inputs change.
// Spike width used for unit i of the classification: mean raw waveform if chosen and measured, else the template.
double MsnFinderView::effWidth(size_t i) const {
    if (p_.widthSource == 1 && raw_ && cls_ && i < cls_->units.size()) {
        const auto it = rawIndex_.find(cls_->units[i]);
        if (it != rawIndex_.end() && std::isfinite(raw_->widthMs[it->second])) return raw_->widthMs[it->second];
    }
    return cls_ && i < cls_->widthMs.size() ? cls_->widthMs[i] : double(NAN);
}

void MsnFinderView::rebuildCandidates() {
    char key[256];
    std::snprintf(key, sizeof key, "%d|%llu|%llu|%llu|%d|%g|%g|%g|%d|%d|%d|%g|%d", p_.rankMode, (unsigned long long)seenRanking_,
                  (unsigned long long)seenClass_, (unsigned long long)seenRaw_, p_.widthSource, p_.maxSongRate, p_.maxNonSongRate, p_.minWidthMs, p_.minSongSpikes, int(p_.includeLow),
                  int(p_.label), p_.minSparseness, p_.minSpikes);
    if (candKey_ == key) return;
    candKey_ = key;

    std::vector<Cand> out;
    if (p_.rankMode == 0) {
        if (ranking_)
            for (const auto& r : ranking_->rows) out.push_back({r.unit, r.variance, -1, false, false});
    } else if (cls_ && p_.rankMode == 2) {
        for (size_t i = 0; i < cls_->units.size(); ++i) {
            const bool ok = std::isfinite(cls_->sparseness[i]) && cls_->sparseN[i] >= p_.minSpikes;
            if (!ok && !p_.includeLow) continue;
            const double sp = cls_->sparseness[i];
            out.push_back({cls_->units[i], std::isfinite(sp) ? -sp : 1e9, int(i), ok && sp >= p_.minSparseness, !ok});
        }
        sortCands(out);
    } else if (cls_) {
        for (size_t i = 0; i < cls_->units.size(); ++i) {
            const bool ok = cls_->nSong[i] >= p_.minSongSpikes;
            if (!ok && !p_.includeLow) continue;
            const double sc = type1Score(*cls_, i, p_, effWidth(i));
            out.push_back({cls_->units[i], sc, int(i), ok && sc <= 0, !ok});
        }
        sortCands(out);
    }
    cands_ = std::move(out);

    sel_ = 0;
    for (size_t i = 0; i < cands_.size(); ++i)
        if (cands_[i].unit == selUnit_) sel_ = int(i);
    selUnit_ = cands_.empty() ? -1 : cands_[size_t(sel_)].unit;
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
        cls_.reset();
        raw_.reset();
        rawIndex_.clear();
        fig_.reset();
        reqLabel_ = reqUnit_ = -1;
        sel_ = 0;
        selUnit_ = -1;
        candKey_.clear();
        if (session_ && !session_->labels.empty()) {
            if (std::find(session_->labels.begin(), session_->labels.end(), p_.label) == session_->labels.end())
                p_.label = session_->labels.front();
            std::string err;
            if (!backend_.requestRank(p_.label, p_.nbins, p_.minSpikes, err)) message_ = err;
            if (!backend_.requestClassify(p_.boutGapS, p_.nonSongMarginS, err)) message_ = err;
        }
    }
    if (backend_.classificationVersion() != seenClass_) {
        seenClass_ = backend_.classificationVersion();
        cls_ = backend_.classification();
        reqLabel_ = reqUnit_ = -1;
    }
    if (backend_.rawWidthsVersion() != seenRaw_) {
        seenRaw_ = backend_.rawWidthsVersion();
        raw_ = backend_.rawWidths();
        rawIndex_.clear();
        if (raw_)
            for (size_t i = 0; i < raw_->units.size(); ++i) rawIndex_[raw_->units[i]] = i;
        reqLabel_ = reqUnit_ = -1;
    }
    if (backend_.rankingVersion() != seenRanking_) {
        seenRanking_ = backend_.rankingVersion();
        ranking_ = backend_.ranking();
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

    rebuildCandidates();

    // Ask for the figure of the selected unit (one request at a time; flipping fast just skips ahead).
    const int unit = curUnit(), label = curLabel();
    if (unit >= 0 && label >= 0 && !backend_.busy() && (unit != reqUnit_ || label != reqLabel_ || p_.pad != reqPad_)) {
        std::string err;
        if (!backend_.requestFigure(label, unit, p_.pad, err)) message_ = err;
        reqUnit_ = unit;  // also on failure: don't retry every frame
        reqLabel_ = label;
        reqPad_ = p_.pad;
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
    if (cands_.empty()) return;
    const bool t1 = p_.rankMode != 0;
    if (!ImGui::BeginTable("msn_rank", t1 ? 9 : 5,
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                               ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, height)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("rank");
    ImGui::TableSetupColumn("unit");
    if (t1) {
        ImGui::TableSetupColumn("song rate (Hz)");
        ImGui::TableSetupColumn("non-song (Hz)");
        ImGui::TableSetupColumn("width (ms)");
        ImGui::TableSetupColumn("peak in song (Hz)");
        ImGui::TableSetupColumn("song spikes");
        ImGui::TableSetupColumn("sparseness");
        ImGui::TableSetupColumn(p_.rankMode == 2 ? "sparse?" : "type 1?");
    } else {
        ImGui::TableSetupColumn("variance (ms^2)");
        ImGui::TableSetupColumn("spikes");
        ImGui::TableSetupColumn("peak (ms)");
    }
    ImGui::TableHeadersRow();
    for (int i = 0; i < int(cands_.size()); ++i) {
        const Cand& c = cands_[size_t(i)];
        ImGui::TableNextRow();
        if (c.low) ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
        ImGui::TableNextColumn();
        char lab[32];
        std::snprintf(lab, sizeof lab, "%d##r%d", i + 1, i);
        if (ImGui::Selectable(lab, i == sel_, ImGuiSelectableFlags_SpanAllColumns)) selectRank(i);
        ImGui::TableNextColumn();
        ImGui::Text("%d", c.unit);
        if (t1 && cls_ && c.cls >= 0) {
            const size_t k = size_t(c.cls);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", cls_->songRate[k]);
            ImGui::TableNextColumn();
            if (std::isfinite(cls_->nonSongRate[k])) ImGui::Text("%.4f", cls_->nonSongRate[k]);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            {
                const double wv = effWidth(k);
                double pol = cls_->polarity[k];
                if (p_.widthSource == 1 && raw_) {
                    const auto ri = rawIndex_.find(c.unit);
                    if (ri != rawIndex_.end() && raw_->polarity[ri->second] != 0) pol = raw_->polarity[ri->second];
                }
                ImGui::Text("%.3f%s", wv, pol < 0 ? " (+)" : "");
            }
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", cls_->peakRate[k]);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", cls_->nSong[k]);
            ImGui::TableNextColumn();
            if (std::isfinite(cls_->sparseness[k])) ImGui::Text("%.3f", cls_->sparseness[k]);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (c.meets) ImGui::TextColored(kGreen, "yes");
            else if (p_.rankMode == 2) ImGui::TextDisabled("no");
            else ImGui::TextDisabled("%.1f dec. off", c.score);
        } else if (!t1 && ranking_ && i < int(ranking_->rows.size())) {
            const MsnRankRow& r = ranking_->rows[size_t(i)];
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", r.variance);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", r.nSpikes);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", r.peakMs);
        }
        if (c.low) ImGui::PopStyleColor();
    }
    ImGui::EndTable();
}

// The classification scatters of Goldberg & Fee 2010, Fig. 1D-F: song rate vs non-song rate, song rate vs spike
// width, peak song rate vs spike width. Gray: units that are not ranked (few spikes); blue: on the list; green: meets every type-1
// criterion; red: the unit being viewed (click any listed unit to jump to it).
void MsnFinderView::drawScatters(float height) {
    if (!cls_ || cls_->units.empty()) return;
    const MsnClassification& c = *cls_;
    const double zeroX = 3e-4;  // rates of exactly 0 can't be shown on a log axis: drawn here
    auto logRate = [&](double v) { return std::isfinite(v) ? std::max(v, zeroX) : double(NAN); };

    std::map<int, int> rowOf;  // unit -> position in cands_
    for (size_t i = 0; i < cands_.size(); ++i) rowOf[cands_[i].unit] = int(i);

    const float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 4.0f;
    struct Spec {
        const char* id;
        const char* xl;
        const char* yl;
        bool xLog;
        double x0, x1, y0, y1;
        double vx, hy;  // threshold lines
    };
    const Spec specs[4] = {
        {"##sc_rates", "non-song rate (Hz)", "song rate (Hz)", true, 1e-4, 300, 1e-2, 300, p_.maxNonSongRate, p_.maxSongRate},
        {"##sc_width_rate", "spike half-width (ms)", "song rate (Hz)", false, 0, 0.6, 1e-2, 300, p_.minWidthMs, p_.maxSongRate},
        {"##sc_width_peak", "spike half-width (ms)", "peak rate in song (Hz)", false, 0, 0.6, 10, 5000, p_.minWidthMs, 600.0},
        {"##sc_sparse", "sparseness index", "song rate (Hz)", false, 0, 1, 1e-2, 300, p_.minSparseness, p_.maxSongRate}};

    for (int s = 0; s < 4; ++s) {
        if (s) ImGui::SameLine();
        const Spec& sp = specs[s];
        if (!ImPlot::BeginPlot(sp.id, ImVec2(w, height), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) continue;
        ImPlot::SetupAxes(sp.xl, sp.yl);
        if (sp.xLog) ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxisLimits(ImAxis_X1, sp.x0, sp.x1, ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, sp.y0, sp.y1, ImPlotCond_Once);

        std::vector<double> gx, gy, bx, by, kx, ky;  // other / listed / listed and meeting every criterion
        std::vector<int> pickUnit;
        std::vector<double> pickX, pickY;
        double cx = NAN, cy = NAN;
        for (size_t i = 0; i < c.units.size(); ++i) {
            double x = s == 0 ? logRate(c.nonSongRate[i]) : s == 3 ? c.sparseness[i] : effWidth(i);
            const double y = s == 2 ? logRate(c.peakRate[i]) : logRate(c.songRate[i]);
            if (!std::isfinite(y)) continue;
            if (!std::isfinite(x)) {
                if (s != 0) continue;
                x = sp.x0 * 1.5;  // no non-song data: park at the left edge
            }
            const auto it = rowOf.find(c.units[i]);
            const bool listed = it != rowOf.end() && !cands_[size_t(it->second)].low;
            if (!listed) {
                gx.push_back(x);
                gy.push_back(y);
            } else {
                const bool meets = cands_[size_t(it->second)].meets;
                (meets ? kx : bx).push_back(x);
                (meets ? ky : by).push_back(y);
            }
            pickUnit.push_back(c.units[i]);
            pickX.push_back(x);
            pickY.push_back(y);
            if (c.units[i] == curUnit()) {
                cx = x;
                cy = y;
            }
        }
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3, ImVec4(0.45f, 0.45f, 0.45f, 1), -1, ImVec4(0.45f, 0.45f, 0.45f, 1));
        if (!gx.empty()) ImPlot::PlotScatter("##other", gx.data(), gy.data(), int(gx.size()));
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 4, kBlue, -1, kBlue);
        if (!bx.empty()) ImPlot::PlotScatter("##listed", bx.data(), by.data(), int(bx.size()));
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 5, kGreen, -1, kGreen);
        if (!kx.empty()) ImPlot::PlotScatter("##meets", kx.data(), ky.data(), int(kx.size()));
        ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.7f, 0.2f, 0.6f), 1.0f);
        ImPlot::PlotInfLines("##vx", &sp.vx, 1);
        ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.7f, 0.2f, 0.6f), 1.0f);
        ImPlot::PlotInfLines("##hy", &sp.hy, 1, ImPlotInfLinesFlags_Horizontal);
        if (std::isfinite(cx)) {  // the unit being viewed
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 11, ImVec4(0, 0, 0, 0), 2.5f, kRed);
            ImPlot::PlotScatter("##cur", &cx, &cy, 1);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 4, kRed, -1, kRed);
            ImPlot::PlotScatter("##cur2", &cx, &cy, 1);
        }
        if (ImPlot::IsPlotHovered()) {
            const ImVec2 m = ImGui::GetMousePos();
            float best = 12.0f;
            int pick = -1;
            for (size_t i = 0; i < pickUnit.size(); ++i) {
                const ImVec2 p = ImPlot::PlotToPixels(pickX[i], pickY[i]);
                const float d = std::hypot(p.x - m.x, p.y - m.y);
                if (d < best) {
                    best = d;
                    pick = pickUnit[i];
                }
            }
            if (pick >= 0) {
                ImGui::SetTooltip("unit %d  (click to view)", pick);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    const auto it = rowOf.find(pick);
                    if (it != rowOf.end()) selectRank(it->second);
                    else extraUnit_ = pick;
                }
            }
        }
        ImPlot::EndPlot();
    }
}

void MsnFinderView::drawMain() {
    sync();
    drawStatus();

    if (!cands_.empty()) {
        const int n = int(cands_.size());
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
        char rankFmt[32];
        std::snprintf(rankFmt, sizeof rankFmt, "rank %%d of %d", n);
        ImGui::SetNextItemWidth(14 * em());
        if (ImGui::SliderInt("##rank", &rank, 1, n, rankFmt)) selectRank(rank - 1);
        const Cand& c = cands_[size_t(std::clamp(sel_, 0, n - 1))];
        ImGui::SameLine();
        int ck = c.cls;  // classification row of the unit shown (a unit picked from a scatter need not be on the list)
        bool meets = c.meets, low = c.low;
        if (extraUnit_ >= 0 && cls_) {
            ck = -1;
            for (size_t i = 0; i < cls_->units.size(); ++i)
                if (cls_->units[i] == extraUnit_) ck = int(i);
            meets = false;
            low = true;
        }
        if (p_.rankMode != 0 && cls_ && ck >= 0) {
            const size_t k = size_t(ck);
            char non[32] = "n/a";
            if (std::isfinite(cls_->nonSongRate[k])) std::snprintf(non, sizeof non, "%.4f Hz", cls_->nonSongRate[k]);
            ImGui::Text("unit %d   song %.2f Hz   non-song %s   width %.3f ms   sparseness %.3f   %s", cls_->units[k], cls_->songRate[k], non,
                        effWidth(k), cls_->sparseness[k],
                        extraUnit_ >= 0 ? "(not on the list)" : low ? "(few spikes)" : meets ? (p_.rankMode == 2 ? "sparse" : "meets all type-1 criteria") : "");
        } else if (p_.rankMode == 0 && ranking_ && sel_ < int(ranking_->rows.size())) {
            const MsnRankRow& r = ranking_->rows[size_t(sel_)];
            ImGui::Text("unit %d   variance %.0f   %.0f spikes   (arrow keys also page)", r.unit, r.variance, r.nSpikes);
        }
    } else if (p_.rankMode != 0 && cls_) {
        if (p_.rankMode == 2) ImGui::TextColored(kAmber, "No unit has %d or more spikes in the syllable windows.", p_.minSpikes);
        else ImGui::TextColored(kAmber, "No unit has %d or more spikes during song at these settings.", p_.minSongSpikes);
    }

    const float avail = ImGui::GetContentRegionAvail().y;
    if (!cands_.empty() && ImGui::CollapsingHeader("Ranking table")) drawRankTable(std::min(10 * em(), avail * 0.3f));
    if (p_.rankMode != 0 && cls_) drawScatters(std::clamp(ImGui::GetContentRegionAvail().y * 0.26f, 9 * em(), 16 * em()));
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
    const bool stale = curUnit() >= 0 && (f.unit != curUnit() || f.label != curLabel());

    char rankTxt[48];
    if (extraUnit_ >= 0) std::snprintf(rankTxt, sizeof rankTxt, "not on the list");
    else std::snprintf(rankTxt, sizeof rankTxt, "rank %d", sel_ + 1);
    char title[256];
    std::snprintf(title, sizeof title, "syllable %d (n=%d, median %.0f ms)  |  unit %d  |  %s  |  peak %.0f ms%s", f.label,
                  f.nRenditions, f.medianMs, f.unit, rankTxt,  f.peakMs, stale ? "   (loading...)" : "");
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

    if (ImPlot::BeginPlot("##msn_wf", ImVec2(-1, hw), ImPlotFlags_NoMenus)) {
        ImPlot::SetupLegend(ImPlotLocation_SouthEast);
        ImPlot::SetupAxes("ms from peak deflection", "normalized", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        auto peakNormalized = [](const std::vector<float>& v) {
            float m = 1e-12f;
            for (float x : v) m = std::max(m, std::fabs(x));
            std::vector<float> o(v.size());
            for (size_t i = 0; i < v.size(); ++i) o[i] = v[i] / m;
            return o;
        };
        if (!f.wf.empty() && f.wfT.size() == f.wf.size()) {
            const auto n = peakNormalized(f.wf);
            ImPlot::SetNextLineStyle(kRed, 1.5f);
            ImPlot::PlotLine("Kilosort template", f.wfT.data(), n.data(), int(n.size()));
        }
        if (!f.rawWf.empty() && f.rawT.size() == f.rawWf.size()) {
            const auto n = peakNormalized(f.rawWf);
            ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.9f, 0.9f, 1), 1.5f);
            ImPlot::PlotLine("raw mean", f.rawT.data(), n.data(), int(n.size()));
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

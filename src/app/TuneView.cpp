#include "app/TuneView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstring>
#include <filesystem>
#include <random>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <GL/gl.h>

#include "app/Widgets.h"
#include "core/Filters.h"
#include "core/WavFile.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "implot.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace fp {

using namespace ui;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
const ImVec4 kCyan(0.35f, 0.85f, 0.95f, 1.0f);
const ImVec4 kYellow(0.95f, 0.90f, 0.35f, 1.0f);

ImVec4 templateColor(int i) {
    static const ImVec4 c[] = {{1.00f, 0.55f, 0.20f, 1}, {0.35f, 0.85f, 0.95f, 1}, {0.95f, 0.45f, 0.75f, 1},
                               {0.70f, 0.95f, 0.40f, 1}, {0.95f, 0.90f, 0.35f, 1}, {0.65f, 0.55f, 1.00f, 1}};
    return c[size_t(std::max(0, i)) % (sizeof c / sizeof c[0])];
}

uint64_t specKeyOf(int window, double maxHz) { return (uint64_t(window) << 32) ^ uint64_t(maxHz * 16.0); }
// What the slow per-file tracks (detection signal, template features) depend on.
uint64_t tracksKey(const AppParams& p) { return envelopeKey(p) ^ (featureKey(p) * 0x9E3779B97F4A7C15ull); }

Spectrogram computeSpectrogram(const std::vector<float>& x, double fsr, int W, double fMax,
                               const std::atomic<bool>& cancel) {
    Spectrogram s;
    W = std::clamp(W, 64, 8192);
    if (x.size() < size_t(W) || fsr <= 0) return s;
    const size_t n = x.size();
    // At most ~8000 columns so the texture stays small; at least 1 ms resolution.
    const int hop = std::max({1, int(fsr * 0.001), int(std::ceil(double(n - W) / 8000.0))});
    const int cols = int((n - W) / hop) + 1;
    const int nfft = nextPow2(W);
    const double df = fsr / nfft;
    s.fMax = std::min(fMax, fsr / 2.0);
    const int rows = std::min(nfft / 2 + 1, int(s.fMax / df) + 1);
    s.fMax = (rows - 1) * df;
    s.cols = cols;
    s.rows = rows;
    s.t0 = (W / 2.0) / fsr;
    s.t1 = s.t0 + double(cols - 1) * hop / fsr;
    s.db.resize(size_t(rows) * cols);

    std::vector<double> win(W);
    for (int i = 0; i < W; ++i) win[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / W);
    std::vector<std::complex<double>> buf(nfft);
    for (int c = 0; c < cols; ++c) {
        if ((c & 255) == 0 && cancel.load()) return Spectrogram{};
        const float* p = &x[size_t(c) * hop];
        double mean = 0.0;
        for (int i = 0; i < W; ++i) mean += p[i];
        mean /= W;
        for (int i = 0; i < W; ++i) buf[i] = {(p[i] - mean) * win[i], 0.0};
        std::fill(buf.begin() + W, buf.end(), std::complex<double>(0.0, 0.0));
        fftInPlace(buf);
        for (int r = 0; r < rows; ++r) s.db[size_t(r) * cols + c] = float(10.0 * std::log10(std::norm(buf[r]) + 1e-20));
    }
    // Reference level: 99.5th percentile, so one click doesn't set the colour scale.
    std::vector<float> tmp(s.db);
    const size_t k = size_t(tmp.size() * 0.995);
    std::nth_element(tmp.begin(), tmp.begin() + k, tmp.end());
    s.dbMax = tmp[k];
    return s;
}

bool wildcardMatch(const char* pat, const char* str) {
    while (*pat) {
        if (*pat == '*') {
            ++pat;
            if (!*pat) return true;
            for (; *str; ++str)
                if (wildcardMatch(pat, str)) return true;
            return false;
        }
        if (!*str) return false;
        if (*pat != '?' && std::tolower((unsigned char)*pat) != std::tolower((unsigned char)*str)) return false;
        ++pat;
        ++str;
    }
    return *str == 0;
}

bool matchesAny(const std::string& patterns, const std::string& name) {
    size_t a = 0;
    while (a <= patterns.size()) {
        size_t b = patterns.find(';', a);
        if (b == std::string::npos) b = patterns.size();
        std::string p = patterns.substr(a, b - a);
        p.erase(0, p.find_first_not_of(' '));
        if (!p.empty() && wildcardMatch(p.c_str(), name.c_str())) return true;
        a = b + 1;
    }
    return false;
}

std::string fileName(const std::string& path) { return fs::u8path(path).filename().u8string(); }

// The three values a syllable definition is checked against at syllable s's onset:
// the IOI ending there, the previous syllable's duration, and the IOI before that.
struct Compared {
    double ioi, sd, prevIoi;
};
Compared comparedValues(const SyllableInfo& s) { return {s.ioiMs, s.prevSdMs, s.prevIoiMs}; }

// Moves a window so it is centred on v, keeping its width.
void centreWindow(TimeWindow& w, RangeMode m, double v) {
    v = std::round(v);
    if (m == RangeMode::SetpointEps) {
        w.setpointMs = v;
    } else {
        const double half = 0.5 * (w.maxMs - w.minMs);
        w.minMs = v - half;
        w.maxMs = v + half;
    }
}

}  // namespace

// ------------------------------------------------------------------ FileAnalyzer

FileAnalyzer::~FileAnalyzer() {
    {
        std::lock_guard<std::mutex> lk(mx_);
        stop_ = true;
        cancel_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void FileAnalyzer::request(const std::string& path, const AppParams& p, int specWindow, double specMaxHz) {
    {
        std::lock_guard<std::mutex> lk(mx_);
        job_ = Job{path, p, specWindow, specMaxHz};
        hasJob_ = true;
        cancel_ = true;  // abandon whatever is running; the new job supersedes it
    }
    if (!th_.joinable()) th_ = std::thread(&FileAnalyzer::run, this);
    cv_.notify_all();
}

std::shared_ptr<const FileAnalysis> FileAnalyzer::result() const {
    std::lock_guard<std::mutex> lk(mx_);
    return result_;
}

void FileAnalyzer::publish(const std::shared_ptr<FileAnalysis>& a) {
    auto snapshot = std::make_shared<const FileAnalysis>(*a);  // the worker keeps editing *a
    std::lock_guard<std::mutex> lk(mx_);
    result_ = snapshot;
}

void FileAnalyzer::run() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait(lk, [&] { return hasJob_ || stop_; });
            if (stop_) return;
            job = std::move(job_);
            hasJob_ = false;
            cancel_ = false;
            busy_ = true;
            progress_ = 0.0f;
        }
        const auto prev = result();
        auto a = std::make_shared<FileAnalysis>();
        if (prev && prev->path == job.path && prev->error.empty() && prev->audio) {
            *a = *prev;
        } else {
            a->path = job.path;
            WavData w;
            std::string err;
            if (!readWav(job.path, w, &err)) {
                a->error = err;
                publish(a);
                busy_ = false;
                continue;
            }
            a->sampleRate = w.sampleRate;
            a->durationSec = w.samples.size() / w.sampleRate;
            a->audio = std::make_shared<const std::vector<float>>(std::move(w.samples));
            publish(a);  // lets the view switch files right away
        }
        const AppParams pr = paramsForRate(job.p, a->sampleRate);

        const uint64_t sk = specKeyOf(job.specWindow, job.specMaxHz);
        if (!a->spec || a->specKey != sk) {
            auto s = std::make_shared<Spectrogram>(
                computeSpectrogram(*a->audio, a->sampleRate, job.specWindow, job.specMaxHz, cancel_));
            if (cancel_) {
                busy_ = false;
                continue;
            }
            a->spec = s;
            a->specKey = sk;
            publish(a);
        }
        const uint64_t songK = songKey(job.p);
        if (!a->song || a->songKey != songK) {
            auto t = std::make_shared<SongTrack>();
            computeSongTrack(*a->audio, pr, *t);
            a->song = t;
            a->songKey = songK;
            publish(a);
        }
        const uint64_t featK = featureKey(job.p);
        if (a->featKey != featK || (!a->feat && job.p.tmpl.method == DetectMethod::Template)) {
            auto t = std::make_shared<FeatureTrack>();
            computeFeatureTrack(*a->audio, pr, *t);
            a->feat = t;
            a->featKey = featK;
            publish(a);
        }
        const uint64_t envK = envelopeKey(job.p);
        if (!a->env || a->envKey != envK) {
            auto e = std::make_shared<EnvelopeTrack>();
            if (computeEnvelopeTrack(*a->audio, pr, *e, &cancel_, &progress_)) {
                a->env = e;
                a->envKey = envK;
                publish(a);
            }
        }
        busy_ = false;
    }
}

// ------------------------------------------------------------------ BatchAnalyzer

BatchAnalyzer::~BatchAnalyzer() { cancel(); }

void BatchAnalyzer::cancel() {
    cancel_ = true;
    if (th_.joinable()) th_.join();
    running_ = false;
}

void BatchAnalyzer::clear() {
    cancel();
    std::lock_guard<std::mutex> lk(mx_);
    entries_.clear();
    ++version_;
}

void BatchAnalyzer::removePaths(const std::set<std::string>& paths) {
    std::lock_guard<std::mutex> lk(mx_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const BatchEntry& e) { return paths.count(e.path) > 0; }),
                   entries_.end());
    ++version_;
}

void BatchAnalyzer::start(const std::vector<std::string>& files, const AppParams& p) {
    cancel();
    {
        std::lock_guard<std::mutex> lk(mx_);
        entries_.clear();
        envKey_ = tracksKey(p);
        songKey_ = songKey(p);
        ++version_;
    }
    cancel_ = false;
    running_ = true;
    progress_ = 0.0f;
    th_ = std::thread(&BatchAnalyzer::run, this, files, p);
}

void BatchAnalyzer::run(std::vector<std::string> files, AppParams p) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (size_t i = 0; i < files.size() && !cancel_; ++i) {
        BatchEntry e;
        e.path = files[i];
        WavData w;
        if (!readWav(files[i], w, &e.error)) {
            std::lock_guard<std::mutex> lk(mx_);
            entries_.push_back(std::move(e));
            ++version_;
            continue;
        }
        e.durationSec = w.samples.size() / w.sampleRate;
        const AppParams pr = paramsForRate(p, w.sampleRate);
        auto env = std::make_shared<EnvelopeTrack>();
        if (!computeEnvelopeTrack(w.samples, pr, *env, &cancel_, &fileProgress_)) break;
        auto song = std::make_shared<SongTrack>();
        computeSongTrack(w.samples, pr, *song);
        auto feat = std::make_shared<FeatureTrack>();
        computeFeatureTrack(w.samples, pr, *feat);
        e.feat = feat;
        e.env = env;
        e.song = song;
        {
            std::lock_guard<std::mutex> lk(mx_);
            entries_.push_back(std::move(e));
            ++version_;
        }
        progress_ = float(i + 1) / float(files.size());
    }
    running_ = false;
}

void BatchAnalyzer::results(std::vector<BatchEntry>& out, uint64_t& envKey, uint64_t& songKey) const {
    std::lock_guard<std::mutex> lk(mx_);
    out = entries_;
    envKey = envKey_;
    songKey = songKey_;
}

size_t BatchAnalyzer::count() const {
    std::lock_guard<std::mutex> lk(mx_);
    return entries_.size();
}

// ------------------------------------------------------------------ TuneView

TuneView::TuneView() = default;

TuneView::~TuneView() {
    if (tex_) glDeleteTextures(1, &tex_);
}

void TuneView::scanFolder() {
    files_.clear();
    selected_ = -1;
    scanError_.clear();
    reqPath_.clear();
    // Results from a previous folder would otherwise linger in the summary.
    batch_.clear();
    batchSummary_.clear();
    batchSyllables_.clear();
    batchDecCount_ = 0;
    std::error_code ec;
    const fs::path root = fs::u8path(folder_);
    if (!fs::is_directory(root, ec)) {
        scanError_ = "Not a folder: " + folder_;
        return;
    }
    // 'rejected' folders hold files moved out by the clean-up: never list their contents,
    // but remember them so they can be emptied.
    rejectDirs_.clear();
    auto isRejected = [](const fs::directory_entry& d) {
        std::error_code e2;
        return d.is_directory(e2) && d.path().filename() == "rejected";
    };
    auto consider = [&](const fs::directory_entry& d) {
        std::error_code e2;
        if (d.is_regular_file(e2) && matchesAny(pattern_, d.path().filename().u8string()))
            files_.push_back(d.path().u8string());
    };
    if (recursive_) {
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (isRejected(*it)) {
                rejectDirs_.push_back(it->path().u8string());
                it.disable_recursion_pending();
                continue;
            }
            consider(*it);
        }
    } else {
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (isRejected(*it))
                rejectDirs_.push_back(it->path().u8string());
            else
                consider(*it);
        }
    }
    if (ec) scanError_ = ec.message();
    std::sort(files_.begin(), files_.end());
    if (!files_.empty()) select(0);
}

void TuneView::openFolder(const std::string& folder, bool analyze, const AppParams& p, bool showSummary) {
    folder_ = folder;
    scanFolder();
    if (analyze && !files_.empty()) batch_.start(files_, p);
    openSummaryTab_ = showSummary;
}

void TuneView::select(int index) {
    if (files_.empty()) return;
    selected_ = std::clamp(index, 0, int(files_.size()) - 1);
}

void TuneView::updateRequests(const AppParams& p) {
    if (selected_ < 0 || selected_ >= int(files_.size())) return;
    const std::string& path = files_[selected_];
    const uint64_t ek = envelopeKey(p), sk = songKey(p), xk = specKeyOf(specWindow_, specMaxHz_), fk = featureKey(p);
    const bool newFile = path != reqPath_;
    const bool changedKeys = ek != reqEnvKey_ || sk != reqSongKey_ || xk != reqSpecKey_ || fk != reqFeatKey_;
    const double now = ImGui::GetTime();
    // While someone drags a band edge, restart the analysis at most ~8 times a second.
    if (newFile || (changedKeys && now - lastRequestTime_ > 0.12)) {
        analyzer_.request(path, p, specWindow_, specMaxHz_);
        reqPath_ = path;
        reqEnvKey_ = ek;
        reqSongKey_ = sk;
        reqSpecKey_ = xk;
        reqFeatKey_ = fk;
        lastRequestTime_ = now;
    }
}

void TuneView::updateDecisions(const AppParams& p, const FileAnalysis& a) {
    if (!a.env || !a.song) {
        dec_ = DecisionTrack{};
        decEnv_.reset();
        decSong_.reset();
        return;
    }
    const uint64_t k = decisionKey(p);
    if (k == decKey_ && a.env == decEnv_ && a.song == decSong_ && a.feat == decFeat_) return;
    runDecisions(*a.env, *a.song, a.feat.get(), paramsForRate(p, a.sampleRate), dec_);
    decKey_ = k;
    decEnv_ = a.env;
    decSong_ = a.song;
    decFeat_ = a.feat;
}

void TuneView::updateBatchDecisions(const AppParams& p) {
    const uint64_t k = decisionKey(p);
    if (k == batchDecKey_ && batchEntries_.size() == batchDecCount_) return;
    // Re-running every file can take a moment for big folders; don't do it every frame while dragging.
    const double now = ImGui::GetTime();
    if (batchDecTime_ >= 0 && now - batchDecTime_ < 0.2 && batchEntries_.size() == batchDecCount_) return;
    batchSummary_.assign(batchEntries_.size(), FileSummary{});
    batchSyllables_.clear();
    DecisionTrack dt;
    for (size_t i = 0; i < batchEntries_.size(); ++i) {
        const auto& e = batchEntries_[i];
        if (!e.env || !e.song) continue;
        runDecisions(*e.env, *e.song, e.feat.get(), paramsForRate(p, e.env->sampleRate), dt);
        auto& s = batchSummary_[i];
        s.syllables = dt.syllables.size();
        s.matches = dt.matchMs.size();
        s.blocked = dt.blockedMs.size();
        s.fires = dt.fire1Ms.size() + dt.fire2Ms.size();
        s.perCode.assign(p.codes.list.size(), 0);
        for (const auto& syl : dt.syllables)
            if (syl.codeIndex >= 0 && syl.codeIndex < int(s.perCode.size())) ++s.perCode[size_t(syl.codeIndex)];
        batchSyllables_.insert(batchSyllables_.end(), dt.syllables.begin(), dt.syllables.end());
    }
    batchDecKey_ = k;
    batchDecCount_ = batchEntries_.size();
    batchDecTime_ = now;
}

void TuneView::rebuildTexture(const std::shared_ptr<const Spectrogram>& sp) {
    texSpec_ = sp;  // remember it even if empty, so an empty result isn't retried every frame
    texRange_ = specRange_;
    texOffset_ = specOffset_;
    const Spectrogram& s = *sp;
    if (s.cols == 0 || s.rows == 0) return;
    unsigned char lut[256][4];
    for (int i = 0; i < 256; ++i) {
        const float t = i / 255.0f;
        const ImVec4 c = ImPlot::SampleColormap(t, ImPlotColormap_Jet);
        const float fade = std::min(1.0f, t * 3.0f);  // fade the low end to black, like LabVIEW
        lut[i][0] = (unsigned char)(255 * c.x * fade);
        lut[i][1] = (unsigned char)(255 * c.y * fade);
        lut[i][2] = (unsigned char)(255 * c.z * fade);
        lut[i][3] = 255;
    }
    const float top = s.dbMax + specOffset_;
    const float bottom = top - specRange_;
    std::vector<unsigned char> rgba(size_t(s.cols) * s.rows * 4);
    for (size_t i = 0; i < s.db.size(); ++i) {
        const float t = std::clamp((s.db[i] - bottom) / (top - bottom), 0.0f, 1.0f);
        std::memcpy(&rgba[i * 4], lut[int(t * 255.0f)], 4);
    }
    if (!tex_) glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s.cols, s.rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

int TuneView::syllableAt(double tMs) const {
    const auto& syl = dec_.syllables;
    // The syllable under the cursor, else the one with the nearest onset.
    int best = -1;
    for (int i = 0; i < int(syl.size()); ++i)
        if (tMs >= syl[i].onsetMs && (std::isnan(syl[i].offsetMs) || tMs <= syl[i].offsetMs)) best = i;
    if (best < 0) {
        double bd = 1e300;
        for (int i = 0; i < int(syl.size()); ++i)
            if (std::fabs(syl[i].onsetMs - tMs) < bd) bd = std::fabs(syl[i].onsetMs - tMs), best = i;
    }
    return best;
}

void TuneView::addCodeFromSyllable(AppParams& p, double tMs) {
    const int i = syllableAt(tMs);
    if (i < 0) return;
    const auto& s = dec_.syllables[i];
    if (std::isnan(s.ioiMs)) {
        status_ = "That is the first syllable of the file; it has no IOI to classify on.";
        return;
    }
    if (int(p.codes.list.size()) >= kMaxCodes) return;
    // Same definition the feedback target would get: "IOI, SD, IOI" when the history
    // is long enough, otherwise the longest shorter type. Windows default to +/-10 ms.
    SyllableCode c;
    int code = 1;
    for (const auto& x : p.codes.list) code = std::max(code, x.code + 1);
    c.code = std::min(code, 255);
    c.name = "syl" + std::to_string(p.codes.list.size() + 1);
    c.type = !std::isnan(s.prevIoiMs) && !std::isnan(s.prevSdMs) ? TriggerType::IOI_SD_IOI
             : !std::isnan(s.prevSdMs)                            ? TriggerType::SD_IOI
                                                                  : TriggerType::IOI;
    auto win = [](double v) {
        TimeWindow w{std::isnan(v) ? 100.0 : std::round(v), 10.0, 0, 0};
        w.convertTo(RangeMode::MinMax);
        return w;
    };
    c.ioi = win(s.ioiMs);
    c.sd = win(s.prevSdMs);
    c.prevIoi = win(s.prevIoiMs);
    p.codes.list.push_back(c);
    *edit_ = int(p.codes.list.size()) - 1;  // edit the new syllable
    changed() = true;
    char buf[200];
    std::snprintf(buf, sizeof buf, "Added '%s' (code %d) from the syllable at %.3f s; it is now the syllable being "
                  "edited.", c.name.c_str(), c.code, s.onsetMs / 1000.0);
    status_ = buf;
}

void TuneView::setTemplateFromClick(AppParams& p, double tMs, bool add) {
    const FileAnalysis* a = shown_.get();
    if (!a || !a->audio) return;
    std::vector<float> shape;
    double level = 0.0;
    if (!templateShapeAt(*a->audio, paramsForRate(p, a->sampleRate), tMs, shape, &level)) {
        status_ = "Too close to the start or end of the file.";
        return;
    }
    auto& list = p.tmpl.list;
    int i = *editTmpl_;
    if (i < 0 || i >= int(list.size())) {
        if (list.size() >= size_t(kMaxTemplates)) {
            status_ = "Too many templates.";
            return;
        }
        SpectralTemplate t;
        int code = 1;
        for (const auto& o : list) code = std::max(code, o.code + 1);
        t.code = std::min(code, 255);
        t.name = "T" + std::to_string(list.size() + 1);
        list.push_back(t);
        i = int(list.size()) - 1;
        *editTmpl_ = i;
        if (p.tmpl.target < 0) p.tmpl.target = i;
        add = false;
    }
    SpectralTemplate& t = list[size_t(i)];
    if (!add) t.examples.clear();
    t.examples.push_back({a->path, tMs});
    std::string err;
    if (t.examples.size() == 1)
        t.shape = shape;
    else if (!rebuildTemplateShape(t, p, &err))
        t.shape = shape;
    char buf[200];
    std::snprintf(buf, sizeof buf, "%s: %s example at %.3f s (level %.1f dB), %zu example(s)", t.name.c_str(),
                  add ? "added" : "set to", tMs / 1000.0, level, t.examples.size());
    status_ = err.empty() ? buf : err;
    changed() = true;
}

void TuneView::setEditedFromSyllable(AppParams& p, double tMs) {
    const int best = syllableAt(tMs);
    if (best < 0) return;
    if (*edit_ < 0 || *edit_ >= int(p.codes.list.size())) {
        status_ = "No syllable selected for editing; Ctrl+Shift+click adds a new one.";
        return;
    }
    SyllableCode& e = p.codes.list[size_t(*edit_)];
    const auto c = comparedValues(dec_.syllables[best]);
    if ((usesIoi(e.type) && std::isnan(c.ioi)) || (usesSd(e.type) && std::isnan(c.sd)) ||
        (usesPrevIoi(e.type) && std::isnan(c.prevIoi))) {
        status_ = "That syllable is too early in the bout to have all the intervals '" + e.name + "' uses.";
        return;
    }
    // Centre the windows on this syllable's values, keeping their widths.
    if (usesIoi(e.type)) centreWindow(e.ioi, e.mode, c.ioi);
    if (usesSd(e.type)) centreWindow(e.sd, e.mode, c.sd);
    if (usesPrevIoi(e.type)) centreWindow(e.prevIoi, e.mode, c.prevIoi);
    changed() = true;
    char buf[220];
    std::snprintf(buf, sizeof buf, "'%s' centred on the syllable at %.3f s: IOI %.0f, SD %.0f, prev IOI %.0f ms",
                  e.name.c_str(), tMs / 1000.0, c.ioi, c.sd, c.prevIoi);
    status_ = buf;
}

void TuneView::draw(AppParams& p, int& editSyllable, int& editTemplate) {
    edit_ = &editSyllable;
    editTmpl_ = &editTemplate;
    // Template feature settings changed: rebuild the shapes from their examples (once per change).
    if (p.tmpl.method == DetectMethod::Template) {
        const int bands = templateBands(p.tmpl);
        const uint64_t fk = featureKey(p);
        if (fk != rebuiltFeatKey_) {
            rebuiltFeatKey_ = fk;
            for (auto& t : p.tmpl.list)
                if (!t.examples.empty() && int(t.shape.size()) != bands) {
                    std::string err;
                    if (rebuildTemplateShape(t, p, &err)) changed() = true;
                    else status_ = err;
                }
        }
    }
    drawBrowser(p);
    updateRequests(p);

    // Results from the background analyses.
    if (auto r = analyzer_.result(); r && selected_ >= 0 && selected_ < int(files_.size()) && r->path == files_[selected_])
        shown_ = r;
    if (batch_.version() != batchVersion_) {  // copy only when the batch results changed
        batchVersion_ = batch_.version();
        batch_.results(batchEntries_, batchEnvKey_, batchSongKey_);
    }
    if (!batchEntries_.empty()) updateBatchDecisions(p);  // keeps the per-file counts in the list current
    updatePass();  // red = no song (clean-up in the Folder summary)

    ImGui::BeginChild("filelist", ImVec2(22.0f * em(), 0), ImGuiChildFlags_Border);
    if (files_.empty()) ImGui::TextDisabled(folder_.empty() ? "Choose a folder above." : "No matching files.");
    // Per-file counts appear once the folder has been analyzed.
    std::vector<int> summaryIndex(files_.size(), -1);
    if (!batchSummary_.empty()) {
        for (size_t i = 0, j = 0; i < files_.size() && j < batchEntries_.size(); ++i) {
            while (j < batchEntries_.size() && batchEntries_[j].path < files_[i]) ++j;
            if (j < batchEntries_.size() && batchEntries_[j].path == files_[i] && j < batchSummary_.size())
                summaryIndex[i] = int(j);
        }
    }
    ImGuiListClipper clip;
    clip.Begin(int(files_.size()));
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            ImGui::PushID(i);
            std::string label = fileName(files_[i]);
            if (summaryIndex[i] >= 0) {
                const auto& s = batchSummary_[summaryIndex[i]];
                label += "  [" + std::to_string(s.matches) + " match, " + std::to_string(s.fires) + " fb]";
            }
            const bool fails = summaryIndex[i] >= 0 && size_t(summaryIndex[i]) < pass_.size() && pass_[size_t(summaryIndex[i])] == 0;
            if (fails) ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            if (ImGui::Selectable(label.c_str(), i == selected_)) select(i);
            if (fails) ImGui::PopStyleColor();
            if (i == selected_ && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("tunemain");
    if (ImGui::BeginTabBar("tunetabs")) {
        if (ImGui::BeginTabItem("File", nullptr, openFileTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawFileView(p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Folder summary", nullptr, openSummaryTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawFolderSummary(p);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    openFileTab_ = openSummaryTab_ = false;
    ImGui::EndChild();
}

void TuneView::drawBrowser(AppParams& p) {
    (void)p;
    ImGui::SetNextItemWidth(28.0f * em());
    const bool enter = ImGui::InputText("##folder", &folder_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        const std::string f = pickFolderDialog("Folder of WAV files to tune on");
        if (!f.empty()) {
            folder_ = f;
            scanFolder();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(6.0f * em());
    ImGui::InputText("pattern", &pattern_);
    ImGui::SameLine();
    ImGui::Checkbox("subfolders", &recursive_);
    ImGui::SameLine();
    if (ImGui::Button("Scan") || enter) scanFolder();
    ImGui::SameLine();
    ImGui::Text("%zu files", files_.size());

    ImGui::SameLine();
    ImGui::BeginDisabled(files_.empty());
    if (ImGui::Button("<< Prev")) select(selected_ - 1);
    ImGui::SameLine();
    if (ImGui::Button("Next >>")) select(selected_ + 1);
    ImGui::SameLine();
    if (ImGui::Button("Random")) {
        static std::mt19937 rng(std::random_device{}());
        select(int(rng() % files_.size()));
    }
    ImGui::EndDisabled();
    if (!files_.empty() && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) select(selected_ + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) select(selected_ - 1);
    }
    if (!scanError_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kRed, "%s", scanError_.c_str());
    }
}

void TuneView::drawFileView(AppParams& p) {
    const FileAnalysis* a = shown_.get();
    const bool current = a && selected_ >= 0 && selected_ < int(files_.size()) && a->path == files_[selected_];
    if (!current) {
        ImGui::TextDisabled(selected_ < 0 ? "Pick a file on the left." : "Loading...");
        return;
    }
    if (!a->error.empty()) {
        ImGui::TextColored(kRed, "%s", a->error.c_str());
        return;
    }
    updateDecisions(p, *a);

    // ---- info + display options
    const bool stale = analyzer_.busy();
    ImGui::Text("%s  %.2f s @ %.0f Hz  |  %zu syllables, %zu matches, %zu blocked (no song), %zu feedback",
                fileName(a->path).c_str(), a->durationSec, a->sampleRate, dec_.syllables.size(), dec_.matchMs.size(),
                dec_.blockedMs.size(), dec_.fire1Ms.size() + dec_.fire2Ms.size());
    if (stale) {
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "  updating %.0f%%", analyzer_.progress() * 100.0f);
    }
    if (ImGui::Button("Zoom full")) {
        tMin_ = 0.0;
        tMax_ = a->durationSec;
        fitY_ = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Bands", &showBands_);
    ImGui::SameLine();
    ImGui::Checkbox("Song bands", &showSongBands_);
    ImGui::SameLine();
    ImGui::Checkbox("Durations", &showSD_);
    ImGui::SameLine();
    ImGui::Checkbox("IOIs", &showIOI_);
    ImGui::SameLine();
    ImGui::Checkbox("Matches", &showMatches_);
    ImGui::SameLine();
    ImGui::Checkbox("Feedback", &showFeedback_);
    ImGui::SameLine();
    ImGui::Checkbox("Codes", &showCodes_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(5.0f * em());
    if (ImGui::BeginCombo("window", std::to_string(specWindow_).c_str())) {
        for (int w : {256, 512, 1024, 2048})
            if (ImGui::Selectable(std::to_string(w).c_str(), w == specWindow_)) specWindow_ = w;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(6.0f * em());
    ImGui::SliderFloat("range dB", &specRange_, 20.0f, 120.0f, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(6.0f * em());
    ImGui::SliderFloat("level", &specOffset_, -40.0f, 40.0f, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(5.0f * em());
    ImGui::InputDouble("max Hz", &specMaxHz_, 0, 0, "%.0f");
    specMaxHz_ = std::clamp(specMaxHz_, 1000.0, 50000.0);
    const bool tmplMode = p.tmpl.method == DetectMethod::Template;
    if (tmplMode)
        ImGui::TextDisabled("Templates: Ctrl+click = set the edited template to the sound just before the click (new template "
                            "if none). Ctrl+Shift+click = add another example to it. Drag its threshold (dashed) on the "
                            "detection plot. PageUp/PageDown change file.");
    else
        ImGui::TextDisabled("Drag the green/red lines to change thresholds and band edges. Ctrl+click a syllable: centre "
                            "the edited syllable on it. Ctrl+Shift+click: add a new syllable. PageUp/PageDown change file.");
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kPurple, "%s", status_.c_str());
    }

    if (fitPath_ != a->path) {
        fitPath_ = a->path;
        tMin_ = 0.0;
        tMax_ = a->durationSec;
        fitY_ = true;
        status_.clear();
    }

    const float h = ImGui::GetContentRegionAvail().y - 2 * ImGui::GetStyle().ItemSpacing.y;
    const float specH = h * 0.36f, detH = h * 0.40f, songH = h * 0.24f;

    auto markers = [&] {
        auto lines = [](const char* name, const std::vector<double>& ms, const ImVec4& c, float w) {
            if (ms.empty()) return;
            std::vector<double> s(ms.size());
            for (size_t i = 0; i < ms.size(); ++i) s[i] = ms[i] / 1000.0;
            ImPlot::SetNextLineStyle(c, w);
            ImPlot::PlotInfLines(name, s.data(), int(s.size()));
        };
        if (showMatches_) {
            lines("match", dec_.matchMs, kPurple, 1.0f);
            lines("blocked (no song)", dec_.blockedMs, kGrey, 1.0f);
        }
        if (showFeedback_) {
            lines("feedback 1", dec_.fire1Ms, kRed, 2.0f);
            lines("feedback 2", dec_.fire2Ms, kAmber, 2.0f);
            lines("suppressed", dec_.suppressedMs, ImVec4(0.5f, 0.5f, 0.5f, 0.7f), 1.0f);
        }
    };

    // ---- spectrogram with draggable band edges
    if (a->spec && (texSpec_ != a->spec || texRange_ != specRange_ || texOffset_ != specOffset_))
        rebuildTexture(a->spec);
    // Aligned so the three time axes line up even though they have different axis labels.
    ImPlot::BeginAlignedPlots("tunefile");
    if (ImPlot::BeginPlot("##spectrogram", ImVec2(-1, specH), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, "Hz", 0, ImPlotAxisFlags_Lock);
        // An empty right axis keeps the plot area the same width as the song plot below.
        ImPlot::SetupAxis(ImAxis_Y2, nullptr, ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLinks(ImAxis_X1, &tMin_, &tMax_);
        const double fTop = a->spec ? a->spec->fMax : specMaxHz_;
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, fTop, ImPlotCond_Always);
        if (a->spec && tex_ && a->spec->cols > 0) {
            const auto& s = *a->spec;
            const double halfHop = s.cols > 1 ? 0.5 * (s.t1 - s.t0) / (s.cols - 1) : 0.0;
            // Texture row 0 is the lowest frequency; ImPlot puts uv0 at the top-left, so flip V.
            ImPlot::PlotImage("spec", (ImTextureID)(intptr_t)tex_, ImPlotPoint(s.t0 - halfHop, 0),
                              ImPlotPoint(s.t1 + halfHop, s.fMax), ImVec2(0, 1), ImVec2(1, 0));
        }
        int id = 0;
        auto edge = [&](double* hz, const ImVec4& c) {
            if (ImPlot::DragLineY(id++, hz, c, 1.5f)) {
                *hz = std::clamp(*hz, 0.0, fTop);
                changed() = true;
            }
        };
        if (showBands_) {
            for (auto& b : p.spectrum.include)
                if (b.gain != 0.0) {
                    edge(&b.lowHz, kGreen);
                    edge(&b.highHz, kGreen);
                }
            for (auto& b : p.spectrum.subtract)
                if (b.gain != 0.0) {
                    edge(&b.lowHz, kRed);
                    edge(&b.highHz, kRed);
                }
        }
        if (showSongBands_ && p.song.enabled) {
            edge(&p.song.birdLowHz, kBlue);
            edge(&p.song.birdHighHz, kBlue);
            edge(&p.song.nonBirdCutoffHz, kCyan);
        }
        markers();
        if (tmplMode) {
            // The edited template's examples in this file: the analysis window of each.
            if (*editTmpl_ >= 0 && *editTmpl_ < int(p.tmpl.list.size())) {
                ImPlot::PushPlotClipRect();
                const ImPlotRect lim = ImPlot::GetPlotLimits();
                ImVec4 c = templateColor(*editTmpl_);
                for (const auto& ex : p.tmpl.list[size_t(*editTmpl_)].examples) {
                    if (ex.file != a->path) continue;
                    const ImVec2 p0 = ImPlot::PlotToPixels((ex.ms - p.tmpl.windowMs) / 1000.0, lim.Y.Max);
                    const ImVec2 p1 = ImPlot::PlotToPixels(ex.ms / 1000.0, lim.Y.Min);
                    ImPlot::GetPlotDrawList()->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.25f)));
                    ImPlot::GetPlotDrawList()->AddRect(p0, p1, ImGui::GetColorU32(c));
                }
                ImPlot::PopPlotClipRect();
            }
            if (ImPlot::IsPlotHovered()) {
                ImGui::SetTooltip("Ctrl+click: set the edited template to the %.0f ms before the cursor (new template "
                                  "if none)\nCtrl+Shift+click: add it as another example",
                                  p.tmpl.windowMs);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyCtrl)
                    setTemplateFromClick(p, ImPlot::GetPlotMousePos().x * 1000.0, ImGui::GetIO().KeyShift);
            }
        }
        ImPlot::EndPlot();
    }

    // ---- detection signal (log scale) with draggable thresholds
    const EnvelopeTrack* env = a->env.get();
    if (ImPlot::BeginPlot("##detection", ImVec2(-1, detH), ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, "detection", 0, 0);
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxis(ImAxis_Y2, nullptr,
                          ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks |
                              ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, 1.0, ImPlotCond_Always);
        ImPlot::SetupAxisLinks(ImAxis_X1, &tMin_, &tMax_);
        ImPlot::SetupLegend(ImPlotLocation_SouthEast, ImPlotLegendFlags_Horizontal);
        if (fitY_ && env && !env->value.empty()) {
            const float mx = *std::max_element(env->value.begin(), env->value.end());
            // Like LabVIEW's log plot: a few decades below the loudest syllable, and always
            // low enough to show the end threshold.
            const double hi = std::max(1e-6, double(mx) * 5.0);
            const double lo = std::max(hi * 1e-8, std::min(hi * 1e-4, p.spectrum.sylEndThresh * 0.3));
            ImPlot::SetupAxisLimits(ImAxis_Y1, lo, hi, ImPlotCond_Always);
            fitY_ = false;
        }
        if (env && !env->value.empty()) {
            const size_t m = env->value.size();
            const double dt = env->frameMs() / 1000.0;
            scratchX_.resize(m);
            scratchY_.resize(m);
            std::vector<double> syl(m);
            for (size_t i = 0; i < m; ++i) {
                scratchX_[i] = (i + 1) * dt;
                scratchY_[i] = std::max(double(env->value[i]), 1e-9);  // log axis
                syl[i] = i < dec_.inSyllable.size() ? dec_.inSyllable[i] : 0.0;
            }
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::SetNextFillStyle(kGreen, 0.15f);
            ImPlot::PlotShaded("syllable", scratchX_.data(), syl.data(), int(m), 0.0);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::SetNextLineStyle(ImVec4(0.95f, 0.95f, 0.95f, 1.0f), 1.2f);
            ImPlot::PlotLine("signal", scratchX_.data(), scratchY_.data(), int(m));
        }
        if (ImPlot::DragLineY(100, &p.spectrum.sylStartThresh, kGreen, 2.0f)) {
            p.spectrum.sylStartThresh = std::max(p.spectrum.sylStartThresh, 1e-12);
            changed() = true;
        }
        if (ImPlot::DragLineY(101, &p.spectrum.sylEndThresh, kRed, 2.0f)) {
            p.spectrum.sylEndThresh = std::max(p.spectrum.sylEndThresh, 1e-12);
            changed() = true;
        }
        markers();
        // Template method: correlation with each template on the right axis (0..1), the
        // edited one bold with its draggable threshold, and where each was detected
        // (filled = counted, hollow = failed its timing gate).
        if (tmplMode && env && !dec_.tmplScores.empty()) {
            const double dt = env->frameMs() / 1000.0;
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            for (size_t i = 0; i < dec_.tmplScores.size() && i < p.tmpl.list.size(); ++i) {
                const auto& sc = dec_.tmplScores[i];
                if (sc.empty()) continue;
                const bool ed = int(i) == *editTmpl_;
                ImVec4 c = templateColor(int(i));
                c.w = ed ? 1.0f : 0.45f;
                ImPlot::SetNextLineStyle(c, ed ? 1.8f : 1.0f);
                const std::string label = p.tmpl.list[i].name + "##tscore" + std::to_string(i);
                ImPlot::PlotLine(label.c_str(), sc.data(), int(sc.size()), dt, dt);
            }
            for (size_t i = 0; i < p.tmpl.list.size(); ++i) {
                std::vector<double> xg, yg, xu, yu;
                for (const auto& h : dec_.tmplHits)
                    if (h.index == int(i)) (h.gated ? xg : xu).push_back(h.ms / 1000.0), (h.gated ? yg : yu).push_back(0.97);
                const ImVec4 c = templateColor(int(i));
                const std::string id = "##thit" + std::to_string(i);
                if (!xg.empty()) {
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 5, c, 1, c);
                    ImPlot::PlotScatter(("##g" + id).c_str(), xg.data(), yg.data(), int(xg.size()));
                }
                if (!xu.empty()) {
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 5, ImVec4(0, 0, 0, 0), 1.5f, c);
                    ImPlot::PlotScatter(("##u" + id).c_str(), xu.data(), yu.data(), int(xu.size()));
                }
            }
            if (*editTmpl_ >= 0 && *editTmpl_ < int(p.tmpl.list.size())) {
                SpectralTemplate& t = p.tmpl.list[size_t(*editTmpl_)];
                if (ImPlot::DragLineY(102, &t.thresh, templateColor(*editTmpl_), 1.5f)) {
                    t.thresh = std::clamp(t.thresh, 0.0, 1.0);
                    changed() = true;
                }
            }
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        }


        // Durations (yellow, centred on each syllable) and IOIs (cyan, at each onset), like LabVIEW.
        const ImPlotRect lim = ImPlot::GetPlotLimits();
        size_t visible = 0;
        for (const auto& s : dec_.syllables) visible += s.onsetMs / 1000.0 >= lim.X.Min && s.onsetMs / 1000.0 <= lim.X.Max;
        if (visible < 400) {
            for (const auto& s : dec_.syllables) {
                const double t = s.onsetMs / 1000.0;
                if (t < lim.X.Min || t > lim.X.Max) continue;
                if (showSD_ && !std::isnan(s.sdMs))
                    ImPlot::Annotation(0.5 * (s.onsetMs + s.offsetMs) / 1000.0, lim.Y.Max, kYellow, ImVec2(0, 10), true,
                                       "%.0f", s.sdMs);
                if (showIOI_ && !std::isnan(s.ioiMs))
                    ImPlot::Annotation(t, lim.Y.Max, s.matched ? kPurple : kCyan, ImVec2(0, 28), true, "%.0f",
                                       s.ioiMs);
                if (!tmplMode && showCodes_ && s.codeIndex >= 0 && s.codeIndex < int(p.codes.list.size()))
                    ImPlot::Annotation(t, lim.Y.Max, s.codeIndex == *edit_ ? kAmber : kGreen, ImVec2(0, 46), true,
                                       "%s", p.codes.list[s.codeIndex].name.c_str());
            }
        }
        if (tmplMode && showCodes_)
            for (const auto& h : dec_.tmplHits) {
                const double t = h.ms / 1000.0;
                if (!h.gated || t < lim.X.Min || t > lim.X.Max || h.index >= int(p.tmpl.list.size())) continue;
                ImPlot::Annotation(t, lim.Y.Max, templateColor(h.index), ImVec2(0, 46), true, "%s",
                                   p.tmpl.list[size_t(h.index)].name.c_str());
            }

        // Hover info and Ctrl+click target picking.
        if (ImPlot::IsPlotHovered() && tmplMode) {
            const double tMs = ImPlot::GetPlotMousePos().x * 1000.0;
            const long k = env ? std::lround(tMs / env->frameMs()) - 1 : -1;
            std::string info;
            char buf[160];
            if (a->feat && k >= 0 && size_t(k) < a->feat->frames() && a->feat->levelDb[size_t(k)] > -1e8f) {
                std::snprintf(buf, sizeof buf, "level %.1f dB (min %.0f)\n", a->feat->levelDb[size_t(k)],
                              p.tmpl.minLevelDb);
                info += buf;
            }
            for (size_t i = 0; i < dec_.tmplScores.size() && i < p.tmpl.list.size(); ++i)
                if (k >= 0 && size_t(k) < dec_.tmplScores[i].size()) {
                    std::snprintf(buf, sizeof buf, "%s: %.2f (thresh %.2f)\n", p.tmpl.list[i].name.c_str(),
                                  dec_.tmplScores[i][size_t(k)], p.tmpl.list[i].thresh);
                    info += buf;
                }
            const bool hasEdit = *editTmpl_ >= 0 && *editTmpl_ < int(p.tmpl.list.size());
            info += hasEdit ? "Ctrl+click: make '" + p.tmpl.list[size_t(*editTmpl_)].name +
                                  "' this sound (the window just before the cursor)\nCtrl+Shift+click: add as another example"
                            : std::string("Ctrl+click: new template from this sound (the window just before the cursor)");
            ImGui::SetTooltip("%s", info.c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyCtrl)
                setTemplateFromClick(p, tMs, ImGui::GetIO().KeyShift);
        } else if (ImPlot::IsPlotHovered()) {
            const double tMs = ImPlot::GetPlotMousePos().x * 1000.0;
            for (const auto& s : dec_.syllables)
                if (tMs >= s.onsetMs && (std::isnan(s.offsetMs) || tMs <= s.offsetMs)) {
                    const std::string code =
                        s.codeIndex >= 0 && s.codeIndex < int(p.codes.list.size())
                            ? "\ncode: " + p.codes.list[s.codeIndex].name + " (" + std::to_string(s.code) + ")"
                            : std::string();
                    const std::string editing = *edit_ >= 0 && *edit_ < int(p.codes.list.size())
                                                    ? p.codes.list[size_t(*edit_)].name
                                                    : std::string("(none)");
                    ImGui::SetTooltip("onset %.3f s\nSD %.0f ms\nIOI %.0f ms\nprev SD %.0f ms\nprev IOI %.0f ms%s%s\n"
                                      "Ctrl+click: centre '%s' on this syllable\n"
                                      "Ctrl+Shift+click: add it as a new syllable",
                                      s.onsetMs / 1000.0, s.sdMs, s.ioiMs, s.prevSdMs, s.prevIoiMs, code.c_str(),
                                      s.matched ? "\n= feedback target" : "", editing.c_str());
                    break;
                }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyCtrl) {
                if (ImGui::GetIO().KeyShift)
                    addCodeFromSyllable(p, tMs);
                else
                    setEditedFromSyllable(p, tMs);
            }
        }
        ImPlot::EndPlot();
    }

    // ---- song detector
    const SongTrack* song = a->song.get();
    if (ImPlot::BeginPlot("##song", ImVec2(-1, songH), ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes("time (s)", "song fraction", 0, ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.05, ImPlotCond_Always);
        ImPlot::SetupAxis(ImAxis_Y2, "ratio", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisScale(ImAxis_Y2, ImPlotScale_Log10);
        ImPlot::SetupAxisLinks(ImAxis_X1, &tMin_, &tMax_);
        ImPlot::SetupLegend(ImPlotLocation_SouthEast, ImPlotLegendFlags_Horizontal);
        if (song && env && !song->fraction.empty() && p.song.enabled) {
            const size_t m = song->fraction.size();
            const double dt = env->frameMs() / 1000.0;
            std::vector<double> x(m), frac(m), on(m), ratio(m);
            for (size_t i = 0; i < m; ++i) {
                x[i] = (i + 1) * dt;
                frac[i] = song->fraction[i];
                on[i] = song->active[i] * 1.05;
                ratio[i] = std::max<double>(song->ratio[i], 1e-6);
            }
            ImPlot::SetNextFillStyle(kBlue, 0.25f);
            ImPlot::PlotShaded("song", x.data(), on.data(), int(m), 0.0);
            ImPlot::SetNextLineStyle(kBlue, 1.5f);
            ImPlot::PlotLine("fraction", x.data(), frac.data(), int(m));
            if (ImPlot::DragLineY(200, &p.song.durationThresh, kBlue, 1.5f)) {
                p.song.durationThresh = std::clamp(p.song.durationThresh, 0.0, 1.0);
                changed() = true;
            }
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::SetNextLineStyle(ImVec4(0.8f, 0.8f, 0.8f, 0.8f), 1.0f);
            ImPlot::PlotLine("ratio", x.data(), ratio.data(), int(m));
            if (ImPlot::DragLineY(201, &p.song.ratioThresh, ImVec4(0.85f, 0.85f, 0.85f, 1.0f), 1.5f)) {
                p.song.ratioThresh = std::max(p.song.ratioThresh, 1e-9);
                changed() = true;
            }
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        }
        markers();
        ImPlot::EndPlot();
    }
    ImPlot::EndAlignedPlots();
}

// ---- clean-up: find and move away files without song

namespace {
// Files and bytes under a folder (for the confirmation dialogs).
void folderSize(const std::string& dir, size_t& files, uintmax_t& bytes) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fs::u8path(dir), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) {
            ++files;
            bytes += it->file_size(e2);
        }
    }
}

std::string sizeText(uintmax_t bytes) {
    char buf[32];
    if (bytes >= (uintmax_t(1) << 30))
        std::snprintf(buf, sizeof buf, "%.2f GB", double(bytes) / double(uintmax_t(1) << 30));
    else
        std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / double(uintmax_t(1) << 20));
    return buf;
}
}  // namespace

// Per analyzed file: the longest stretch with the song detector on, and whether that
// reaches keepSongSec_. Uses the song track from 'Analyze folder' (current song settings
// at the time of the analysis).
void TuneView::updatePass() {
    if (passVersion_ == batchVersion_ && passKeep_ == keepSongSec_ && pass_.size() == batchEntries_.size()) return;
    passVersion_ = batchVersion_;
    passKeep_ = keepSongSec_;
    longestSong_.assign(batchEntries_.size(), -1.0);
    pass_.assign(batchEntries_.size(), -1);
    for (size_t i = 0; i < batchEntries_.size(); ++i) {
        const auto& e = batchEntries_[i];
        if (!e.error.empty() || !e.song || !e.env) continue;
        size_t run = 0, best = 0;
        for (uint8_t a : e.song->active) {
            run = a ? run + 1 : 0;
            best = std::max(best, run);
        }
        longestSong_[i] = double(best) * e.env->frameMs() / 1000.0;
        pass_[i] = longestSong_[i] >= keepSongSec_ ? 1 : 0;
    }
}

void TuneView::drawCleanup(const AppParams& p, bool stale) {
    size_t failing = 0, known = 0;
    for (size_t i = 0; i < pass_.size(); ++i) {
        if (pass_[i] < 0) continue;
        ++known;
        if (pass_[i] == 0) ++failing;
    }
    ImGui::SeparatorText("Clean up: files without song");
    ImGui::SetNextItemWidth(4.0f * em());
    ImGui::InputDouble("Keep files with song for at least (s)", &keepSongSec_, 0, 0, "%.3g");
    keepSongSec_ = std::clamp(keepSongSec_, 0.0, 600.0);
    HelpMarker("A file passes if the song detector (Song Detection settings of the tuning set) stays on for at least "
               "this long in one stretch. Failing files are shown in red in the file list and the table below. Tune "
               "the song detector and detection settings until only song passes, then move the rest away.");
    ImGui::SameLine();
    ImGui::TextColored(failing ? kRed : kGreen, "%zu of %zu file(s) fail", failing, known);

    const bool songOff = !p.song.enabled;
    const bool busy = batch_.running();
    const bool canMove = failing > 0 && !stale && !busy && !songOff;
    ImGui::BeginDisabled(!canMove);
    char label[96];
    std::snprintf(label, sizeof label, "Move %zu failing file(s) to 'rejected'...", failing);
    if (ImGui::Button(label)) {
        moveBytes_ = 0;  // sizes once, not every frame (slow on network shares)
        for (size_t i = 0; i < pass_.size(); ++i) {
            std::error_code ec;
            if (pass_[i] == 0) moveBytes_ += fs::file_size(fs::u8path(batchEntries_[i].path), ec);
        }
        ImGui::OpenPopup("Move failing files###confirmmove");
    }
    ImGui::EndDisabled();
    if (!canMove && failing > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", songOff ? "The song detector is off in the tuning set."
                                : busy  ? "Wait for the analysis to finish."
                                        : "Settings changed since the analysis: press 'Analyze folder' again first.");
    ImGui::SameLine();
    ImGui::BeginDisabled(rejectDirs_.empty() || busy);
    if (ImGui::Button("Delete 'rejected' permanently...")) {
        delFiles_ = 0;
        delBytes_ = 0;
        for (const auto& d : rejectDirs_) folderSize(d, delFiles_, delBytes_);
        ImGui::OpenPopup("Delete rejected files###confirmdelete");
    }
    ImGui::EndDisabled();
    if (!cleanupMsg_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "%s", cleanupMsg_.c_str());
    }

    if (ImGui::BeginPopupModal("Move failing files###confirmmove", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Move %zu file(s) (%s) without at least %.3g s of song into a 'rejected' subfolder next to them?",
                    failing, sizeText(moveBytes_).c_str(), keepSongSec_);
        ImGui::TextDisabled("Nothing is deleted: you can move them back, or check them and then use 'Delete "
                            "'rejected' permanently'.");
        if (ImGui::Button("Move")) {
            moveFailing();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete rejected files###confirmdelete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kRed, "Permanently delete %zu file(s) (%s)? This cannot be undone.", delFiles_,
                           sizeText(delBytes_).c_str());
        for (const auto& d : rejectDirs_) ImGui::BulletText("%s", d.c_str());
        if (ImGui::Button("Delete permanently")) {
            deleteRejected();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void TuneView::moveFailing() {
    std::set<std::string> moved;
    size_t errors = 0;
    std::string firstError;
    for (size_t i = 0; i < pass_.size() && i < batchEntries_.size(); ++i) {
        if (pass_[i] != 0) continue;
        const fs::path src = fs::u8path(batchEntries_[i].path);
        const fs::path dir = src.parent_path() / "rejected";
        std::error_code ec;
        fs::create_directories(dir, ec);
        // Never overwrite: add _1, _2, ... if a file of that name was rejected before.
        fs::path dst = dir / src.filename();
        for (int k = 1; fs::exists(dst, ec); ++k)
            dst = dir / fs::u8path(src.stem().u8string() + "_" + std::to_string(k) + src.extension().u8string());
        fs::rename(src, dst, ec);
        if (ec) {
            if (!errors++) firstError = src.filename().u8string() + ": " + ec.message();
            continue;
        }
        moved.insert(batchEntries_[i].path);
        const std::string d = dir.u8string();
        if (std::find(rejectDirs_.begin(), rejectDirs_.end(), d) == rejectDirs_.end()) rejectDirs_.push_back(d);
    }
    // Drop the moved files from the list and the results; no need to analyze again.
    const std::string selPath = selected_ >= 0 && selected_ < int(files_.size()) ? files_[size_t(selected_)] : "";
    files_.erase(std::remove_if(files_.begin(), files_.end(), [&](const std::string& f) { return moved.count(f) > 0; }),
                 files_.end());
    batch_.removePaths(moved);
    const auto it = std::find(files_.begin(), files_.end(), selPath);
    selected_ = files_.empty() ? -1 : it != files_.end() ? int(it - files_.begin()) : 0;
    reqPath_.clear();
    cleanupMsg_ = "Moved " + std::to_string(moved.size()) + " file(s) to 'rejected'" +
                  (errors ? "; " + std::to_string(errors) + " failed (" + firstError + ")" : "");
}

void TuneView::deleteRejected() {
    size_t files = 0;
    std::string firstError;
    std::vector<std::string> kept;
    for (const auto& d : rejectDirs_) {
        size_t n = 0;
        uintmax_t b = 0;
        folderSize(d, n, b);
        std::error_code ec;
        fs::remove_all(fs::u8path(d), ec);
        if (ec) {
            if (firstError.empty()) firstError = d + ": " + ec.message();
            kept.push_back(d);
        } else {
            files += n;
        }
    }
    rejectDirs_ = kept;
    cleanupMsg_ = "Deleted " + std::to_string(files) + " file(s)" + (firstError.empty() ? "" : "; " + firstError);
}

void TuneView::drawFolderSummary(AppParams& p) {
    // ---- run / progress
    if (batch_.running()) {
        if (ImGui::Button("Cancel")) batch_.cancel();
        ImGui::SameLine();
        char buf[64];
        std::snprintf(buf, sizeof buf, "%zu / %zu files", batchEntries_.size(), files_.size());
        ImGui::ProgressBar(batch_.progress(), ImVec2(20.0f * em(), 0), buf);
    } else {
        ImGui::BeginDisabled(files_.empty());
        if (ImGui::Button("Analyze folder")) batch_.start(files_, p);
        ImGui::EndDisabled();
    }
    HelpMarker("Computes the detection signal and song detector for every file (the slow part, using all CPU cores). "
               "After that, changing thresholds, setpoints or feedback settings re-runs the decisions on every file "
               "instantly. Changing bands, gains or song-detector settings needs another 'Analyze folder'.");
    const bool stale = !batchEntries_.empty() && (batchEnvKey_ != tracksKey(p) || batchSongKey_ != songKey(p));
    if (stale) {
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "Band / gain / song settings changed since this analysis: re-analyze to update.");
    }
    if (batchEntries_.empty()) {
        ImGui::TextDisabled("Choose a folder and press 'Analyze folder' to see IOI / duration distributions and how "
                            "often the target matches in each file.");
        return;
    }

    size_t files = 0, syl = 0, matches = 0, blocked = 0, fires = 0, withMatch = 0, errors = 0;
    double seconds = 0.0;
    for (size_t i = 0; i < batchEntries_.size(); ++i) {
        if (!batchEntries_[i].error.empty()) {
            ++errors;
            continue;
        }
        ++files;
        seconds += batchEntries_[i].durationSec;
        if (i < batchSummary_.size()) {
            const auto& s = batchSummary_[i];
            syl += s.syllables;
            matches += s.matches;
            blocked += s.blocked;
            fires += s.fires;
            withMatch += s.matches > 0;
        }
    }
    const SyllableCode* tgt = targetSyllable(p.codes);
    ImGui::Text("%zu files, %.1f min of audio  |  %zu syllables  |  target '%s': %zu matches in %zu files (%.1f%%)  |  "
                "%zu blocked by song gate  |  %zu feedback%s",
                files, seconds / 60.0, syl, tgt ? tgt->name.c_str() : "none", matches, withMatch,
                files ? 100.0 * withMatch / files : 0.0, blocked, fires,
                errors ? "  |  some files could not be read" : "");
    drawCleanup(p, batchSongKey_ != songKey(p));

    // ---- choose the syllable being edited (counts = first matches across the folder)
    auto& list = p.codes.list;
    std::vector<size_t> perCode(list.size(), 0);
    size_t none = 0;
    for (const auto& s : batchSyllables_) {
        if (std::isnan(s.ioiMs)) continue;  // no IOI end -> not classified
        if (s.codeIndex >= 0 && s.codeIndex < int(perCode.size()))
            ++perCode[size_t(s.codeIndex)];
        else
            ++none;
    }
    if (list.empty()) {
        ImGui::TextDisabled("No syllables yet: Ctrl+Shift+click a syllable in the File view, or use '+ Add syllable' "
                            "in the Syllables panel.");
        return;
    }
    *edit_ = std::clamp(*edit_, 0, int(list.size()) - 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Editing:");
    for (int i = 0; i < int(list.size()); ++i) {
        const std::string label = std::string(i == p.codes.target ? "* " : "") + list[i].name + " (" +
                                  std::to_string(list[i].code) + "): " + std::to_string(perCode[i]) + "##ed" +
                                  std::to_string(i);
        const float w = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + 2 * ImGui::GetStyle().FramePadding.x;
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
        const bool sel = i == *edit_;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.50f, 0.10f, 1.0f));
        if (ImGui::Button(label.c_str())) *edit_ = i;
        if (sel) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  no match: %zu", none);
    HelpMarker("Counts are first matches over the whole folder; * = feedback target. Pick the syllable to edit; the "
               "plots below show and drag its windows. Ctrl+click in the File view centres it on a syllable.");

    SyllableCode& e = list[size_t(*edit_)];
    ImGui::PushID("editsyl");
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
    ImGui::SameLine();
    Check("enabled", &e.enabled);
    ImGui::SameLine();
    if (*edit_ == p.codes.target) {
        ImGui::TextColored(kPurple, "  this is the feedback target");
    } else if (ImGui::Button("Make feedback target")) {
        p.codes.target = *edit_;
        changed() = true;
    }
    ImGui::PopID();

    // ---- distributions of the values the edited syllable is checked against
    const bool minMax = e.mode == RangeMode::MinMax;
    std::vector<double> ioiAll, ioiM, sdAll, sdM, prevAll, prevM, scatX, scatY, scatMX, scatMY, durAll, durM;
    for (const auto& s : batchSyllables_) {
        const auto c = comparedValues(s);
        const bool mine = s.codeIndex == *edit_;
        // The classified syllable's own duration (NaN if a file ends mid-syllable).
        if (!std::isnan(s.sdMs) && s.sdMs < 1000) (mine ? durM : durAll).push_back(s.sdMs);
        if (!std::isnan(c.ioi) && c.ioi < 1000) (mine ? ioiM : ioiAll).push_back(c.ioi);
        if (!std::isnan(c.sd) && c.sd < 1000) (mine ? sdM : sdAll).push_back(c.sd);
        if (!std::isnan(c.prevIoi) && c.prevIoi < 1000) (mine ? prevM : prevAll).push_back(c.prevIoi);
        if (!std::isnan(c.ioi) && !std::isnan(c.sd) && c.ioi < 1000 && c.sd < 1000) {
            (mine ? scatMX : scatX).push_back(c.ioi);
            (mine ? scatMY : scatY).push_back(c.sd);
        }
    }
    const float histH = std::max(8.0f * em(), ImGui::GetContentRegionAvail().y * 0.38f);
    const float w3 = (ImGui::GetContentRegionAvail().x - 4 * ImGui::GetStyle().ItemSpacing.x) / 5.0f;
    int dragId = 300;
    auto bounds = [&](const TimeWindow& w) {
        return minMax ? std::pair<double, double>{w.minMs, w.maxMs}
                      : std::pair<double, double>{w.setpointMs - w.epsMs, w.setpointMs + w.epsMs};
    };
    auto hist = [&](const char* title, const std::vector<double>& all, const std::vector<double>& m, TimeWindow& w,
                    bool used) {
        if (!ImPlot::BeginPlot(title, ImVec2(w3, histH), ImPlotFlags_NoMenus | ImPlotFlags_NoLegend)) return;
        ImPlot::SetupAxes("ms", "count", 0, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, 400, ImPlotCond_Once);
        const ImPlotRange range(0, 1000);
        if (!all.empty()) {
            ImPlot::SetNextFillStyle(kGrey, 0.6f);
            ImPlot::PlotHistogram("all", all.data(), int(all.size()), 500, 1.0, range);
        }
        if (!m.empty()) {
            ImPlot::SetNextFillStyle(kPurple, 0.9f);
            ImPlot::PlotHistogram("this syllable", m.data(), int(m.size()), 500, 1.0, range);
        }
        if (used) {
            const ImVec4 c = kAmber, faint(c.x, c.y, c.z, 0.6f);
            if (minMax) {
                if (ImPlot::DragLineX(dragId, &w.minMs, c, 2.0f)) {
                    w.minMs = std::min(w.minMs, w.maxMs);
                    changed() = true;
                }
                if (ImPlot::DragLineX(dragId + 1, &w.maxMs, c, 2.0f)) {
                    w.maxMs = std::max(w.maxMs, w.minMs);
                    changed() = true;
                }
            } else {
                double lo = w.setpointMs - w.epsMs, hi = w.setpointMs + w.epsMs;
                ImPlot::SetNextLineStyle(faint, 1.0f);
                ImPlot::PlotInfLines("##lo", &lo, 1);
                if (ImPlot::DragLineX(dragId, &w.setpointMs, c, 2.0f)) changed() = true;
                if (ImPlot::DragLineX(dragId + 1, &hi, faint, 1.0f)) {
                    w.epsMs = std::max(0.5, hi - w.setpointMs);
                    changed() = true;
                }
            }
        } else {
            ImPlot::Annotation(200, 0, kGrey, ImVec2(0, -20), true, "not used by %s", toString(e.type));
        }
        dragId += 2;
        ImPlot::EndPlot();
    };
    hist("IOI", ioiAll, ioiM, e.ioi, usesIoi(e.type));
    ImGui::SameLine();
    hist("SD of previous syllable", sdAll, sdM, e.sd, usesSd(e.type));
    ImGui::SameLine();
    hist("Previous IOI", prevAll, prevM, e.prevIoi, usesPrevIoi(e.type));
    ImGui::SameLine();

    // Durations of the syllables classified as this one. Not part of the definition
    // (it is only known after the syllable ends), but a tight single peak is a good
    // sign the windows pick out one syllable type; a wide or double peak means a
    // neighbouring syllable is getting through.
    {
        double mean = 0.0, sd = 0.0;
        for (double d : durM) mean += d;
        if (!durM.empty()) mean /= double(durM.size());
        for (double d : durM) sd += (d - mean) * (d - mean);
        if (durM.size() > 1) sd = std::sqrt(sd / double(durM.size() - 1));
        const std::string title = "Duration of '" + e.name + "'###dur";
        if (ImPlot::BeginPlot(title.c_str(), ImVec2(w3, histH), ImPlotFlags_NoMenus | ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("ms", "count", 0, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 300, ImPlotCond_Once);
            const ImPlotRange range(0, 1000);
            if (!durAll.empty()) {
                ImPlot::SetNextFillStyle(kGrey, 0.35f);
                ImPlot::PlotHistogram("all", durAll.data(), int(durAll.size()), 1000, 1.0, range);
            }
            if (!durM.empty()) {
                ImPlot::SetNextFillStyle(kPurple, 0.9f);
                ImPlot::PlotHistogram("this syllable", durM.data(), int(durM.size()), 1000, 1.0, range);
                ImPlot::SetNextLineStyle(kAmber, 1.0f);
                ImPlot::PlotInfLines("mean", &mean, 1);
            }
            // Stats in the top-right corner (clamped inside the plot).
            const ImPlotRect lim = ImPlot::GetPlotLimits();
            if (durM.empty())
                ImPlot::Annotation(lim.X.Max, lim.Y.Max, kGrey, ImVec2(-4, 4), true, "none classified");
            else
                ImPlot::Annotation(lim.X.Max, lim.Y.Max, kPurple, ImVec2(-4, 4), true,
                                   "n = %zu\n%.1f +/- %.1f ms\nCV %.0f%%", durM.size(), mean, sd,
                                   mean > 0 ? 100.0 * sd / mean : 0.0);
            ImPlot::EndPlot();
        }
    }
    ImGui::SameLine();
    if (ImPlot::BeginPlot("IOI vs SD of previous", ImVec2(w3, histH), ImPlotFlags_NoMenus | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("IOI ms", "SD ms", 0, 0);
        ImPlot::SetupAxesLimits(0, 400, 0, 300, ImPlotCond_Once);
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, ImVec4(0.75f, 0.75f, 0.75f, 0.7f), 0.0f);
        if (!scatX.empty()) ImPlot::PlotScatter("all", scatX.data(), scatY.data(), int(scatX.size()));
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 4.0f, kPurple, 0.0f);
        if (!scatMX.empty()) ImPlot::PlotScatter("this syllable", scatMX.data(), scatMY.data(), int(scatMX.size()));
        // The window as a box (an unused SD spans the whole axis).
        const auto [x0, x1] = bounds(e.ioi);
        const auto [y0, y1] = usesSd(e.type) ? bounds(e.sd) : std::pair<double, double>{0.0, 1000.0};
        const double bx[5] = {x0, x1, x1, x0, x0}, by[5] = {y0, y0, y1, y1, y0};
        ImPlot::SetNextLineStyle(kAmber, 1.5f);
        ImPlot::PlotLine("window", bx, by, 5);
        ImPlot::EndPlot();
    }
    ImGui::TextDisabled("Grey = all syllables, purple = syllables classified as '%s' (first match). Drag the amber "
                        "lines to move %s. 'Duration' should be one narrow peak if only one syllable type is caught.",
                        e.name.c_str(), minMax ? "min / max" : "the setpoint (thick) or +/- eps (thin)");

    // ---- per-file table
    if (ImGui::BeginTable("batchfiles", 8,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                              ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("file", ImGuiTableColumnFlags_WidthStretch);
        const std::string editedCol = "'" + e.name + "'";
        for (const char* c : {"sec", "song s", "syllables", editedCol.c_str(), "target", "blocked", "feedback"})
            ImGui::TableSetupColumn(c, ImGuiTableColumnFlags_WidthFixed, 5.5f * em());
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(batchEntries_.size()));
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const auto& e = batchEntries_[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                const bool sel = selected_ >= 0 && selected_ < int(files_.size()) && files_[selected_] == e.path;
                const bool fails = size_t(i) < pass_.size() && pass_[size_t(i)] == 0;
                if (fails) ImGui::PushStyleColor(ImGuiCol_Text, kRed);
                if (ImGui::Selectable(fileName(e.path).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
                    const auto it = std::find(files_.begin(), files_.end(), e.path);
                    if (it != files_.end()) {
                        select(int(it - files_.begin()));
                        openFileTab_ = true;
                    }
                }
                if (fails) ImGui::PopStyleColor();
                ImGui::PopID();
                if (!e.error.empty()) {
                    ImGui::TableNextColumn();
                    ImGui::TextColored(kRed, "%s", e.error.c_str());
                    continue;
                }
                const FileSummary s = i < int(batchSummary_.size()) ? batchSummary_[i] : FileSummary{};
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", e.durationSec);
                ImGui::TableNextColumn();
                const double song = size_t(i) < longestSong_.size() ? longestSong_[size_t(i)] : -1.0;
                if (song >= 0) ImGui::TextColored(fails ? kRed : kGreen, "%.2f", song);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", s.syllables);
                ImGui::TableNextColumn();
                const size_t mine = *edit_ < int(s.perCode.size()) ? s.perCode[size_t(*edit_)] : 0;
                ImGui::TextColored(mine ? kAmber : kGrey, "%zu", mine);
                ImGui::TableNextColumn();
                ImGui::TextColored(s.matches ? kPurple : kGrey, "%zu", s.matches);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", s.blocked);
                ImGui::TableNextColumn();
                ImGui::TextColored(s.fires ? kRed : kGrey, "%zu", s.fires);
            }
        }
        ImGui::EndTable();
    }
}

}  // namespace fp

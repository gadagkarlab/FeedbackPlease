#pragma once
// The "MSN finder" tab: pick a CatGT + Kilosort run and a syllable, then page through the units ranked by
// increasing variance of their spike-time histogram, each with a figure like the msnFinder notebook's
// (mean spectrogram, syllable-aligned raster, spikes-in-window per rendition, template, probe position).
// Figures are drawn live and never saved.

#include <memory>
#include <string>
#include <vector>

#include "app/MsnFinder.h"

namespace fp {

class MsnFinderView {
public:
    ~MsnFinderView();

    void load(const std::string& path, std::string* status = nullptr);
    bool save(const std::string& path, std::string* err = nullptr) const;
    void reset() { p_ = MsnParams(); }

    // Same as the Load session button.
    void loadSession() {
        std::string err;
        message_ = backend_.requestLoad(p_, err) ? std::string() : err;
    }

    void drawParams();  // left-hand panel
    void drawMain();    // ranking list and figure
    void shutdown() { backend_.restart(); }

    bool busy() const { return backend_.busy(); }

private:
    void sync();          // react to new snapshots from the back end
    void drawStatus();
    void drawRankTable(float height);
    void drawFigure(float height);
    void selectRank(int r);
    void prepareSpec(const MsnSpec& s);

    MsnParams p_;
    MsnBackend backend_;

    std::vector<std::string> log_;
    size_t logCursor_ = 0;

    uint64_t seenSession_ = 0, seenRanking_ = 0, seenFigure_ = 0;
    std::shared_ptr<const MsnSession> session_;
    std::shared_ptr<const MsnRanking> ranking_;
    std::shared_ptr<const MsnFigure> fig_;
    int sel_ = 0;                     // index into ranking_->rows
    int reqLabel_ = -1, reqUnit_ = -1;
    double reqPad_ = -1;              // what the last figure request asked for

    // Spectrogram prepared for ImPlot: kRows x kCols, top row = highest frequency.
    std::shared_ptr<const MsnSpec> specSrc_;
    std::vector<float> specVals_;
    float specLo_ = 0, specHi_ = 1;
    int specRows_ = 0, specCols_ = 0;

    double xMin_ = 0, xMax_ = 1;  // shared time axis of the spectrogram, raster and rolling plots
    int limLabel_ = -1;
    double limPad_ = -1;

    std::string message_;
    bool showLog_ = false;
};

}  // namespace fp

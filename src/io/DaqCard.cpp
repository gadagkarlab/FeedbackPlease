// Multi-box hardware: all boxes whose microphones are on one NI card.
//
//   AI : one continuous task with one channel per box (a card runs only one AI task).
//   DO : one on-demand task holding every box's TTL lines on this card, written once
//        per frame (only when something changed).
//   Code ports: one on-demand task per box (NI MAX task name or port spec).
//   AO : one continuous, non-regenerating stream covering the boxes' speaker channels,
//        clocked by this card's AI sample clock. Every frame writes exactly one frame of
//        samples per channel, each box's stimulus mixed into its own channel, so the
//        boxes play independently. Output latency = the prefill (Hardware: "AO stream
//        buffer"); if the PC stalls longer than that, the stream underflows, the error is
//        logged and the stream restarts.

#include "io/FrameSource.h"

#ifdef FP_HAVE_DAQMX

#include <NIDAQmx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#include "core/WavFile.h"

namespace fp {

namespace {

std::string daqError(int32 code, const std::string& what) {
    char buf[2048] = {};
    DAQmxGetExtendedErrorInfo(buf, sizeof buf);
    return what + " failed (" + std::to_string(code) + "): " + buf;
}

int32 toDaqTerminal(TerminalConfig t) {
    switch (t) {
        case TerminalConfig::RSE: return DAQmx_Val_RSE;
        case TerminalConfig::NRSE: return DAQmx_Val_NRSE;
        case TerminalConfig::Differential: return DAQmx_Val_Diff;
        case TerminalConfig::PseudoDiff: return DAQmx_Val_PseudoDiff;
        case TerminalConfig::Default: break;
    }
    return DAQmx_Val_Cfg_Default;
}

// Linear resampling, used when a feedback wave file's rate differs from the card's rate.
std::vector<float> resample(const std::vector<float>& x, double from, double to) {
    if (from == to || x.empty()) return x;
    const size_t n = size_t(std::floor((x.size() - 1) * to / from)) + 1;
    std::vector<float> y(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = i * from / to;
        const size_t a = size_t(t);
        const double f = t - double(a);
        y[i] = float(a + 1 < x.size() ? x[a] * (1.0 - f) + x[a + 1] * f : x[a]);
    }
    return y;
}

class DaqCard;

// One box's view of the card's outputs.
class BoxOut final : public IOBackend {
public:
    BoxOut(DaqCard* card, int k) : card_(card), k_(k) {}
    bool start(const AppParams&, std::string&) override { return true; }
    void stop() override {}
    double sampleRate() const override { return 0.0; }
    bool read(float*, int, std::string&) override { return false; }
    void writeDigital(const DigitalOutputs& d) override { dout = d; }
    void writeSyllableCode(uint8_t code) override;
    void fireWave(int which) override;
    std::string takeOutputError() override {
        std::string e;
        e.swap(err);
        return e;
    }
    const char* name() const override { return "NI-DAQmx card"; }

    DigitalOutputs dout;
    std::string err;

private:
    DaqCard* card_;
    int k_;
};

class DaqCard final : public FrameSource {
public:
    DaqCard(std::string dev, std::vector<AppParams> boxes) : dev_(std::move(dev)), p_(std::move(boxes)) {
        for (int k = 0; k < int(p_.size()); ++k) out_.push_back(std::make_unique<BoxOut>(this, k));
        name_ = "NI-DAQmx " + dev_;
    }
    ~DaqCard() override { stop(); }

    bool start(std::string& err) override {
        stop();
        const bool ok = startAI(err) && startDO(err) && startCodes(err) && startAO(err) &&
                        check(DAQmxStartTask(ai_), "DAQmxStartTask(AI)", err);  // also starts the AO clock
        if (!ok) stop();
        return ok;
    }

    void stop() override {
        if (ai_) {
            DAQmxStopTask(ai_);
            DAQmxClearTask(ai_);
            ai_ = 0;
        }
        if (do_) {
            std::fill(doData_.begin(), doData_.end(), uInt8(0));
            int32 w = 0;
            DAQmxWriteDigitalLines(do_, 1, 1, 0.1, DAQmx_Val_GroupByChannel, doData_.data(), &w, nullptr);
            DAQmxStopTask(do_);
            DAQmxClearTask(do_);
            do_ = 0;
        }
        for (auto& t : code_) {
            if (!t) continue;
            uInt8 z = 0;
            int32 w = 0;
            DAQmxWriteDigitalU8(t, 1, 1, 0.1, DAQmx_Val_GroupByChannel, &z, &w, nullptr);
            DAQmxStopTask(t);
            DAQmxClearTask(t);
            t = 0;
        }
        if (ao_) {
            const bool midStimulus = std::any_of(play_.begin(), play_.end(), [](const Play& p) { return p.wave >= 0; });
            DAQmxStopTask(ao_);
            DAQmxClearTask(ao_);
            ao_ = 0;
            if (midStimulus) zeroAO();  // a stopped stream holds its last sample
        }
        std::fill(play_.begin(), play_.end(), Play{});
    }

    int boxCount() const override { return int(p_.size()); }
    double sampleRate() const override { return fs_; }
    int frameSamples() const override { return n_; }
    const char* name() const override { return name_.c_str(); }

    bool read(std::string& err) override {
        const int k = boxCount();
        int32 got = 0;
        const int32 code = DAQmxReadAnalogF64(ai_, n_, 2.0, DAQmx_Val_GroupByChannel, buf_.data(),
                                              uInt32(buf_.size()), &got, nullptr);
        if (DAQmxFailed(code)) {
            err = daqError(code, "DAQmxReadAnalogF64 (" + dev_ + ")");
            return false;
        }
        if (got != n_) {
            err = "DAQmxReadAnalogF64 returned " + std::to_string(got) + " of " + std::to_string(n_) + " samples";
            return false;
        }
        for (size_t i = 0; i < size_t(k) * size_t(n_); ++i) frames_[i] = float(buf_[i]);
        return true;
    }

    const float* frame(int k) const override { return &frames_[size_t(k) * size_t(n_)]; }
    IOBackend& out(int k) override { return *out_[size_t(k)]; }

    size_t backlog() override {
        uInt32 avail = 0;
        if (ai_) DAQmxGetReadAvailSampPerChan(ai_, &avail);
        return avail;
    }

    void endFrame() override {
        writeDO();
        writeAO();
    }

    void writeCode(int k, uint8_t code) {
        TaskHandle t = code_[size_t(k)];
        if (!t) return;
        uInt8 v = code;
        int32 w = 0;
        const int32 c = DAQmxWriteDigitalU8(t, 1, 1, 0.1, DAQmx_Val_GroupByChannel, &v, &w, nullptr);
        if (DAQmxFailed(c)) out_[size_t(k)]->err = daqError(c, "DAQmxWriteDigitalU8 (syllable code)");
    }

    void fire(int k, int which) {
        if (which < 1 || which > 2) return;  // the pulse burst (3) is single-box only
        if (aoChan_[size_t(k)] < 0 || waves_[size_t(k)][size_t(which - 1)].empty()) return;
        play_[size_t(k)] = Play{which - 1, 0};  // restarts if something was playing
    }

private:
    struct Play {
        int wave = -1;  // index into waves_[box], -1 = silent
        size_t pos = 0;
    };

    bool check(int32 code, const std::string& what, std::string& err) {
        if (DAQmxFailed(code)) {
            err = daqError(code, what);
            return false;
        }
        return true;
    }

    bool sameDevice(const std::string& chan, const std::string& what, int k, std::string& err) {
        if (deviceOf(chan) == dev_) return true;
        err = p_[size_t(k)].boxName + ": " + what + " '" + chan + "' must be on " + dev_ +
              " (the card of its microphone)";
        return false;
    }

    bool startAI(std::string& err) {
        const auto& h0 = p_[0].hw;
        for (const auto& p : p_) {
            if (p.hw.sampleRate != h0.sampleRate || p.hw.samplesPerFrame != h0.samplesPerFrame) {
                err = "boxes on " + dev_ + " must use the same sample rate and samples per frame";
                return false;
            }
        }
        const uInt32 bufSamps = uInt32(std::max(h0.sampleRate * h0.inputBufferSec, 10.0 * h0.samplesPerFrame));
        if (!check(DAQmxCreateTask(("FP_AI_" + dev_).c_str(), &ai_), "DAQmxCreateTask(AI)", err)) return false;
        for (const auto& p : p_) {
            if (!check(DAQmxCreateAIVoltageChan(ai_, p.hw.aiChannel.c_str(), "", toDaqTerminal(p.hw.terminalConfig),
                                                p.hw.aiMin, p.hw.aiMax, DAQmx_Val_Volts, nullptr),
                       "DAQmxCreateAIVoltageChan (" + p.boxName + ")", err))
                return false;
        }
        uInt32 nChans = 0;
        DAQmxGetTaskNumChans(ai_, &nChans);
        if (nChans != p_.size()) {
            err = "each box's AI channel must be a single channel (" + dev_ + " has " + std::to_string(nChans) +
                  " channels for " + std::to_string(p_.size()) + " boxes)";
            return false;
        }
        DAQmxSetAIDataXferReqCond(ai_, "", DAQmx_Val_OnBrdMemNotEmpty);  // lowest latency; ignored if unsupported
        if (!check(DAQmxCfgSampClkTiming(ai_, "", h0.sampleRate, DAQmx_Val_Rising, DAQmx_Val_ContSamps, bufSamps),
                   "DAQmxCfgSampClkTiming(AI)", err) ||
            !check(DAQmxCfgInputBuffer(ai_, bufSamps), "DAQmxCfgInputBuffer", err) ||
            !check(DAQmxGetSampClkRate(ai_, &fs_), "DAQmxGetSampClkRate", err))
            return false;
        n_ = h0.samplesPerFrame;
        buf_.assign(p_.size() * size_t(n_), 0.0);
        frames_.assign(p_.size() * size_t(n_), 0.0f);
        return true;
    }

    bool startDO(std::string& err) {
        doMap_.clear();
        for (int k = 0; k < int(p_.size()); ++k) {
            const auto& h = p_[size_t(k)].hw;
            const std::string* lines[4] = {&h.doFeedbackLine, &h.doSyllableLine, &h.doMatchLine, &h.doSongLine};
            for (int w = 0; w < 4; ++w) {
                if (lines[w]->empty()) continue;
                if (!sameDevice(*lines[w], "DO line", k, err)) return false;
                doMap_.push_back({k, w, *lines[w]});
            }
        }
        if (doMap_.empty()) return true;
        if (!check(DAQmxCreateTask(("FP_DO_" + dev_).c_str(), &do_), "DAQmxCreateTask(DO)", err)) return false;
        for (const auto& m : doMap_)
            if (!check(DAQmxCreateDOChan(do_, m.line.c_str(), "", DAQmx_Val_ChanForAllLines),
                       "DAQmxCreateDOChan (" + p_[size_t(m.box)].boxName + ")", err))
                return false;
        if (!check(DAQmxStartTask(do_), "DAQmxStartTask(DO)", err)) return false;
        doData_.assign(doMap_.size(), 0);
        doLast_.assign(doMap_.size(), 1);  // forces the first all-low write
        writeDO();
        return true;
    }

    bool startCodes(std::string& err) {
        code_.assign(p_.size(), 0);
        for (int k = 0; k < int(p_.size()); ++k) {
            const std::string& spec = p_[size_t(k)].hw.sylCodeOutput;
            if (spec.empty()) continue;
            TaskHandle& t = code_[size_t(k)];
            const std::string who = " (" + p_[size_t(k)].boxName + ")";
            if (spec.find('/') == std::string::npos) {
                if (!check(DAQmxLoadTask(spec.c_str(), &t), "DAQmxLoadTask syllable code" + who, err)) return false;
            } else if (!check(DAQmxCreateTask("", &t), "DAQmxCreateTask(code)" + who, err) ||
                       !check(DAQmxCreateDOChan(t, spec.c_str(), "", DAQmx_Val_ChanForAllLines),
                              "DAQmxCreateDOChan(code)" + who, err)) {
                return false;
            }
            if (!check(DAQmxStartTask(t), "DAQmxStartTask(syllable code)" + who, err)) return false;
            writeCode(k, 0);
        }
        return true;
    }

    bool startAO(std::string& err) {
        aoChan_.assign(p_.size(), -1);
        waves_.assign(p_.size(), {});
        play_.assign(p_.size(), Play{});
        aoChannels_.clear();
        for (int k = 0; k < int(p_.size()); ++k) {
            const auto& p = p_[size_t(k)];
            if (p.hw.aoChannel.empty() || (p.fb.wave1.empty() && p.fb.wave2.empty())) continue;
            if (!sameDevice(p.hw.aoChannel, "AO channel", k, err)) return false;
            for (const auto& c : aoChannels_)
                if (c == p.hw.aoChannel) {
                    err = p.boxName + ": AO channel " + c + " is already used by another box";
                    return false;
                }
            const std::string* paths[2] = {&p.fb.wave1, &p.fb.wave2};
            for (int w = 0; w < 2; ++w) {
                if (paths[w]->empty()) continue;
                WavData d;
                if (!readWav(*paths[w], d, &err)) {
                    err = p.boxName + ": " + err;
                    return false;
                }
                const auto s = resample(d.samples, d.sampleRate, fs_);
                auto& out = waves_[size_t(k)][size_t(w)];
                out.resize(s.size());
                for (size_t i = 0; i < s.size(); ++i)
                    out[i] = std::clamp(double(s[i]) * p.hw.aoScale, p.hw.aoMin, p.hw.aoMax);
            }
            aoChan_[size_t(k)] = int(aoChannels_.size());
            aoChannels_.push_back(p.hw.aoChannel);
        }
        if (aoChannels_.empty()) return true;

        const double frameMs = n_ * 1000.0 / fs_;
        prefillFrames_ = std::max(2, int(std::lround(p_[0].hw.aoStreamMs / frameMs)));
        if (!check(DAQmxCreateTask(("FP_AO_" + dev_).c_str(), &ao_), "DAQmxCreateTask(AO)", err)) return false;
        for (int k = 0; k < int(p_.size()); ++k)
            if (aoChan_[size_t(k)] >= 0 &&
                !check(DAQmxCreateAOVoltageChan(ao_, p_[size_t(k)].hw.aoChannel.c_str(), "", p_[size_t(k)].hw.aoMin,
                                                p_[size_t(k)].hw.aoMax, DAQmx_Val_Volts, nullptr),
                       "DAQmxCreateAOVoltageChan (" + p_[size_t(k)].boxName + ")", err))
                return false;
        // Clocked by this card's AI sample clock: AO advances exactly with the frames we read.
        const std::string clock = "/" + dev_ + "/ai/SampleClock";
        const uInt64 bufSamps = uInt64(std::max(fs_, 20.0 * n_));
        if (!check(DAQmxCfgSampClkTiming(ao_, clock.c_str(), fs_, DAQmx_Val_Rising, DAQmx_Val_ContSamps, bufSamps),
                   "DAQmxCfgSampClkTiming(AO stream)", err) ||
            !check(DAQmxSetWriteRegenMode(ao_, DAQmx_Val_DoNotAllowRegen), "DAQmxSetWriteRegenMode", err) ||
            !check(DAQmxCfgOutputBuffer(ao_, uInt32(bufSamps)), "DAQmxCfgOutputBuffer(AO)", err))
            return false;
        aoBuf_.assign(aoChannels_.size() * size_t(n_), 0.0);
        if (!prefillAO(err)) return false;
        // Started before AI: it waits for the AI sample clock, so both begin together.
        return check(DAQmxStartTask(ao_), "DAQmxStartTask(AO stream)", err);
    }

    bool prefillAO(std::string& err) {
        std::vector<double> zeros(aoChannels_.size() * size_t(n_) * size_t(prefillFrames_), 0.0);
        int32 w = 0;
        return check(DAQmxWriteAnalogF64(ao_, n_ * prefillFrames_, 0, 1.0, DAQmx_Val_GroupByChannel, zeros.data(), &w,
                                         nullptr),
                     "AO stream prefill", err);
    }

    void writeDO() {
        if (!do_) return;
        for (size_t i = 0; i < doMap_.size(); ++i) {
            const DigitalOutputs& d = out_[size_t(doMap_[i].box)]->dout;
            const bool v[4] = {d.feedback, d.syllable, d.match, d.song};
            doData_[i] = v[doMap_[i].which] ? 1 : 0;
        }
        if (doData_ == doLast_) return;
        int32 w = 0;
        const int32 code = DAQmxWriteDigitalLines(do_, 1, 1, 0.1, DAQmx_Val_GroupByChannel, doData_.data(), &w, nullptr);
        if (DAQmxFailed(code)) out_[0]->err = daqError(code, "DAQmxWriteDigitalLines (" + dev_ + ")");
        doLast_ = doData_;
    }

    void writeAO() {
        if (!ao_) return;
        // Mix: each box's stimulus into its own channel (group-by-channel layout).
        std::fill(aoBuf_.begin(), aoBuf_.end(), 0.0);
        for (int k = 0; k < int(p_.size()); ++k) {
            Play& pl = play_[size_t(k)];
            if (pl.wave < 0) continue;
            const auto& w = waves_[size_t(k)][size_t(pl.wave)];
            double* dst = &aoBuf_[size_t(aoChan_[size_t(k)]) * size_t(n_)];
            const size_t m = std::min(size_t(n_), w.size() - pl.pos);
            std::copy_n(&w[pl.pos], m, dst);
            pl.pos += m;
            if (pl.pos >= w.size()) pl = Play{};
        }
        int32 written = 0;
        const int32 code =
            DAQmxWriteAnalogF64(ao_, n_, 0, 0.1, DAQmx_Val_GroupByChannel, aoBuf_.data(), &written, nullptr);
        if (!DAQmxFailed(code)) return;
        // Most likely an underflow (the PC stalled longer than the stream buffer). Report it to
        // every box with a speaker on this card and restart the stream, at most once a second.
        const std::string e = daqError(code, "AO stream (" + dev_ + ")");
        for (int k = 0; k < int(p_.size()); ++k)
            if (aoChan_[size_t(k)] >= 0) out_[size_t(k)]->err = e;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRestart_ < std::chrono::seconds(1)) return;
        lastRestart_ = now;
        DAQmxStopTask(ao_);
        std::string pe;
        if (prefillAO(pe)) DAQmxStartTask(ao_);
    }

    void zeroAO() {
        for (int k = 0; k < int(p_.size()); ++k) {
            if (aoChan_[size_t(k)] < 0) continue;
            const auto& h = p_[size_t(k)].hw;
            TaskHandle t = 0;
            if (DAQmxFailed(DAQmxCreateTask("", &t))) continue;
            if (!DAQmxFailed(DAQmxCreateAOVoltageChan(t, h.aoChannel.c_str(), "", h.aoMin, h.aoMax, DAQmx_Val_Volts,
                                                      nullptr)))
                DAQmxWriteAnalogScalarF64(t, 1, 1.0, 0.0, nullptr);
            DAQmxClearTask(t);
        }
    }

    struct DoLine {
        int box;
        int which;  // 0 feedback, 1 syllable, 2 match, 3 song
        std::string line;
    };

    std::string dev_, name_;
    std::vector<AppParams> p_;
    std::vector<std::unique_ptr<BoxOut>> out_;
    double fs_ = 40000.0;
    int n_ = 40;

    TaskHandle ai_ = 0, do_ = 0, ao_ = 0;
    std::vector<TaskHandle> code_;
    std::vector<float64> buf_;
    std::vector<float> frames_;
    std::vector<DoLine> doMap_;
    std::vector<uInt8> doData_, doLast_;

    std::vector<int> aoChan_;  // per box: index of its channel in the AO task, -1 = none
    std::vector<std::string> aoChannels_;
    std::vector<std::array<std::vector<double>, 2>> waves_;  // per box: wave 1, wave 2 at fs_
    std::vector<Play> play_;
    std::vector<double> aoBuf_;
    int prefillFrames_ = 4;
    std::chrono::steady_clock::time_point lastRestart_{};
};

void BoxOut::writeSyllableCode(uint8_t code) { card_->writeCode(k_, code); }
void BoxOut::fireWave(int which) { card_->fire(k_, which); }

}  // namespace

std::unique_ptr<FrameSource> makeDaqCard(const std::string& device, const std::vector<AppParams>& boxes) {
    return std::make_unique<DaqCard>(device, boxes);
}

}  // namespace fp

#else  // built without NI-DAQmx

namespace fp {
std::unique_ptr<FrameSource> makeDaqCard(const std::string&, const std::vector<AppParams>&) { return nullptr; }
}  // namespace fp

#endif

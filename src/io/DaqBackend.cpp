#include "io/IOBackend.h"

#ifdef FP_HAVE_DAQMX

#include <NIDAQmx.h>

#include <algorithm>
#include <vector>

#include "core/NoiseBurst.h"
#include "core/WavFile.h"

namespace fp {

namespace {

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

std::string daqError(int32 code, const char* what) {
    char buf[2048] = {};
    DAQmxGetExtendedErrorInfo(buf, sizeof buf);
    return std::string(what) + " failed (" + std::to_string(code) + "): " + buf;
}

// Input:   continuous hardware-timed AI, read in frames from the engine thread.
// Digital: software-timed (on-demand) DO lines, written only when they change.
// Analog:  a finite, committed AO task holding one feedback waveform. The wave is
//          written while the state machine is in its delay ("arm"), so delivery only
//          costs a DAQmxStartTask on an already committed task.
class DaqBackend final : public IOBackend {
public:
    ~DaqBackend() override { stop(); }

    bool start(const AppParams& p, std::string& err) override {
        stop();
        fs_ = p.hw.sampleRate;
        const bool ok = startAI(p, err) && startDO(p, err) && startCode(p, err) && startAO(p, err) && startPulseInput(p, err) &&
                        check(DAQmxStartTask(ai_), "DAQmxStartTask(AI)", err);
        if (!ok) stop();
        return ok;
    }

    void stop() override {
        if (do_) {
            writeDigital(DigitalOutputs{});
            DAQmxStopTask(do_);
            DAQmxClearTask(do_);
            do_ = 0;
        }
        if (di_) {
            DAQmxStopTask(di_);
            DAQmxClearTask(di_);
            di_ = 0;
        }
        if (code_) {
            writeSyllableCode(0);
            DAQmxStopTask(code_);
            DAQmxClearTask(code_);
            code_ = 0;
        }
        // A stimulus cut short leaves the AO channel holding its last sample; drive it back to 0 V.
        const bool aoMidStimulus = ao_ && aoBusy();
        for (TaskHandle* t : {&ai_, &ao_}) {
            if (*t) {
                DAQmxStopTask(*t);
                DAQmxClearTask(*t);
                *t = 0;
            }
        }
        if (aoMidStimulus) zeroAO();
        doCount_ = 0;
        lastDo_ = DigitalOutputs{};
        aoLoaded_ = aoPending_ = 0;
        aoStarted_ = false;
        for (auto& w : waves_) w.clear();
        aoLen_ = 0;
        noiseSpent_ = false;
    }

    void zeroAO() {
        TaskHandle t = 0;
        if (aoChannel_.empty() || DAQmxFailed(DAQmxCreateTask("FeedbackPlease_AOzero", &t))) return;
        if (!DAQmxFailed(DAQmxCreateAOVoltageChan(t, aoChannel_.c_str(), "", aoMin_, aoMax_, DAQmx_Val_Volts, nullptr)))
            DAQmxWriteAnalogScalarF64(t, 1, 1.0, 0.0, nullptr);
        DAQmxClearTask(t);
    }

    double sampleRate() const override { return fs_; }

    bool read(float* dst, int n, std::string& err) override {
        if (int(buf_.size()) < n) buf_.resize(n);
        int32 got = 0;
        const int32 code = DAQmxReadAnalogF64(ai_, n, 2.0, DAQmx_Val_GroupByChannel, buf_.data(), uInt32(n), &got, nullptr);
        if (DAQmxFailed(code)) {
            err = daqError(code, "DAQmxReadAnalogF64");
            return false;
        }
        if (got != n) {
            err = "DAQmxReadAnalogF64 returned " + std::to_string(got) + " of " + std::to_string(n) + " samples";
            return false;
        }
        for (int i = 0; i < n; ++i) dst[i] = float(buf_[i]);
        return true;
    }

    size_t backlog() override {
        uInt32 avail = 0;
        if (ai_) DAQmxGetReadAvailSampPerChan(ai_, &avail);
        return avail;
    }

    void writeDigital(const DigitalOutputs& d) override {
        if (!do_ || d == lastDo_) return;
        uInt8 data[4];
        int k = 0;
        if (hasFb_) data[k++] = d.feedback;
        if (hasSyl_) data[k++] = d.syllable;
        if (hasMatch_) data[k++] = d.match;
        if (hasSong_) data[k++] = d.song;
        int32 written = 0;
        const int32 code = DAQmxWriteDigitalLines(do_, 1, 1, 0.1, DAQmx_Val_GroupByChannel, data, &written, nullptr);
        if (DAQmxFailed(code)) outErr_ = daqError(code, "DAQmxWriteDigitalLines");
        lastDo_ = d;
    }

    void writeSyllableCode(uint8_t code) override {
        if (!code_) return;
        uInt8 v = code;
        int32 written = 0;
        const int32 c = DAQmxWriteDigitalU8(code_, 1, 1, 0.1, DAQmx_Val_GroupByChannel, &v, &written, nullptr);
        if (DAQmxFailed(c)) outErr_ = daqError(c, "DAQmxWriteDigitalU8 (syllable code)");
    }

    int pollPulseInput() override {
        if (!di_) return -1;
        uInt8 v = 0;
        int32 read = 0, bytes = 0;
        const int32 code = DAQmxReadDigitalLines(di_, 1, 1.0, DAQmx_Val_GroupByChannel, &v, 1, &read, &bytes, nullptr);
        if (DAQmxFailed(code)) {
            outErr_ = daqError(code, "DAQmxReadDigitalLines (pulse input)");
            return -1;
        }
        return v ? 1 : 0;
    }

    void armWave(int w) override {
        if (!hasWave(w)) return;
        if (aoBusy()) {
            aoPending_ = w;  // load once the current playback finishes
            return;
        }
        if (aoLoaded_ != w) loadWave(w);
    }

    void fireWave(int w) override {
        if (!hasWave(w)) return;
        if (aoBusy()) {
            DAQmxStopTask(ao_);  // truncate the previous stimulus
            aoStarted_ = false;
        }
        if (aoLoaded_ != w && !loadWave(w)) return;
        const int32 code = DAQmxStartTask(ao_);
        if (DAQmxFailed(code)) {
            outErr_ = daqError(code, "DAQmxStartTask(AO)");
            return;
        }
        aoStarted_ = true;
        aoPending_ = 0;
        if (w == 3) noiseSpent_ = true;  // a fresh burst is generated once this one has finished
    }

    void service() override {
        if (noiseSpent_ && !aoBusy()) {  // new noise for the next trigger, ready before it arrives
            noiseSpent_ = false;
            if (regenerateBurst() && aoLoaded_ == 3) loadWave(3);
        }
        if (aoPending_ && !aoBusy()) {
            if (aoLoaded_ != aoPending_) loadWave(aoPending_);
            aoPending_ = 0;
        }
    }

    std::string takeOutputError() override {
        std::string e;
        e.swap(outErr_);
        return e;
    }

    const char* name() const override { return "NI-DAQmx"; }

private:
    bool check(int32 code, const char* what, std::string& err) {
        if (DAQmxFailed(code)) {
            err = daqError(code, what);
            return false;
        }
        return true;
    }

    bool startAI(const AppParams& p, std::string& err) {
        const auto& hw = p.hw;
        const uInt32 bufSamps = uInt32(std::max(hw.sampleRate * hw.inputBufferSec, 10.0 * hw.samplesPerFrame));
        if (!check(DAQmxCreateTask("FeedbackPlease_AI", &ai_), "DAQmxCreateTask(AI)", err) ||
            !check(DAQmxCreateAIVoltageChan(ai_, hw.aiChannel.c_str(), "", toDaqTerminal(hw.terminalConfig), hw.aiMin,
                                            hw.aiMax, DAQmx_Val_Volts, nullptr),
                   "DAQmxCreateAIVoltageChan", err))
            return false;
        // read() assumes exactly one channel (e.g. "Dev2/ai0", not "Dev2/ai0:1").
        uInt32 nChans = 0;
        DAQmxGetTaskNumChans(ai_, &nChans);
        if (nChans != 1) {
            err = "AI channel must be a single channel; '" + hw.aiChannel + "' has " + std::to_string(nChans);
            return false;
        }
        // Move samples to the host as soon as any are in the device FIFO (lowest latency).
        // Not every device supports this property, so failure is ignored.
        DAQmxSetAIDataXferReqCond(ai_, "", DAQmx_Val_OnBrdMemNotEmpty);
        return check(DAQmxCfgSampClkTiming(ai_, "", hw.sampleRate, DAQmx_Val_Rising, DAQmx_Val_ContSamps, bufSamps),
                     "DAQmxCfgSampClkTiming(AI)", err) &&
               check(DAQmxCfgInputBuffer(ai_, bufSamps), "DAQmxCfgInputBuffer", err) &&
               check(DAQmxGetSampClkRate(ai_, &fs_), "DAQmxGetSampClkRate", err);  // actual (coerced) rate
    }

    bool startDO(const AppParams& p, std::string& err) {
        const auto& hw = p.hw;
        hasFb_ = !hw.doFeedbackLine.empty();
        hasSyl_ = !hw.doSyllableLine.empty();
        hasMatch_ = !hw.doMatchLine.empty();
        hasSong_ = !hw.doSongLine.empty();
        if (!hasFb_ && !hasSyl_ && !hasMatch_ && !hasSong_) return true;
        if (!check(DAQmxCreateTask("FeedbackPlease_DO", &do_), "DAQmxCreateTask(DO)", err)) return false;
        // Channel order must match the data order in writeDigital().
        for (const std::string* line : {&hw.doFeedbackLine, &hw.doSyllableLine, &hw.doMatchLine, &hw.doSongLine}) {
            if (line->empty()) continue;
            if (!check(DAQmxCreateDOChan(do_, line->c_str(), "", DAQmx_Val_ChanForAllLines), "DAQmxCreateDOChan", err))
                return false;
            ++doCount_;
        }
        if (!check(DAQmxStartTask(do_), "DAQmxStartTask(DO)", err)) return false;
        lastDo_.feedback = !lastDo_.feedback;  // force the initial all-low write
        writeDigital(DigitalOutputs{});
        return outErr_.empty() || (err = outErr_, false);
    }

    // Syllable code port: either a task saved in NI MAX (loaded by name, like LabVIEW's
    // "P2_SyllOut") or a physical port / line range, e.g. "Dev2/port1" or "Dev2/port1/line0:7".
    bool startCode(const AppParams& p, std::string& err) {
        const std::string& spec = p.hw.sylCodeOutput;
        if (spec.empty()) return true;
        if (spec.find('/') == std::string::npos) {
            if (!check(DAQmxLoadTask(spec.c_str(), &code_), "DAQmxLoadTask (syllable code task)", err)) return false;
        } else {
            if (!check(DAQmxCreateTask("FeedbackPlease_SylCode", &code_), "DAQmxCreateTask(code)", err) ||
                !check(DAQmxCreateDOChan(code_, spec.c_str(), "", DAQmx_Val_ChanForAllLines), "DAQmxCreateDOChan(code)",
                       err))
                return false;
        }
        if (!check(DAQmxStartTask(code_), "DAQmxStartTask(syllable code)", err)) return false;
        writeSyllableCode(0);
        return outErr_.empty() || (err = outErr_, false);
    }

    bool startPulseInput(const AppParams& p, std::string& err) {
        if (!p.pulse.enabled || p.pulse.diLine.empty()) return true;
        return check(DAQmxCreateTask("FeedbackPlease_DI", &di_), "DAQmxCreateTask(DI)", err) &&
               check(DAQmxCreateDIChan(di_, p.pulse.diLine.c_str(), "", DAQmx_Val_ChanForAllLines), "DAQmxCreateDIChan(pulse input)",
                     err) &&
               check(DAQmxStartTask(di_), "DAQmxStartTask(DI)", err);
    }

    // Three waveforms share one finite AO task: wave files 1 and 2 (syllable-triggered feedback) and, as 3, the
    // burst played when a closed-loop pulse arrives. Everything is padded to the longest.
    bool startAO(const AppParams& p, std::string& err) {
        const auto& hw = p.hw;
        const std::string* paths[2] = {&p.fb.wave1, &p.fb.wave2};
        const bool pulseOn = p.pulse.enabled && !p.pulse.diLine.empty();
        const bool pulseWav = pulseOn && p.pulse.source == PulseSource::WavFile;
        if (hw.aoChannel.empty() || (paths[0]->empty() && paths[1]->empty() && !pulseOn)) return true;

        double rate = 0.0;
        size_t len = 0;
        auto setRate = [&](double r, const std::string& what) {
            if (rate != 0.0 && r != rate) {
                err = what + " must have the same sample rate as the other feedback wave files";
                return false;
            }
            rate = r;
            return true;
        };
        for (int i = 0; i < 2; ++i) {
            if (paths[i]->empty()) continue;
            WavData w;
            if (!readWav(*paths[i], w, &err)) return false;
            if (!setRate(w.sampleRate, "feedback wave files")) return false;
            waves_[i].resize(w.samples.size());
            for (size_t k = 0; k < w.samples.size(); ++k)
                waves_[i][k] = std::clamp(double(w.samples[k]) * hw.aoScale, hw.aoMin, hw.aoMax);
            len = std::max(len, waves_[i].size());
        }

        aoMin_ = hw.aoMin;
        aoMax_ = hw.aoMax;
        if (pulseOn) {
            pulse_ = p.pulse;
            if (pulseWav) {
                if (p.pulse.wavPath.empty()) {
                    err = "pulse feedback: choose a wave file or switch the source to white noise";
                    return false;
                }
                WavData w;
                if (!readWav(p.pulse.wavPath, w, &err)) return false;
                if (!setRate(w.sampleRate, "the pulse feedback wave file")) return false;
                waves_[2] = makeWaveBurst(w.samples, rate, p.pulse, hw.aoMin, hw.aoMax);
            } else {
                if (rate == 0.0) rate = p.pulse.aoRateHz;  // else: noise is made at the wave files' rate
                burstRate_ = rate;
                if (!regenerateBurst()) {
                    err = "pulse feedback: " + outErr_;
                    outErr_.clear();
                    return false;
                }
            }
            len = std::max(len, waves_[2].size());
        }
        // Pad to a common length. Trailing zeros leave the output at 0 V when playback ends.
        len += 16;
        for (auto& w : waves_)
            if (!w.empty()) w.resize(len, 0.0);
        aoLen_ = len;
        aoChannel_ = hw.aoChannel;

        auto first = [&] { return !waves_[0].empty() ? 1 : !waves_[1].empty() ? 2 : 3; };
        return check(DAQmxCreateTask("FeedbackPlease_AO", &ao_), "DAQmxCreateTask(AO)", err) &&
               check(DAQmxCreateAOVoltageChan(ao_, hw.aoChannel.c_str(), "", hw.aoMin, hw.aoMax, DAQmx_Val_Volts,
                                              nullptr),
                     "DAQmxCreateAOVoltageChan", err) &&
               check(DAQmxCfgSampClkTiming(ao_, "", rate, DAQmx_Val_Rising, DAQmx_Val_FiniteSamps, uInt64(len)),
                     "DAQmxCfgSampClkTiming(AO)", err) &&
               // Every write replaces the whole buffer from sample 0.
               check(DAQmxSetWriteRelativeTo(ao_, DAQmx_Val_FirstSample), "DAQmxSetWriteRelativeTo", err) &&
               check(DAQmxSetWriteOffset(ao_, 0), "DAQmxSetWriteOffset", err) &&
               (loadWave(first()) || (err = outErr_, false)) &&
               check(DAQmxTaskControl(ao_, DAQmx_Val_Task_Commit), "DAQmxTaskControl(commit AO)", err);
    }

    // New white-noise burst into waves_[2] (padded to the task length once the task exists).
    bool regenerateBurst() {
        if (pulse_.source != PulseSource::WhiteNoise) return true;
        std::string e;
        std::vector<double> b = makeNoiseBurst(pulse_, burstRate_, aoMin_, aoMax_, ++burstSeed_ * 2654435761u + 12345u, &e);
        if (b.empty() && !e.empty()) {
            outErr_ = e;
            return false;
        }
        if (aoLen_ > 0) b.resize(aoLen_, 0.0);
        waves_[2] = std::move(b);
        return true;
    }

    bool hasWave(int w) const { return ao_ && w >= 1 && w <= 3 && !waves_[w - 1].empty(); }

    bool aoBusy() {
        if (!aoStarted_) return false;
        bool32 done = 0;
        DAQmxIsTaskDone(ao_, &done);
        if (!done) return true;
        DAQmxStopTask(ao_);  // back to the committed state, ready to restart quickly
        aoStarted_ = false;
        return false;
    }

    bool loadWave(int w) {
        int32 written = 0;
        const int32 code = DAQmxWriteAnalogF64(ao_, int32(aoLen_), 0, 1.0, DAQmx_Val_GroupByChannel,
                                               waves_[w - 1].data(), &written, nullptr);
        if (DAQmxFailed(code)) {
            outErr_ = daqError(code, "DAQmxWriteAnalogF64");
            aoLoaded_ = 0;
            return false;
        }
        aoLoaded_ = w;
        return true;
    }

    TaskHandle ai_ = 0, do_ = 0, ao_ = 0, code_ = 0, di_ = 0;
    double fs_ = 40000.0;
    std::vector<float64> buf_;

    bool hasFb_ = false, hasSyl_ = false, hasMatch_ = false, hasSong_ = false;
    int doCount_ = 0;
    DigitalOutputs lastDo_;

    std::vector<float64> waves_[3];
    PulseFeedbackParams pulse_;
    double burstRate_ = 40000.0;
    uint32_t burstSeed_ = 0;
    bool noiseSpent_ = false;
    size_t aoLen_ = 0;
    std::string aoChannel_;
    double aoMin_ = -10.0, aoMax_ = 10.0;
    int aoLoaded_ = 0, aoPending_ = 0;
    bool aoStarted_ = false;

    std::string outErr_;
};

}  // namespace

std::unique_ptr<IOBackend> makeDaqBackend() { return std::make_unique<DaqBackend>(); }
bool daqAvailable() { return true; }

std::string describeDaqDevices() {
    char devs[2048] = {};
    if (DAQmxFailed(DAQmxGetSysDevNames(devs, sizeof devs))) return "(device query failed)";
    std::string out;
    const std::string all = devs;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(',', pos);
        if (end == std::string::npos) end = all.size();
        std::string name = all.substr(pos, end - pos);
        name.erase(0, name.find_first_not_of(' '));
        if (!name.empty()) {
            char type[256] = {};
            DAQmxGetDevProductType(name.c_str(), type, sizeof type);
            if (!out.empty()) out += ", ";
            out += name + " (" + type + ")";
        }
        pos = end + 1;
    }
    return out.empty() ? "(none)" : out;
}

}  // namespace fp

#else  // built without NI-DAQmx

namespace fp {

namespace {
class NoDaqBackend final : public IOBackend {
public:
    bool start(const AppParams&, std::string& err) override {
        err = "This build has no NI-DAQmx support. Install NI-DAQmx and rebuild, or use the simulated source.";
        return false;
    }
    void stop() override {}
    double sampleRate() const override { return 0.0; }
    bool read(float*, int, std::string& err) override {
        err = "no DAQmx";
        return false;
    }
    const char* name() const override { return "NI-DAQmx (unavailable)"; }
};
}  // namespace

std::unique_ptr<IOBackend> makeDaqBackend() { return std::make_unique<NoDaqBackend>(); }
bool daqAvailable() { return false; }
std::string describeDaqDevices() { return "(built without NI-DAQmx)"; }

}  // namespace fp

#endif

#include "core/Params.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <type_traits>

namespace fp {

namespace {

// Calls f(key, field) for every persisted field. Shared by load and save so the
// two can never drift apart.
template <class F>
void visitParams(AppParams& p, F&& f) {
    f("box_name", p.boxName);
    f("box_enabled", p.boxEnabled);
    f("hw.ai_channel", p.hw.aiChannel);
    f("hw.ao_stream_ms", p.hw.aoStreamMs);
    f("hw.terminal_config", p.hw.terminalConfig);
    f("hw.ai_min", p.hw.aiMin);
    f("hw.ai_max", p.hw.aiMax);
    f("hw.sample_rate", p.hw.sampleRate);
    f("hw.samples_per_frame", p.hw.samplesPerFrame);
    f("hw.input_buffer_sec", p.hw.inputBufferSec);
    f("hw.ao_channel", p.hw.aoChannel);
    f("hw.ao_min", p.hw.aoMin);
    f("hw.ao_max", p.hw.aoMax);
    f("hw.ao_scale", p.hw.aoScale);
    f("hw.do_feedback_line", p.hw.doFeedbackLine);
    f("hw.do_syllable_line", p.hw.doSyllableLine);
    f("hw.do_match_line", p.hw.doMatchLine);

    f("source.kind", p.source.kind);
    f("source.sim_wav_path", p.source.simWavPath);
    f("source.sim_noise_volts", p.source.simNoiseVolts);

    f("spectrum.window_ms", p.spectrum.windowMs);
    for (int i = 0; i < kBandsPerGroup; ++i) {
        const std::string inc = "spectrum.include" + std::to_string(i);
        f(inc + ".gain", p.spectrum.include[i].gain);
        f(inc + ".low_hz", p.spectrum.include[i].lowHz);
        f(inc + ".high_hz", p.spectrum.include[i].highHz);
        const std::string sub = "spectrum.subtract" + std::to_string(i);
        f(sub + ".gain", p.spectrum.subtract[i].gain);
        f(sub + ".low_hz", p.spectrum.subtract[i].lowHz);
        f(sub + ".high_hz", p.spectrum.subtract[i].highHz);
    }
    f("spectrum.combination", p.spectrum.combination);
    f("spectrum.syl_start_thresh", p.spectrum.sylStartThresh);
    f("spectrum.syl_end_thresh", p.spectrum.sylEndThresh);
    f("spectrum.min_gap_ms", p.spectrum.minGapMs);

    f("proc.mean_subtract", p.proc.meanSubtract);
    f("proc.low_cutoff", p.proc.lowCutoff);
    f("proc.low_cutoff_thresh", p.proc.lowCutoffThresh);
    f("proc.software_gain", p.proc.softwareGain);
    f("proc.gain", p.proc.gain);
    f("proc.display_db", p.proc.displayDb);

    f("fb.enabled", p.fb.enabled);
    f("fb.prob1", p.fb.prob1);
    f("fb.delay1_ms", p.fb.delay1Ms);
    f("fb.thresh1", p.fb.thresh1);
    f("fb.wave1", p.fb.wave1);
    f("fb.prob2", p.fb.prob2);
    f("fb.delay2_ms", p.fb.delay2Ms);
    f("fb.thresh2", p.fb.thresh2);
    f("fb.wave2", p.fb.wave2);
    f("fb.mode2", p.fb.mode2);
    f("fb.refractory_ms", p.fb.refractoryMs);
    f("fb.extra_delay_ms", p.fb.extraDelayMs);
    f("fb.ttl_pulse_ms", p.fb.ttlPulseMs);

    f("pulse.enabled", p.pulse.enabled);
    f("pulse.di_line", p.pulse.diLine);
    f("pulse.source", p.pulse.source);
    f("pulse.wav_path", p.pulse.wavPath);
    f("pulse.duration_ms", p.pulse.durationMs);
    f("pulse.delay_ms", p.pulse.delayMs);
    f("pulse.gain", p.pulse.gain);
    f("pulse.low_hz", p.pulse.lowHz);
    f("pulse.high_hz", p.pulse.highHz);
    f("pulse.filter_order", p.pulse.filterOrder);
    f("pulse.ao_rate_hz", p.pulse.aoRateHz);
    f("pulse.refractory_ms", p.pulse.refractoryMs);

    f("song.enabled", p.song.enabled);
    f("song.gate_feedback", p.song.gateFeedback);
    f("song.mean_subtract", p.song.meanSubtract);
    f("song.gain", p.song.gain);
    f("song.offset", p.song.offset);
    f("song.butterworth", p.song.butterworth);
    f("song.filter_order", p.song.filterOrder);
    f("song.filter_low_hz", p.song.filterLowHz);
    f("song.filter_high_hz", p.song.filterHighHz);
    f("song.window_length", p.song.windowLength);
    f("song.frequency_bins", p.song.frequencyBins);
    f("song.hop_ms", p.song.hopMs);
    f("song.non_bird_cutoff_hz", p.song.nonBirdCutoffHz);
    f("song.bird_low_hz", p.song.birdLowHz);
    f("song.bird_high_hz", p.song.birdHighHz);
    f("song.ratio_mode", p.song.ratioMode);
    f("song.ratio_thresh", p.song.ratioThresh);
    f("song.min_bird_rms", p.song.minBirdRms);
    f("song.song_duration_sec", p.song.songDurationSec);
    f("song.duration_thresh", p.song.durationThresh);
    f("song.hold_sec", p.song.holdSec);
    f("hw.do_song_line", p.hw.doSongLine);
    f("hw.syl_code_output", p.hw.sylCodeOutput);

    f("codes.enabled", p.codes.enabled);
    f("codes.hold", p.codes.hold);
    f("codes.pulse_ms", p.codes.pulseMs);
    f("codes.target", p.codes.target);
    // The count comes first so the loader can size the list before reading entries.
    int nCodes = int(p.codes.list.size());
    f("codes.count", nCodes);
    p.codes.list.resize(size_t(std::clamp(nCodes, 0, kMaxCodes)));
    for (size_t i = 0; i < p.codes.list.size(); ++i) {
        auto& c = p.codes.list[i];
        const std::string k = "codes." + std::to_string(i);
        f(k + ".name", c.name);
        f(k + ".code", c.code);
        f(k + ".enabled", c.enabled);
        f(k + ".type", c.type);
        f(k + ".mode", c.mode);
        for (auto [w, n] : {std::pair<TimeWindow*, const char*>{&c.ioi, "ioi"}, {&c.sd, "sd"}, {&c.prevIoi, "prev_ioi"}}) {
            const std::string kw = k + "." + n;
            f(kw + "_setpoint_ms", w->setpointMs);
            f(kw + "_eps_ms", w->epsMs);
            f(kw + "_min_ms", w->minMs);
            f(kw + "_max_ms", w->maxMs);
        }
    }

    f("tmpl.method", p.tmpl.method);
    f("tmpl.window_ms", p.tmpl.windowMs);
    f("tmpl.low_hz", p.tmpl.lowHz);
    f("tmpl.high_hz", p.tmpl.highHz);
    f("tmpl.band_hz", p.tmpl.bandHz);
    f("tmpl.min_level_db", p.tmpl.minLevelDb);
    f("tmpl.rearm_ms", p.tmpl.rearmMs);
    f("tmpl.target", p.tmpl.target);
    int nTmpl = int(p.tmpl.list.size());
    f("tmpl.count", nTmpl);
    p.tmpl.list.resize(size_t(std::clamp(nTmpl, 0, kMaxTemplates)));
    for (size_t i = 0; i < p.tmpl.list.size(); ++i) {
        auto& t = p.tmpl.list[i];
        const std::string k = "tmpl." + std::to_string(i);
        f(k + ".name", t.name);
        f(k + ".code", t.code);
        f(k + ".enabled", t.enabled);
        f(k + ".thresh", t.thresh);
        f(k + ".min_frames", t.minFrames);
        f(k + ".gate_index", t.gateIndex);
        f(k + ".gate_min_ms", t.gateMinMs);
        f(k + ".gate_max_ms", t.gateMaxMs);
        int nEx = int(t.examples.size());
        f(k + ".example_count", nEx);
        t.examples.resize(size_t(std::clamp(nEx, 0, 1000)));
        for (size_t j = 0; j < t.examples.size(); ++j) {
            const std::string ke = k + ".example." + std::to_string(j);
            f(ke + ".file", t.examples[j].file);
            f(ke + ".ms", t.examples[j].ms);
        }
        // The shape as one line of numbers (written as text, parsed back on load).
        std::string shape;
        for (float v : t.shape) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%s%.6g", shape.empty() ? "" : " ", v);
            shape += buf;
        }
        f(k + ".shape", shape);
        std::istringstream is(shape);
        t.shape.clear();
        for (float v; is >> v;) t.shape.push_back(v);
    }

    f("display.window_sec", p.display.windowSec);
    f("display.raw_auto_y", p.display.rawAutoY);
    f("display.raw_y_range", p.display.rawYRange);
    f("display.env_auto_y", p.display.envAutoY);

    f("rec.output_dir", p.rec.outputDir);
    f("rec.save_events", p.rec.saveEvents);
    f("rec.save_envelope", p.rec.saveEnvelope);
    f("rec.song_files", p.rec.songFiles);
    f("rec.pre_roll_sec", p.rec.preRollSec);
    f("rec.post_roll_sec", p.rec.postRollSec);
    f("rec.ch_mic", p.rec.chMic);
    f("rec.ch_detection", p.rec.chDetection);
    f("rec.ch_markers", p.rec.chMarkers);
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

struct Saver {
    std::ostream& os;
    std::string pre;  // key prefix, e.g. "box2." or "tune."
    void operator()(const std::string& k, std::string& v) { os << pre << k << " = " << v << "\n"; }
    void operator()(const std::string& k, double& v) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.10g", v);
        os << pre << k << " = " << buf << "\n";
    }
    void operator()(const std::string& k, int& v) { os << pre << k << " = " << v << "\n"; }
    void operator()(const std::string& k, bool& v) { os << pre << k << " = " << (v ? 1 : 0) << "\n"; }
    template <class E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
    void operator()(const std::string& k, E& v) {
        os << pre << k << " = " << static_cast<int>(v) << "  # " << toString(v) << "\n";
    }
};

struct Loader {
    const std::map<std::string, std::string>& kv;
    std::string& err;
    std::string pre;
    const std::string* find(const std::string& k) const {
        auto it = kv.find(pre + k);
        return it == kv.end() ? nullptr : &it->second;
    }
    void fail(const std::string& k) { err += "bad value for " + pre + k + "; "; }
    void operator()(const std::string& k, std::string& v) {
        if (auto s = find(k)) v = *s;
    }
    void operator()(const std::string& k, double& v) {
        if (auto s = find(k)) {
            try { v = std::stod(*s); } catch (...) { fail(k); }
        }
    }
    void operator()(const std::string& k, int& v) {
        if (auto s = find(k)) {
            try { v = std::stoi(*s); } catch (...) { fail(k); }
        }
    }
    void operator()(const std::string& k, bool& v) {
        if (auto s = find(k)) v = (*s == "1" || *s == "true" || *s == "TRUE");
    }
    template <class E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
    void operator()(const std::string& k, E& v) {
        if (auto s = find(k)) {
            try { v = static_cast<E>(std::stoi(*s)); } catch (...) { fail(k); }
        }
    }
};

}  // namespace

bool readParamFile(const std::string& path, ParamMap& kv, std::string* err) {
    std::ifstream in(std::filesystem::u8path(path));
    if (!in) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    kv.clear();
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = line.substr(eq + 1);
        // Strip the "  # comment" the saver appends to enum values. Only that exact form
        // (two spaces + '#'), so names and paths can contain '#' ("noise #1.wav").
        const auto hash = val.find("  #");
        if (hash != std::string::npos) val = val.substr(0, hash);
        kv[key] = trim(val);
    }
    return true;
}

bool hasSection(const ParamMap& kv, const std::string& prefix) {
    const auto it = kv.lower_bound(prefix);
    return it != kv.end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

std::string readParams(const ParamMap& kv, AppParams& p, const std::string& prefix) {
    std::string localErr;
    visitParams(p, Loader{kv, localErr, prefix});

    // Settings saved before syllables existed had a single target ("seq.*" keys):
    // turn it into a syllable called "target" and select it.
    if (!kv.count(prefix + "codes.count") && kv.count(prefix + "seq.trigger_type")) {
        SyllableCode c = defaultTargetSyllable();
        auto num = [&](const char* k, double& v) {
            if (auto it = kv.find(prefix + k); it != kv.end()) {
                try { v = std::stod(it->second); } catch (...) {}
            }
        };
        double type = double(int(c.type));
        num("seq.trigger_type", type);
        c.type = static_cast<TriggerType>(int(type));
        if (!isOnsetEvaluated(c.type)) c.type = TriggerType::SD_IOI;  // SD / IOI,SD were decided at offsets
        num("seq.ioi_setpoint_ms", c.ioi.setpointMs);
        num("seq.ioi_eps_ms", c.ioi.epsMs);
        num("seq.sd_setpoint_ms", c.sd.setpointMs);
        num("seq.sd_eps_ms", c.sd.epsMs);
        num("seq.prev_ioi_setpoint_ms", c.prevIoi.setpointMs);
        num("seq.prev_ioi_eps_ms", c.prevIoi.epsMs);
        for (TimeWindow* w : {&c.ioi, &c.sd, &c.prevIoi}) w->convertTo(RangeMode::MinMax);  // fill min/max
        p.codes.list = {c};
        p.codes.target = 0;
    }
    if (p.codes.target >= int(p.codes.list.size())) p.codes.target = -1;
    if (p.tmpl.target >= int(p.tmpl.list.size())) p.tmpl.target = -1;
    return localErr;
}

bool loadParams(const std::string& path, AppParams& p, std::string* err) {
    ParamMap kv;
    if (!readParamFile(path, kv, err)) return false;
    const std::string e = readParams(kv, p);
    if (err) *err = e;
    return e.empty();
}

void writeParams(std::ostream& os, const AppParams& p, const std::string& prefix) {
    AppParams copy = p;
    visitParams(copy, Saver{os, prefix});
}

int templateBands(const TemplateParams& t) {
    if (t.bandHz <= 0 || t.highHz <= t.lowHz) return 0;
    return std::clamp(int(std::ceil((t.highHz - t.lowHz) / t.bandHz - 1e-9)), 1, 1024);
}

SyllableCode defaultTargetSyllable() {
    SyllableCode c;
    c.name = "target";
    c.code = 1;
    c.type = TriggerType::IOI_SD_IOI;
    c.ioi = {124.0, 10.0, 114.0, 134.0};
    c.sd = {95.0, 10.0, 85.0, 105.0};
    c.prevIoi = {95.0, 10.0, 85.0, 105.0};
    return c;
}

const SyllableCode* targetSyllable(const SyllableCodeParams& p) {
    return p.target >= 0 && p.target < int(p.list.size()) ? &p.list[size_t(p.target)] : nullptr;
}

bool saveParams(const std::string& path, const AppParams& p, std::string* err) {
    std::ofstream out(path);
    if (!out) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    out << "# FeedbackPlease settings\n";
    writeParams(out, p);
    return true;
}

const char* toString(BandCombination c) {
    switch (c) {
        case BandCombination::Difference: return "Difference";
        case BandCombination::Sum: return "Sum";
        case BandCombination::Ratio: return "Ratio";
        case BandCombination::IncludeOnly: return "Include only";
    }
    return "?";
}

const char* toString(TriggerType t) {
    switch (t) {
        case TriggerType::IOI: return "IOI";
        case TriggerType::SD: return "SD";
        case TriggerType::IOI_SD: return "IOI, SD";
        case TriggerType::SD_IOI: return "SD, IOI";
        case TriggerType::IOI_SD_IOI: return "IOI, SD, IOI";
    }
    return "?";
}

const char* toString(Feedback2Mode m) {
    switch (m) {
        case Feedback2Mode::Alternative: return "Alternative to feedback 1";
        case Feedback2Mode::AfterFeedback1: return "Follows feedback 1";
    }
    return "?";
}

const char* toString(PulseSource s) {
    switch (s) {
        case PulseSource::WhiteNoise: return "White noise";
        case PulseSource::WavFile: return "Wave file";
    }
    return "?";
}

const char* toString(SourceKind k) {
    switch (k) {
        case SourceKind::Daq: return "NI-DAQmx";
        case SourceKind::Simulated: return "Simulated (WAV / synthetic)";
    }
    return "?";
}

bool TimeWindow::contains(double v, RangeMode m) const {
    if (m == RangeMode::MinMax) return v >= minMs && v <= maxMs;
    return std::fabs(v - setpointMs) <= epsMs;
}

void TimeWindow::convertTo(RangeMode to) {
    if (to == RangeMode::MinMax) {
        minMs = setpointMs - epsMs;
        maxMs = setpointMs + epsMs;
    } else {
        setpointMs = 0.5 * (minMs + maxMs);
        epsMs = 0.5 * std::fabs(maxMs - minMs);
    }
}

bool usesIoi(TriggerType t) { return t != TriggerType::SD; }
bool usesSd(TriggerType t) { return t != TriggerType::IOI; }
bool usesPrevIoi(TriggerType t) { return t == TriggerType::IOI_SD_IOI; }
bool isOnsetEvaluated(TriggerType t) {
    return t == TriggerType::IOI || t == TriggerType::SD_IOI || t == TriggerType::IOI_SD_IOI;
}

const char* toString(DetectMethod m) {
    switch (m) {
        case DetectMethod::Sequence: return "Syllable sequence (IOI / SD)";
        case DetectMethod::Template: return "Spectral templates";
    }
    return "?";
}

const char* toString(RangeMode m) {
    switch (m) {
        case RangeMode::SetpointEps: return "Setpoint +/- eps";
        case RangeMode::MinMax: return "Min / max";
    }
    return "?";
}

const char* toString(SylCodeHold h) {
    switch (h) {
        case SylCodeHold::UntilOffset: return "Until syllable ends";
        case SylCodeHold::UntilNextOnset: return "Until next onset";
        case SylCodeHold::Pulse: return "Pulse";
    }
    return "?";
}

const char* toString(SongRatioMode m) {
    switch (m) {
        case SongRatioMode::BirdOverLow: return "Bird / Non-bird";
        case SongRatioMode::BirdOverTotal: return "Bird / (Bird + Non-bird)";
    }
    return "?";
}

const char* toString(TerminalConfig t) {
    switch (t) {
        case TerminalConfig::Default: return "Default";
        case TerminalConfig::RSE: return "RSE";
        case TerminalConfig::NRSE: return "NRSE";
        case TerminalConfig::Differential: return "Differential";
        case TerminalConfig::PseudoDiff: return "Pseudodifferential";
    }
    return "?";
}

}  // namespace fp

#pragma once
// All user-tunable settings. Field names mirror the LabVIEW front panel so the
// two can be compared side by side; defaults are the values from that panel.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace fp {

// One row of the "Spectrum Selection" band table: Gain, High Freq, Low Freq.
struct Band {
    double gain = 0.0;  // linear weight applied to the band amplitude; 0 disables the band
    double lowHz = 0.0;
    double highHz = 0.0;
};

enum class BandCombination : int { Difference = 0, Sum = 1, Ratio = 2, IncludeOnly = 3 };
enum class TriggerType : int { IOI = 0, SD = 1, IOI_SD = 2, SD_IOI = 3, IOI_SD_IOI = 4 };
// How "Feedback 2" relates to "Feedback 1" (the LabVIEW panel is ambiguous, see README).
enum class Feedback2Mode : int { Alternative = 0, AfterFeedback1 = 1 };
enum class SourceKind : int { Daq = 0, Simulated = 1 };
// DAQmx terminal configuration (index into the GUI combo; mapped in DaqBackend).
enum class TerminalConfig : int { Default = 0, RSE = 1, NRSE = 2, Differential = 3, PseudoDiff = 4 };

constexpr int kBandsPerGroup = 4;

struct HardwareParams {
    std::string aiChannel = "Dev1/ai0";
    TerminalConfig terminalConfig = TerminalConfig::Default;
    double aiMin = -10.0;
    double aiMax = 10.0;
    double sampleRate = 40000.0;
    int samplesPerFrame = 40;     // 40 samples @ 40 kHz = 1 ms detection update
    double aoStreamMs = 5.0;      // multi-box: AO stream buffer = feedback output latency (ms)
    double inputBufferSec = 2.0;  // DAQmx host buffer; only matters if we fall behind

    std::string aoChannel = "Dev1/ao0";  // feedback waveform output; empty = no AO
    double aoMin = -10.0;
    double aoMax = 10.0;
    double aoScale = 1.0;  // volts output for a full-scale (+/-1.0) sample in the wave file

    // Digital lines; any may be empty to disable it.
    std::string doFeedbackLine = "Dev1/port0/line0";  // pulses when feedback is delivered
    std::string doSyllableLine = "Dev1/port0/line1";  // high while a syllable is detected
    std::string doMatchLine = "Dev1/port0/line2";     // pulses when the target sequence matches
    std::string doSongLine;                           // high while song is detected (off by default)
    // Syllable code output (U8): an NI MAX task name (e.g. "P2_SyllOut") or a port /
    // line spec (e.g. "Dev2/port1"). Empty = off.
    std::string sylCodeOutput;
};

// Feedback triggered by a digital pulse on an input line (the closed loop raises one when its spike count
// crosses threshold). Plays a bandpassed white-noise burst (or a wave file) on the AO feedback channel.
enum class PulseSource : int { WhiteNoise = 0, WavFile = 1 };

struct PulseFeedbackParams {
    bool enabled = false;
    std::string diLine;  // digital input carrying the pulse, e.g. Dev1/port0/line4; rising edge = trigger
    PulseSource source = PulseSource::WhiteNoise;
    std::string wavPath;        // for PulseSource::WavFile
    double durationMs = 100.0;  // "Feedback Duration (ms)"
    double delayMs = 0.0;       // "Feedback Delay (ms)": silence before the burst
    double gain = 1.0;          // "Feedback Amplitude gain": V of unfiltered unit-variance noise (V per full-scale for a wave)
    double lowHz = 1500.0;      // bandpass edges, Butterworth
    double highHz = 8000.0;
    int filterOrder = 10;
    double aoRateHz = 40000.0;  // AO sample clock when no wave file sets it
    double refractoryMs = 200.0;  // ignore pulses this long after one that played
};

struct SourceParams {
    SourceKind kind = SourceKind::Daq;
    std::string simWavPath;  // empty = built-in synthetic song
    double simNoiseVolts = 0.0005;
};

// "Spectrum Selection" cluster.
struct SpectrumParams {
    double windowMs = 15.0;  // "# previous ms included in calcs"
    // Green rows on the panel: bands that are summed into the detection signal.
    std::array<Band, kBandsPerGroup> include{{{0, 1500, 2500}, {20, 1000, 7000}, {0, 0, 0}, {0, 0, 0}}};
    // Red rows on the panel: bands combined against the green ones (noise bands).
    std::array<Band, kBandsPerGroup> subtract{{{0, 500, 1500}, {1, 2, 1000}, {0, 0, 0}, {0, 0, 0}}};
    BandCombination combination = BandCombination::Difference;
    double sylStartThresh = 0.069296;
    double sylEndThresh = 0.0274191;
    double minGapMs = 10.0;  // gaps shorter than this are bridged: the syllables on both sides count as one
};

// "Processing Params" cluster.
struct ProcessingParams {
    bool meanSubtract = true;
    bool lowCutoff = false;
    double lowCutoffThresh = 1e-7;
    bool softwareGain = true;
    double gain = 3.0;
    bool displayDb = true;  // display only; detection always uses the linear value
};

// Feedback state machine part of "Feedback Params". What triggers it is the target
// syllable, chosen in SyllableCodeParams::target.
struct FeedbackParams {
    double prob1 = 1.0;
    double delay1Ms = 20.0;
    double thresh1 = 0.0001;  // "Detect Signal Thresh. 1": envelope must still be >= this when sending
    std::string wave1;

    double prob2 = 0.0;
    double delay2Ms = -1.0;  // -1 = unused
    double thresh2 = 0.0;
    std::string wave2;

    Feedback2Mode mode2 = Feedback2Mode::Alternative;
    double refractoryMs = 100.0;
    double extraDelayMs = 0.0;  // "Feedback Delay (ms)", added to Delay 1/2
    double ttlPulseMs = 5.0;    // width of the feedback / match TTL pulses
    bool enabled = true;        // master switch; detection still runs when off
};

enum class SongRatioMode : int { BirdOverLow = 0, BirdOverTotal = 1 };

// DetectBirdSong.vi: "Song Detect 5 Params" + "Pre-Processing Params".
// Separates song from calls and cage noise; when gateFeedback is on, sequence
// matches only start feedback trials while song is detected.
struct SongParams {
    bool enabled = true;
    bool gateFeedback = true;

    // Pre-processing (applied to a copy of the signal used only by the song detector)
    bool meanSubtract = true;
    double gain = 1.0;
    double offset = 0.0;
    bool butterworth = false;
    int filterOrder = 6;
    double filterLowHz = 3000.0;
    double filterHighHz = 10000.0;

    // Spectrogram
    int windowLength = 2048;  // "window info: length" (Hanning)
    int frequencyBins = 512;  // FFT size = max(window length, 2 * bins), rounded up to a power of 2
    double hopMs = 5.0;       // time between spectrogram columns (replaces "time steps")

    // Decision
    double nonBirdCutoffHz = 1000.0;
    double birdLowHz = 3000.0;
    double birdHighHz = 10000.0;
    SongRatioMode ratioMode = SongRatioMode::BirdOverLow;
    double ratioThresh = 0.1;
    double minBirdRms = 0.0;      // optional level gate (V RMS in the bird band); 0 = off
    double songDurationSec = 0.5; // length of the box kernel
    double durationThresh = 0.25; // fraction of the kernel that must be above the ratio threshold
    double holdSec = 0.0;         // keep "song" on this long after the detector drops
};

// ---- Syllables ("All Sylls Dig Code") and the feedback target
// Each syllable has a trigger type saying which of IOI / SD / previous IOI it uses
// (as in CalcDetectSignal.vi), and a window for each. At every IOI end (syllable
// onset) the list is checked in order and the first match is reported. With onsets
// A, B, C, at C's onset: IOI = C-B, SD = duration of B, previous IOI = B-A. Only the
// onset-evaluated trigger types are used: IOI, "SD, IOI" and "IOI, SD, IOI".
// Feedback fires when the first match is the target syllable.
enum class SylCodeHold : int { UntilOffset = 0, UntilNextOnset = 1, Pulse = 2 };
enum class RangeMode : int { SetpointEps = 0, MinMax = 1 };
constexpr int kMaxCodes = 64;

struct TimeWindow {
    double setpointMs = 100.0;
    double epsMs = 10.0;
    double minMs = 90.0;
    double maxMs = 110.0;
    bool contains(double v, RangeMode m) const;  // false for NaN
    void convertTo(RangeMode to);                 // keep the same window when switching mode
};

struct SyllableCode {
    std::string name = "syllable";
    int code = 1;  // value written to the port, 1..255 (0 = no match)
    bool enabled = true;
    TriggerType type = TriggerType::SD_IOI;
    RangeMode mode = RangeMode::SetpointEps;
    TimeWindow ioi, sd, prevIoi;
};
bool usesIoi(TriggerType t);
bool usesSd(TriggerType t);
bool usesPrevIoi(TriggerType t);
bool isOnsetEvaluated(TriggerType t);  // IOI, SD_IOI, IOI_SD_IOI

// The LabVIEW panel's target: "IOI, SD, IOI" with 124 / 95 / 95 ms +/- 10.
SyllableCode defaultTargetSyllable();

struct SyllableCodeParams {
    bool enabled = true;  // write codes to the syllable code port (classification always runs)
    SylCodeHold hold = SylCodeHold::UntilOffset;
    double pulseMs = 5.0;  // for SylCodeHold::Pulse
    std::vector<SyllableCode> list{defaultTargetSyllable()};
    int target = 0;  // index into list that triggers feedback; -1 = none
};
// The target syllable, or nullptr if none / out of range.
const SyllableCode* targetSyllable(const SyllableCodeParams& p);

// ---- Spectral templates (alternative to IOI / SD sequences)
// Every frame, the spectrum of the last windowMs is reduced to log power in bands of
// bandHz between lowHz and highHz, normalized (mean removed, unit length), and
// correlated with each template: 1 = same spectral shape, regardless of loudness.
// A template is detected when its correlation stays >= thresh for minFrames frames in
// a row while the level is above minLevelDb. Like EvTAF (Tumer & Brainard 2007).
enum class DetectMethod : int { Sequence = 0, Template = 1 };
constexpr int kMaxTemplates = 16;

struct TemplateExample {
    std::string file;  // recording the template was taken from
    double ms = 0.0;   // end of the analysis window in that file
};

// How well a template separates its labeled examples from everything else in the files
// they come from (computed when the template is rebuilt; not saved).
struct TemplateSeparation {
    std::vector<float> pos, neg;  // scores of example frames / of all other loud frames
    double dPrime = 0.0;          // (mean pos - mean neg) / pooled SD
    double suggested = 0.0;       // threshold: midway in the gap if they separate, else best balance
    double minPos = 0.0, maxNeg = 0.0;
    int negAbove = 0, posBelow = 0;  // at the suggested threshold
};
struct TemplateStats {
    int files = 0, positives = 0, negatives = 0;
    TemplateSeparation plain, disc;
    std::string note;  // why discriminative training failed, if it did
};

struct SpectralTemplate {
    std::string name = "template";
    int code = 1;  // value written to the syllable code port
    bool enabled = true;
    double thresh = 0.9;   // score needed (correlation, or ~0..1 when discriminative)
    int minFrames = 3;     // consecutive frames at or above thresh
    // Optional timing gate: only counts if template gateIndex was detected between
    // gateMinMs and gateMaxMs earlier. -1 = no gate.
    int gateIndex = -1;
    double gateMinMs = 50.0, gateMaxMs = 150.0;
    std::vector<TemplateExample> examples;  // averaged into shape
    std::vector<float> shape;               // normalized band log-powers (see above)
    // Discriminative: score = weights . features + bias, learned from the labeled files (all
    // renditions of this syllable in them must be examples) so that the examples score ~1
    // and every other sound in those files ~0. Off = plain correlation with shape.
    bool discriminative = false;
    std::vector<float> weights;
    double bias = 0.0;
    std::shared_ptr<const TemplateStats> stats;  // not saved
};

struct TemplateParams {
    DetectMethod method = DetectMethod::Sequence;
    double windowMs = 12.0;
    double lowHz = 500.0, highHz = 10000.0, bandHz = 125.0;
    double minLevelDb = -50.0;  // RMS between lowHz and highHz, dB re 1 V (before software gain)
    double rearmMs = 30.0;      // a template can't be detected again sooner than this
    std::vector<SpectralTemplate> list;
    int target = 0;  // index into list that triggers feedback; -1 = none
};
int templateBands(const TemplateParams& t);  // number of bands (length of a shape)

struct DisplayParams {
    double windowSec = 2.0;
    bool rawAutoY = true;
    double rawYRange = 1.0;
    bool envAutoY = true;
};

struct RecordingParams {
    std::string outputDir = "recordings";
    bool saveEvents = true;    // CSV of onsets/offsets/matches/feedback
    bool saveEnvelope = false; // continuous detection signal WAV at the frame rate

    // Song-triggered WAVs: each time the song detector turns on, a new file starts with
    // the last preRollSec of audio and continues until song has been off for postRollSec.
    bool songFiles = true;
    double preRollSec = 2.0;
    double postRollSec = 2.0;
    bool chMic = true;        // raw microphone (V)
    bool chDetection = true;  // detection signal (linear), held over each frame
    bool chMarkers = true;    // 0.1 song + 0.2 syllable + 0.3 target match + 0.4 feedback (the TTL lines)
};

struct AppParams {
    std::string boxName = "Box 1";  // used in the multi-box view and in recording file names
    bool boxEnabled = true;         // multi-box: included when "Start all" is pressed
    HardwareParams hw;
    SourceParams source;
    SpectrumParams spectrum;
    ProcessingParams proc;
    FeedbackParams fb;
    SongParams song;
    SyllableCodeParams codes;
    TemplateParams tmpl;
    PulseFeedbackParams pulse;
    DisplayParams display;
    RecordingParams rec;
};

// Simple "key = value" text format. Unknown keys are ignored and missing keys keep
// their current values, so old settings files keep loading after fields are added.
bool loadParams(const std::string& path, AppParams& p, std::string* err = nullptr);
bool saveParams(const std::string& path, const AppParams& p, std::string* err = nullptr);

// Several parameter sets in one file, distinguished by a key prefix ("tune.", "box2.").
// readParams returns an error description, empty on success.
using ParamMap = std::map<std::string, std::string>;
bool readParamFile(const std::string& path, ParamMap& kv, std::string* err = nullptr);
bool hasSection(const ParamMap& kv, const std::string& prefix);
std::string readParams(const ParamMap& kv, AppParams& p, const std::string& prefix = "");
void writeParams(std::ostream& os, const AppParams& p, const std::string& prefix = "");

const char* toString(BandCombination c);
const char* toString(TriggerType t);
const char* toString(Feedback2Mode m);
const char* toString(SourceKind k);
const char* toString(PulseSource s);
const char* toString(TerminalConfig t);
const char* toString(SongRatioMode m);
const char* toString(SylCodeHold h);
const char* toString(RangeMode m);
const char* toString(DetectMethod m);

}  // namespace fp

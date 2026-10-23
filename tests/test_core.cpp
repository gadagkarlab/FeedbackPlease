// Minimal self-checking tests for the detection core (no framework needed).
// Run: build/Release/fp_tests.exe   (or `ctest -C Release`)

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "core/BandEnvelope.h"
#include "core/Detector.h"
#include "core/FeedbackStateMachine.h"
#include "core/Filters.h"
#include "core/NoiseBurst.h"
#include "core/Offline.h"
#include "core/WavFile.h"
#include "core/Params.h"
#include "core/SongDetector.h"
#include "core/SongRecorder.h"
#include "core/SyllableCodes.h"
#include "core/SynthSong.h"
#include "core/Syllables.h"

using namespace fp;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static void testBandEnvelopeSine() {
    const double fs = 40000.0;
    const int n = 600;
    SpectrumParams sp;
    sp.include = {{{1.0, 1000, 7000}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};
    sp.subtract = {{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};
    sp.combination = BandCombination::IncludeOnly;
    ProcessingParams pp;
    pp.softwareGain = false;
    pp.meanSubtract = true;

    BandEnvelope env;
    env.configure(fs, n, sp, pp);
    std::vector<float> x(n);

    // A 0.5 V sine in band should read as its RMS, 0.354 V.
    for (int i = 0; i < n; ++i) x[i] = float(0.5 * std::sin(2 * 3.14159265358979 * 3000.0 * i / fs) + 0.3);  // + DC
    const double inBand = env.compute(x.data()).value;
    std::printf("  3 kHz sine RMS in band: %.4f (expect ~0.354)\n", inBand);
    CHECK(std::fabs(inBand - 0.3536) < 0.02);

    // Out of band (300 Hz) should be mostly rejected.
    for (int i = 0; i < n; ++i) x[i] = float(0.5 * std::sin(2 * 3.14159265358979 * 300.0 * i / fs));
    const double outBand = env.compute(x.data()).value;
    std::printf("  300 Hz sine in 1-7 kHz band: %.4f (expect ~0)\n", outBand);
    CHECK(outBand < 0.01);
}

static void testSegmenterAndMatcher() {
    SyllableSegmenter seg;
    SyllableClassifier m;
    SyllableCodeParams sp;  // default: the "target" syllable, IOI,SD,IOI with 124 / 95 / 95 +-10
    // Onsets at 0, 95, 219 ms. Syllable starting at 95 lasts 95 ms.
    struct Ev { double t; double v; };
    std::vector<Ev> evs = {{0, 1}, {40, 0}, {95, 1}, {190, 0}, {219, 1}};
    int matches = 0, evaluations = 0;
    for (const auto& e : evs) {
        auto o = seg.update(e.v, e.t, 0.5, 0.2);
        auto r = m.update(o, sp);
        evaluations += r.evaluated;
        matches += r.index == sp.target;
    }
    CHECK(evaluations == 2);  // at the 2nd and 3rd onsets
    CHECK(matches == 1);
    CHECK(std::fabs(m.ioi() - 124) < 1e-9);
    CHECK(std::fabs(m.prevIoi() - 95) < 1e-9);
    CHECK(std::fabs(m.sd() - 95) < 1e-9);

    // Minimum gap (10 ms): a 6 ms gap is bridged, a real gap ends the syllable at the drop.
    {
        SyllableSegmenter g;
        int onsets = 0, offsets = 0;
        SegmenterOutput last;
        auto step = [&](double v, double t) {
            last = g.update(v, t, 0.5, 0.2, 10.0);
            onsets += last.onset;
            offsets += last.offset;
            return last;
        };
        step(1.0, 0);                            // onset
        CHECK(!step(0.1, 50).offset);            // drop: maybe a gap
        CHECK(step(0.1, 50).inSyllable);         // still one syllable during the possible gap
        CHECK(!step(1.0, 56).onset);             // back after 6 ms: bridged, no new onset
        CHECK(!step(0.1, 120).offset);           // real end at 120 ...
        CHECK(!step(0.1, 125).offset);
        const auto off = step(0.1, 130);         // ... confirmed 10 ms later
        CHECK(off.offset && off.offsetMs == 120 && off.sdMs == 120 && !off.inSyllable);
        const auto on2 = step(1.0, 200);
        CHECK(on2.onset && on2.newIOI && on2.ioiMs == 200);  // IOI across the bridged gap
        step(0.1, 250);                          // drop ...
        CHECK(step(0.1, 260).offset);            // ... a gap of exactly 10 ms is real
        CHECK(step(1.0, 261).onset);
        CHECK(onsets == 3 && offsets == 2);

        SyllableSegmenter z;  // min gap 0 = the old behaviour: the drop is the offset
        z.update(1.0, 0, 0.5, 0.2, 0.0);
        const auto o = z.update(0.1, 50, 0.5, 0.2, 0.0);
        CHECK(o.offset && o.offsetMs == 50 && o.sdMs == 50);
    }

    // Hysteresis: dipping between the thresholds does not end a syllable.
    SyllableSegmenter s2;
    CHECK(s2.update(1.0, 0, 0.5, 0.2).onset);
    CHECK(!s2.update(0.3, 1, 0.5, 0.2).offset);
    CHECK(s2.update(0.1, 2, 0.5, 0.2).offset);
}

static void testStateMachine() {
    FeedbackParams p;  // prob1 = 1, delay1 = 20 ms, refractory = 100 ms
    FeedbackStateMachine fsm(7);
    int fireFrame = -1, armFrame = -1;
    for (int f = 0; f < 300; ++f) {
        const bool detect = (f == 10) || (f == 50);  // second detect falls inside the trial and is ignored
        auto o = fsm.update(detect, 1.0, 1.0, p);
        if (o.arm == 1) armFrame = f;
        if (o.fire == 1) {
            CHECK(fireFrame < 0);
            fireFrame = f;
        }
    }
    CHECK(armFrame == 10);
    CHECK(fireFrame == 30);
    CHECK(fsm.state() == FbState::Listening);

    // Amplitude check at send time.
    FeedbackStateMachine fsm2(7);
    p.delay1Ms = 0;
    p.thresh1 = 0.5;
    auto o = fsm2.update(true, 0.1, 1.0, p);
    CHECK(o.fire == 0 && o.suppressed == 1);

    // Turning feedback off during the delay cancels the pending delivery.
    {
        FeedbackParams q;  // delay 20 ms
        FeedbackStateMachine f(7);
        f.update(true, 1.0, 1.0, q);  // trial starts, now in Delay 1
        q.enabled = false;
        int fired = 0;
        for (int i = 0; i < 50; ++i) fired += f.update(false, 1.0, 1.0, q).fire != 0;
        CHECK(fired == 0 && f.state() == FbState::Listening);
    }

    // Probability 0 gives a catch trial.
    FeedbackStateMachine fsm3(7);
    p.prob1 = 0;
    o = fsm3.update(true, 1.0, 1.0, p);
    CHECK(o.trialStarted && o.catchTrial && o.fire == 0);
}

static void testEndToEndSynthetic() {
    AppParams p;  // defaults from the LabVIEW panel
    const double fs = p.hw.sampleRate;
    const auto song = synthesizeSong(fs, 10.0, p.source.simNoiseVolts);
    Detector det(3);
    det.setParams(p);
    const int n = p.hw.samplesPerFrame;
    int onsets = 0, matches = 0, fires = 0;
    double firstFireMs = -1;
    for (size_t i = 0; i + n <= song.size(); i += n) {
        auto r = det.process(&song[i], n);
        if (r.seg.onset) ++onsets;
        if (r.seg.newSD && onsets <= 5) std::printf("  SD %.0f ms\n", r.seg.sdMs);
        if (r.seg.newIOI && onsets <= 5) std::printf("  IOI %.0f ms\n", r.seg.ioiMs);
        matches += r.match.match;
        if (r.fb.fire) {
            ++fires;
            if (firstFireMs < 0) firstFireMs = r.tMs;
        }
    }
    std::printf("  synthetic 10 s: %d onsets, %d matches, %d feedbacks, first at %.0f ms\n", onsets, matches, fires,
                firstFireMs);
    const int motifs = 10000 / (90 + 95 + 124 + 150 + 130 + 600);  // ~8 complete motifs
    CHECK(onsets >= 5 * motifs);
    CHECK(matches >= motifs);
    CHECK(matches <= motifs + 1);  // exactly one target per motif
    CHECK(fires == matches);
}

static void testButterworth() {
    ButterworthBandpass bp;
    bp.design(40000.0, 3000.0, 10000.0, 6);
    const double pass = bp.magnitude(5477.0), edgeLo = bp.magnitude(3000.0), edgeHi = bp.magnitude(10000.0);
    const double stop = bp.magnitude(1000.0);
    std::printf("  |H| centre %.3f, 3 kHz %.3f, 10 kHz %.3f, 1 kHz %.2e\n", pass, edgeLo, edgeHi, stop);
    CHECK(std::fabs(pass - 1.0) < 0.01);
    CHECK(std::fabs(edgeLo - 0.7071) < 0.02 && std::fabs(edgeHi - 0.7071) < 0.02);  // -3 dB edges
    CHECK(stop < 1e-3);

    // Streaming a tone through it: in-band passes, out-of-band is removed.
    auto rms = [&](double f) {
        ButterworthBandpass b = bp;
        b.reset();
        std::vector<float> x(8000);
        for (size_t i = 0; i < x.size(); ++i) x[i] = float(std::sin(2 * 3.14159265358979 * f * i / 40000.0));
        b.process(x.data(), int(x.size()));
        double s = 0;
        for (size_t i = 4000; i < x.size(); ++i) s += x[i] * x[i];
        return std::sqrt(s / 4000);
    };
    CHECK(std::fabs(rms(6000) - 0.7071) < 0.02);
    CHECK(rms(500) < 1e-3);
}

static void testSongDetector() {
    const double fs = 40000.0;
    AppParams p;
    auto run = [&](const std::vector<float>& x, double* maxFrac) {
        SongDetector sd;
        sd.configure(fs, p.song);
        int activeFrames = 0;
        *maxFrac = 0;
        for (size_t i = 0; i + 40 <= x.size(); i += 40) {
            const auto& s = sd.process(&x[i], 40);
            activeFrames += s.active;
            *maxFrac = std::max(*maxFrac, s.fraction);
        }
        return activeFrames;
    };
    double frac;

    // Song: detected for a good part of the time.
    const auto song = synthesizeSong(fs, 5.0, 0.0005);
    const int songFrames = run(song, &frac);
    std::printf("  song: active %d of %d ms, max fraction %.2f\n", songFrames, int(song.size() / 40), frac);
    CHECK(songFrames > 1500 && frac > 0.5);

    // Loud low-frequency cage noise alone: never song.
    const auto cage = synthesizeSong(fs, 5.0, 0.0005, 1, {}, 600.0, 0.2, 0.05);
    CHECK(run(cage, &frac) == 0);
    std::printf("  cage noise only: max fraction %.2f\n", frac);

    // An isolated 40 ms call every 1.5 s: too short to fill 25% of the 0.5 s kernel.
    auto calls = synthesizeSong(fs, 5.0, 0.0005, 1, {}, 600.0, 0.2, 0.003);
    for (double t = 300; t < 4800; t += 1500) addSyllable(calls, fs, t, 40, 700);
    CHECK(run(calls, &frac) == 0);
    std::printf("  isolated calls: max fraction %.2f\n", frac);
}

static void testSongGate() {
    // Same song, but every 3rd motif is replaced by the target triplet alone with
    // long gaps around it... simpler: gate on vs. a song detector that can never fire.
    AppParams p;
    p.song.ratioThresh = 1e9;  // nothing counts as song
    const auto song = synthesizeSong(p.hw.sampleRate, 5.0, p.source.simNoiseVolts);
    Detector det(3);
    det.setParams(p);
    int matches = 0, blocked = 0, fires = 0;
    for (size_t i = 0; i + 40 <= song.size(); i += 40) {
        auto r = det.process(&song[i], 40);
        matches += r.match.match;
        blocked += r.matchBlocked;
        fires += r.fb.fire != 0;
    }
    CHECK(matches > 0 && blocked == matches && fires == 0);
    p.song.gateFeedback = false;  // gate off -> feedback again
    Detector det2(3);
    det2.setParams(p);
    fires = 0;
    for (size_t i = 0; i + 40 <= song.size(); i += 40) fires += det2.process(&song[i], 40).fb.fire != 0;
    CHECK(fires == matches);
}

static void testOfflineMatchesRealtime() {
    AppParams p;
    p.fb.prob1 = 0.7;  // exercise the random draw too
    const auto song = synthesizeSong(p.hw.sampleRate, 12.0, p.source.simNoiseVolts);
    const AppParams pr = paramsForRate(p, p.hw.sampleRate);

    Detector det(99);
    det.setParams(pr);
    const int n = pr.hw.samplesPerFrame;
    std::vector<float> rtValue;
    std::vector<double> rtOnsets, rtFires, rtMatches;
    for (size_t i = 0; i + n <= song.size(); i += n) {
        const auto r = det.process(&song[i], n);
        rtValue.push_back(float(r.value));
        if (r.seg.onset) rtOnsets.push_back(r.tMs);
        if (r.fb.fire) rtFires.push_back(r.tMs);
        if (r.match.match && !r.matchBlocked) rtMatches.push_back(r.tMs);
    }

    EnvelopeTrack env;
    SongTrack st;
    DecisionTrack dt;
    CHECK(computeEnvelopeTrack(song, pr, env));
    computeSongTrack(song, pr, st);
    runDecisions(env, st, nullptr, pr, dt, 99);

    CHECK(env.value == rtValue);  // bit-identical
    std::vector<double> offOnsets;
    for (const auto& s : dt.syllables) offOnsets.push_back(s.onsetMs);
    std::vector<double> offFires = dt.fire1Ms;
    CHECK(offOnsets == rtOnsets);
    CHECK(dt.matchMs == rtMatches);
    CHECK(offFires == rtFires);
    std::printf("  %zu frames, %zu syllables, %zu matches, %zu feedback: identical\n", env.value.size(),
                dt.syllables.size(), dt.matchMs.size(), offFires.size());

    // A syllable that matched should carry the values that made it the target.
    const SyllableCode& t = *targetSyllable(p.codes);
    for (const auto& s : dt.syllables)
        if (s.matched) {
            CHECK(s.codeIndex == p.codes.target);
            CHECK(t.ioi.contains(s.ioiMs, t.mode) && t.sd.contains(s.prevSdMs, t.mode) &&
                  t.prevIoi.contains(s.prevIoiMs, t.mode));
        }
}

static void testTargetIsFirstMatch() {
    // The target only fires when it is the first syllable in the list that matches.
    AppParams p;
    const auto song = synthesizeSong(p.hw.sampleRate, 10.0, p.source.simNoiseVolts);
    const AppParams pr0 = paramsForRate(p, p.hw.sampleRate);
    EnvelopeTrack env;
    SongTrack st;
    computeEnvelopeTrack(song, pr0, env);
    computeSongTrack(song, pr0, st);
    DecisionTrack a, b, c;
    runDecisions(env, st, nullptr, pr0, a);  // default: target is the only syllable
    CHECK(a.matchMs.size() >= 7);

    AppParams p2 = p;  // a broad "any IOI 50-200 ms" syllable listed before the target swallows it
    SyllableCode any = defaultTargetSyllable();
    any.name = "any";
    any.code = 9;
    any.type = TriggerType::IOI;
    any.mode = RangeMode::MinMax;
    any.ioi.minMs = 50;
    any.ioi.maxMs = 200;
    p2.codes.list.insert(p2.codes.list.begin(), any);
    p2.codes.target = 1;
    runDecisions(env, st, nullptr, paramsForRate(p2, p.hw.sampleRate), b);
    CHECK(b.matchMs.empty() && b.fire1Ms.empty());

    p2.codes.target = 0;  // now "any" itself is the target: every classified syllable
    runDecisions(env, st, nullptr, paramsForRate(p2, p.hw.sampleRate), c);
    size_t classified = 0;
    for (const auto& s : c.syllables) classified += s.codeIndex == 0;
    // Early-bout matches can be blocked by the song gate, so count both.
    CHECK(c.matchMs.size() + c.blockedMs.size() == classified && classified > a.matchMs.size());

    p2.codes.target = -1;  // no target: no feedback at all
    runDecisions(env, st, nullptr, paramsForRate(p2, p.hw.sampleRate), c);
    CHECK(c.matchMs.empty() && c.fire1Ms.empty());
}

static void testLegacySettingsMigrate() {
    // A settings file from before syllables existed: its single target becomes a syllable.
    {
        std::FILE* f = std::fopen("fp_legacy.ini", "w");
        std::fputs("seq.trigger_type = 3\nseq.ioi_setpoint_ms = 130\nseq.ioi_eps_ms = 7\nseq.sd_setpoint_ms = 88\n", f);
        std::fclose(f);
    }
    AppParams p;
    CHECK(loadParams("fp_legacy.ini", p));
    CHECK(p.codes.list.size() == 1 && p.codes.target == 0);
    if (!p.codes.list.empty()) {
        const auto& c = p.codes.list[0];
        CHECK(c.type == TriggerType::SD_IOI && c.ioi.setpointMs == 130 && c.ioi.epsMs == 7 && c.sd.setpointMs == 88 &&
              c.ioi.minMs == 123 && c.ioi.maxMs == 137);
    }
    std::remove("fp_legacy.ini");
}

static SyllableCode makeCode(const char* name, int code, TriggerType type, double ioi, double sd, double prevIoi,
                             double eps = 10) {
    SyllableCode c;
    c.name = name;
    c.code = code;
    c.type = type;
    c.ioi = {ioi, eps, ioi - eps, ioi + eps};
    c.sd = {sd, eps, sd - eps, sd + eps};
    c.prevIoi = {prevIoi, eps, prevIoi - eps, prevIoi + eps};
    return c;
}

static void testSyllableClassifier() {
    // Onsets at 0, 100, 250 ms; durations 60, 80.
    // At 100: IOI 100, SD (prev) 60, no prev IOI. At 250: IOI 150, SD 80, prev IOI 100.
    SyllableCodeParams p;
    p.list.clear();  // drop the default target
    p.list.push_back(makeCode("x", 9, TriggerType::IOI_SD_IOI, 150, 80, 100));
    SyllableCode y = makeCode("y", 7, TriggerType::SD_IOI, 0, 0, 0);
    y.mode = RangeMode::MinMax;
    y.ioi.minMs = 140;
    y.ioi.maxMs = 160;
    y.sd.minMs = 70;
    y.sd.maxMs = 90;
    p.list.push_back(y);
    p.list.push_back(makeCode("z", 3, TriggerType::IOI, 100, 0, 0, 5));
    auto run = [](const SyllableCodeParams& pp) {
        SyllableSegmenter seg;
        SyllableClassifier cl;
        std::vector<int> codes;
        for (auto [t, v] : std::vector<std::pair<double, double>>{{0, 1}, {60, 0}, {100, 1}, {180, 0}, {250, 1}}) {
            auto r = cl.update(seg.update(v, t, 0.5, 0.2), pp);
            if (r.evaluated) codes.push_back(r.code);
        }
        return codes;
    };
    auto c = run(p);
    CHECK(c.size() == 2 && c[0] == 3 && c[1] == 9);  // "x" needs a previous IOI, so the first IOI end is "z"
    std::swap(p.list[0], p.list[1]);
    c = run(p);
    CHECK(c.size() == 2 && c[1] == 7);  // x and y both fit the second IOI end; the first listed wins

    // Min/max and setpoint/eps describe the same window after converting.
    TimeWindow w{120, 8, 0, 0};
    w.convertTo(RangeMode::MinMax);
    CHECK(w.minMs == 112 && w.maxMs == 128 && w.contains(112, RangeMode::MinMax) && !w.contains(111.9, RangeMode::MinMax));
    w.minMs = 100;
    w.convertTo(RangeMode::SetpointEps);
    CHECK(w.setpointMs == 114 && w.epsMs == 14);
}

static void testSyllableCodesOnSong() {
    // Label every syllable of the synthetic motif after the first. Motif timing as
    // measured: IOIs A-B 90, B-C 95, C-D 124, D-E 150 ms; SDs A 59, B 69, C 91, D 109.
    AppParams p;
    SyllableCode e = makeCode("E", 5, TriggerType::SD_IOI, 0, 0, 0);
    e.mode = RangeMode::MinMax;  // E by min/max: IOI 140-160, SD of D 100-120
    e.ioi.minMs = 140;
    e.ioi.maxMs = 160;
    e.sd.minMs = 100;
    e.sd.maxMs = 120;
    p.codes.list = {
        makeCode("B", 2, TriggerType::IOI, 90, 0, 0, 3),
        makeCode("C", 3, TriggerType::SD_IOI, 95, 69, 0, 4),
        makeCode("D", 4, TriggerType::IOI_SD_IOI, 124, 91, 95, 5),
        e,
    };
    const auto song = synthesizeSong(p.hw.sampleRate, 10.0, p.source.simNoiseVolts);
    const AppParams pr = paramsForRate(p, p.hw.sampleRate);
    EnvelopeTrack env;
    SongTrack st;
    DecisionTrack dt;
    computeEnvelopeTrack(song, pr, env);
    computeSongTrack(song, pr, st);
    runDecisions(env, st, nullptr, pr, dt);
    int counts[6] = {0};
    int unlabeled = 0;
    std::string seq;
    for (const auto& s : dt.syllables) {
        if (s.code) ++counts[s.code];
        else ++unlabeled;
        seq += s.code ? char('A' + s.code - 1) : '.';
    }
    std::printf("  labels: %s\n", seq.c_str());
    // Each motif: A unlabeled (it follows the long inter-motif gap), then B C D E.
    // The file may end part-way through a motif.
    std::string expected;
    while (expected.size() < seq.size()) expected += ".BCDE";
    expected.resize(seq.size());
    CHECK(seq == expected);
    CHECK(counts[2] >= 7 && unlabeled >= 7);
}

static void testSongFiles() {
    // 1 kHz, 10-sample frames, 0.5 s pre-roll, 0.3 s post-roll. The "mic" signal is the
    // sample index, so each file's first sample and length show exactly what was saved.
    // The session starts 1 s before midnight (local time), so later files go in the next day's folder.
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 9;
    tm.tm_mday = 23;
    tm.tm_hour = 23;
    tm.tm_min = 59;
    tm.tm_sec = 59;
    tm.tm_isdst = -1;
    SongFileConfig cfg;
    cfg.birdDir = "fp_songtest_dir";
    cfg.bird = "Zf1";
    cfg.indexPath = "fp_songtest_songs.csv";
    cfg.startEpochMs = double(std::mktime(&tm)) * 1000.0;
    cfg.sampleRate = 1000.0;
    cfg.preRollSec = 0.5;
    cfg.postRollSec = 0.3;
    SongFileWriter w;
    CHECK(w.open(cfg));
    CHECK(w.channels() == 3);
    auto songOn = [](int f) {
        return (f >= 100 && f < 150) ||  // song 1 ...
               (f >= 165 && f < 170) ||  // ... resumes within the post-roll: same file
               (f >= 300 && f < 310) ||  // song 2
               (f >= 345 && f < 350) ||  // song 3 starts 50 ms after file 2 closed: pre-roll overlaps it
               f >= 400;                 // song 4 still on when recording stops
    };
    float mic[10];
    for (int f = 0; f < 420; ++f) {
        for (int i = 0; i < 10; ++i) mic[i] = float(f * 10 + i);
        w.push(mic, 10, 0.5f, songOn(f) ? 0.1f : 0.0f, songOn(f));
    }
    CHECK(w.recording());
    w.close();
    CHECK(w.filesDone() == 4);

    // {first sample, length}: pre-roll (up to 500) + song + 300 post-roll.
    const std::pair<int, int> expected[4] = {{500, 1500}, {2500, 900}, {2950, 850}, {3500, 700}};
    // Named after each file's first sample: 23:59:59 + 0.5 s, + 2.5 s (next day), ...
    const char* names[4] = {"fp_songtest_dir/20261023/Zf1_20261023_235959_500.wav",
                            "fp_songtest_dir/20261024/Zf1_20261024_000001_500.wav",
                            "fp_songtest_dir/20261024/Zf1_20261024_000001_950.wav",
                            "fp_songtest_dir/20261024/Zf1_20261024_000002_500.wav"};
    for (int k = 0; k < 4; ++k) {
        const char* name = names[k];
        WavData d;
        CHECK(readWav(name, d));
        const auto [first, len] = expected[k];
        bool contiguous = d.samples.size() == size_t(len);
        for (size_t i = 0; contiguous && i < d.samples.size(); ++i) contiguous = d.samples[i] == float(first + int(i));
        std::printf("  %s: starts at sample %.0f, %zu samples (expect %d, %d)\n", name,
                    d.samples.empty() ? -1.0 : d.samples[0], d.samples.size(), first, len);
        CHECK(contiguous);
    }
    std::ifstream idx("fp_songtest_songs.csv");
    int rows = 0;
    for (std::string line; std::getline(idx, line);) ++rows;
    idx.close();
    CHECK(rows == 5);  // header + 4 files
    std::remove("fp_songtest_songs.csv");
    std::filesystem::remove_all("fp_songtest_dir");
}

static void testNoiseBurst() {
    const double fs = 40000.0;
    PulseFeedbackParams p;  // LabVIEW defaults: 1500-8000 Hz, order 10
    p.durationMs = 200;
    p.delayMs = 10;
    p.gain = 1.0;
    std::string err;
    auto x = makeNoiseBurst(p, fs, -10, 10, 1, &err);
    CHECK(err.empty());
    CHECK(x.size() == size_t(210.0 * fs / 1000.0));
    const size_t nDelay = size_t(10.0 * fs / 1000.0);
    bool silent = true;
    for (size_t i = 0; i < nDelay; ++i) silent = silent && x[i] == 0.0;
    CHECK(silent);  // the delay is silence

    // Power inside the band vs. well outside it (FFT of the noise part).
    const int nfft = 8192;
    std::vector<std::complex<double>> spec(nfft);
    for (int i = 0; i < nfft; ++i) spec[size_t(i)] = x[nDelay + size_t(i)];
    fftInPlace(spec);
    double inBand = 0, below = 0, above = 0;
    int nIn = 0, nBelow = 0, nAbove = 0;
    for (int k = 1; k < nfft / 2; ++k) {
        const double f = k * fs / nfft, pw = std::norm(spec[size_t(k)]);
        if (f > 2500 && f < 7000) { inBand += pw; ++nIn; }
        else if (f < 500) { below += pw; ++nBelow; }
        else if (f > 10000) { above += pw; ++nAbove; }
    }
    CHECK(inBand / nIn > 100 * below / nBelow);
    CHECK(inBand / nIn > 100 * above / nAbove);

    // rms is close to gain * sqrt(band fraction)
    double ss = 0;
    for (size_t i = nDelay + 2000; i < x.size(); ++i) ss += x[i] * x[i];
    const double rms = std::sqrt(ss / double(x.size() - nDelay - 2000));
    CHECK(std::fabs(rms - expectedNoiseRms(p, fs)) < 0.1 * expectedNoiseRms(p, fs));

    // linear in gain, different every call, clipped to the output range
    PulseFeedbackParams p2 = p;
    p2.gain = 2.0;
    auto y = makeNoiseBurst(p2, fs, -10, 10, 1);
    CHECK(std::fabs(y[nDelay + 1000] - 2.0 * x[nDelay + 1000]) < 1e-3);
    CHECK(makeNoiseBurst(p, fs, -10, 10, 2) != x);
    p2.gain = 100.0;
    auto z = makeNoiseBurst(p2, fs, -1, 1, 1);
    double zmax = 0;
    for (double v : z) zmax = std::max(zmax, std::fabs(v));
    CHECK(zmax == 1.0);

    // a band that does not fit the sample rate is rejected
    p2 = p;
    p2.highHz = 30000;
    CHECK(makeNoiseBurst(p2, fs, -10, 10, 1, &err).empty() && !err.empty());

    // wave file: delay, then samples x gain
    p2 = p;
    p2.gain = 2.0;
    auto w = makeWaveBurst({0.25f, -0.5f, 1.0f}, fs, p2, -10, 10);
    CHECK(w.size() == nDelay + 3 && w[nDelay] == 0.5 && w[nDelay + 1] == -1.0 && w[nDelay + 2] == 2.0 && w[0] == 0.0);
}

static void testParamSections() {
    // A multi-box settings file: two boxes and a tuning set, each under its own prefix.
    AppParams a, b, t;
    a.boxName = "Zebra A";
    a.hw.aiChannel = "Dev2/ai0";
    b.boxName = "Zebra B";
    b.hw.aiChannel = "Dev2/ai1";
    b.boxEnabled = false;
    b.spectrum.sylStartThresh = 0.5;
    t.spectrum.sylStartThresh = 0.123;
    t.codes.target = -1;
    {
        std::ofstream out("fp_sections.ini");
        out << "boxes.count = 2\n";
        writeParams(out, a, "box1.");
        writeParams(out, b, "box2.");
        writeParams(out, t, "tune.");
    }
    ParamMap kv;
    CHECK(readParamFile("fp_sections.ini", kv));
    CHECK(hasSection(kv, "box2.") && hasSection(kv, "tune.") && !hasSection(kv, "box3.") && !hasSection(kv, "hw."));
    AppParams a2, b2, t2;
    CHECK(readParams(kv, a2, "box1.").empty());
    CHECK(readParams(kv, b2, "box2.").empty());
    CHECK(readParams(kv, t2, "tune.").empty());
    CHECK(a2.boxName == "Zebra A" && a2.hw.aiChannel == "Dev2/ai0" && a2.boxEnabled);
    CHECK(b2.boxName == "Zebra B" && b2.hw.aiChannel == "Dev2/ai1" && !b2.boxEnabled && b2.spectrum.sylStartThresh == 0.5);
    CHECK(t2.spectrum.sylStartThresh == 0.123 && t2.codes.target == -1);
    std::remove("fp_sections.ini");

    // A single-box file still loads unprefixed, as before.
    CHECK(saveParams("fp_single.ini", a));
    AppParams s;
    CHECK(loadParams("fp_single.ini", s) && s.boxName == "Zebra A");
    std::remove("fp_single.ini");
}

static void testParamsRoundTrip() {
    AppParams a;
    a.hw.aiChannel = "PXI1Slot2/ai3";
    a.spectrum.include[2] = {2.5, 300, 900};
    a.fb.wave1 = "C:\\stim\\noise #1.wav";
    a.codes.list = {makeCode("A #1", 1, TriggerType::IOI, 90, 0, 0)};
    a.pulse.enabled = true;
    a.pulse.diLine = "Dev1/port0/line4";
    a.pulse.source = PulseSource::WavFile;
    a.pulse.durationMs = 75;
    a.pulse.delayMs = 12.5;
    a.pulse.gain = 2.25;
    a.pulse.lowHz = 1200;
    a.pulse.highHz = 9000;
    a.pulse.filterOrder = 6;
    a.codes.target = 1;
    SyllableCode c2 = makeCode("B", 17, TriggerType::IOI_SD_IOI, 124, 91, 95, 7);
    c2.mode = RangeMode::MinMax;
    c2.sd.minMs = 80.5;
    a.codes.list.push_back(c2);
    CHECK(saveParams("fp_test_params.ini", a));
    AppParams b;
    CHECK(loadParams("fp_test_params.ini", b));
    CHECK(b.hw.aiChannel == a.hw.aiChannel);
    CHECK(b.spectrum.include[2].gain == 2.5 && b.spectrum.include[2].highHz == 900);
    CHECK(b.fb.wave1 == a.fb.wave1);
    CHECK(b.pulse.enabled && b.pulse.diLine == a.pulse.diLine && b.pulse.source == PulseSource::WavFile);
    CHECK(b.pulse.durationMs == 75 && b.pulse.delayMs == 12.5 && b.pulse.gain == 2.25 && b.pulse.lowHz == 1200 &&
          b.pulse.highHz == 9000 && b.pulse.filterOrder == 6);
    CHECK(b.codes.list.size() == 2 && b.codes.target == 1);
    CHECK(!b.codes.list.empty() && b.codes.list[0].name == "A #1");  // '#' survives in names
    if (b.codes.list.size() == 2) {
        const auto& c = b.codes.list[1];
        CHECK(c.name == "B" && c.code == 17 && c.type == TriggerType::IOI_SD_IOI && c.mode == RangeMode::MinMax &&
              c.sd.minMs == 80.5 && c.prevIoi.setpointMs == 95 && c.ioi.epsMs == 7);
    }
    std::remove("fp_test_params.ini");
}



static void testSpectralTemplates() {
    AppParams p;
    p.tmpl.method = DetectMethod::Template;
    const double fs = p.hw.sampleRate;
    const auto song = synthesizeSong(fs, 10.0, p.source.simNoiseVolts);
    // Motif every 1189 ms from 200 ms: A 200-250, B 290-350, C 385-468 ms (see defaultMotif()).
    const AppParams pr = paramsForRate(p, fs);
    SpectralTemplate a, c;
    a.name = "A";
    a.code = 1;
    c.name = "C";
    c.code = 3;
    CHECK(templateShapeAt(song, pr, 220.0, a.shape));
    CHECK(templateShapeAt(song, pr, 410.0, c.shape));
    CHECK(int(c.shape.size()) == templateBands(p.tmpl));
    p.tmpl.list = {a, c};
    p.tmpl.target = 1;

    // Offline (feature track + decisions) and real time must agree exactly.
    auto offline = [&](const AppParams& q, DecisionTrack& dt) {
        const AppParams qr = paramsForRate(q, fs);
        EnvelopeTrack env;
        SongTrack st;
        FeatureTrack ft;
        computeEnvelopeTrack(song, qr, env);
        computeSongTrack(song, qr, st);
        computeFeatureTrack(song, qr, ft);
        runDecisions(env, st, &ft, qr, dt);
    };
    DecisionTrack dt;
    offline(p, dt);
    std::vector<double> live;
    Detector det;
    det.setParams(p);
    const int n = p.hw.samplesPerFrame;
    for (size_t i = 0; i + size_t(n) <= song.size(); i += size_t(n))
        if (det.process(&song[i], n).match.match) live.push_back(double(i + size_t(n)) * 1000.0 / fs);
    std::vector<double> matches = dt.matchMs;
    matches.insert(matches.end(), dt.blockedMs.begin(), dt.blockedMs.end());
    std::sort(matches.begin(), matches.end());
    CHECK(matches == live);

    // One detection of C per motif, a few ms after the point the template was taken from.
    int c3 = 0, wrong = 0;
    double worst = 0;
    for (const auto& h : dt.tmplHits) {
        if (h.index != 1) continue;
        const double k = std::round((h.ms - 410.0) / 1189.0), err = h.ms - (410.0 + k * 1189.0);
        if (std::abs(err) < 20) ++c3, worst = std::max(worst, std::abs(err));
        else ++wrong;
    }
    const int motifs = 8;  // full motifs in 10 s
    std::printf("  template C: %d hits on C (worst %.1f ms from the template point), %d elsewhere; %zu matches\n", c3,
                worst, wrong, matches.size());
    CHECK(c3 >= motifs && wrong == 0);

    // Timing gate: C after A (A at +20 ms, C at +210 ms into the motif).
    p.tmpl.list[1].gateIndex = 0;
    p.tmpl.list[1].gateMinMs = 150;
    p.tmpl.list[1].gateMaxMs = 230;
    DecisionTrack g1, g2;
    offline(p, g1);
    p.tmpl.list[1].gateMinMs = 10;
    p.tmpl.list[1].gateMaxMs = 50;
    offline(p, g2);
    std::printf("  gate 150-230 ms: %zu matches, gate 10-50 ms: %zu\n", g1.matchMs.size() + g1.blockedMs.size(),
                g2.matchMs.size() + g2.blockedMs.size());
    CHECK(g1.matchMs.size() + g1.blockedMs.size() == matches.size());
    CHECK(g2.matchMs.empty() && g2.blockedMs.empty());

    // Settings round trip, shape included.
    std::ostringstream os;
    writeParams(os, p);
    const std::string path = "fp_tmpl_test.ini";
    {
        std::ofstream f(path);
        f << os.str();
    }
    AppParams q;
    CHECK(loadParams(path, q));
    std::remove(path.c_str());
    CHECK(q.tmpl.method == DetectMethod::Template && q.tmpl.list.size() == 2 && q.tmpl.target == 1);
    CHECK(q.tmpl.list[1].shape.size() == p.tmpl.list[1].shape.size() && q.tmpl.list[1].gateIndex == 0);
    double maxDiff = 0;
    for (size_t i = 0; i < q.tmpl.list[1].shape.size(); ++i)
        maxDiff = std::max(maxDiff, double(std::abs(q.tmpl.list[1].shape[i] - p.tmpl.list[1].shape[i])));
    CHECK(maxDiff < 1e-5);
}

static void testDiscriminativeTemplate() {
    AppParams p;
    p.tmpl.method = DetectMethod::Template;
    p.tmpl.windowMs = 8.0;  // short window: plain correlation confuses syllables here
    const double fs = p.hw.sampleRate;
    const auto song = synthesizeSong(fs, 10.0, p.source.simNoiseVolts);
    const std::string path = "fp_disc_test.wav";
    {
        WavWriter w;
        w.open(path, fs);
        w.write(song.data(), song.size());
        w.close();
    }
    // Every rendition of syllable C labeled (motif every 1189 ms; C at +210 ms).
    SpectralTemplate c;
    c.name = "C";
    for (int k = 0; k < 9; ++k) c.examples.push_back({path, 410.0 + 1189.0 * k});
    std::string err;
    CHECK(rebuildTemplateShape(c, p, &err));
    CHECK(c.stats && int(c.weights.size()) == templateBands(p.tmpl));
    if (!c.stats) return;
    const auto& st = *c.stats;
    std::printf("  %d target frames, %d other frames. plain: d' %.1f, max other %.2f vs min target %.2f. "
                "discriminative: d' %.1f, max other %.2f vs min target %.2f\n",
                st.positives, st.negatives, st.plain.dPrime, st.plain.maxNeg, st.plain.minPos, st.disc.dPrime,
                st.disc.maxNeg, st.disc.minPos);
    CHECK(st.disc.dPrime > st.plain.dPrime);
    CHECK(st.disc.maxNeg < st.disc.minPos);  // fully separated

    // Detection with each, at its suggested threshold: hits on C vs elsewhere.
    auto run = [&](bool disc) {
        AppParams q = p;
        c.discriminative = disc;
        c.thresh = disc ? st.disc.suggested : 0.85;
        c.minFrames = 2;
        q.tmpl.list = {c};
        q.tmpl.target = 0;
        const AppParams qr = paramsForRate(q, fs);
        EnvelopeTrack env;
        SongTrack sg;
        FeatureTrack ft;
        computeEnvelopeTrack(song, qr, env);
        computeSongTrack(song, qr, sg);
        computeFeatureTrack(song, qr, ft);
        DecisionTrack dt;
        runDecisions(env, sg, &ft, qr, dt);
        int onC = 0, other = 0;
        for (const auto& h : dt.tmplHits) {
            const double k = std::round((h.ms - 410.0) / 1189.0);
            (std::abs(h.ms - 410.0 - 1189.0 * k) < 20 ? onC : other)++;
        }
        std::printf("  %s: %d on C, %d elsewhere\n", disc ? "discriminative" : "plain (0.85)", onC, other);
        return std::make_pair(onC, other);
    };
    const auto pl = run(false), di = run(true);
    CHECK(di.first >= 9 && di.second == 0);
    CHECK(di.second <= pl.second);

    // Saved and loaded with the template.
    std::ostringstream os;
    AppParams q = p;
    c.discriminative = true;
    q.tmpl.list = {c};
    writeParams(os, q);
    {
        std::ofstream f("fp_disc_test.ini");
        f << os.str();
    }
    AppParams r;
    CHECK(loadParams("fp_disc_test.ini", r));
    CHECK(r.tmpl.list.size() == 1 && r.tmpl.list[0].discriminative && r.tmpl.list[0].weights.size() == c.weights.size() &&
          std::abs(r.tmpl.list[0].bias - c.bias) < 1e-6);
    std::remove("fp_disc_test.ini");
    std::remove(path.c_str());
}

int main() {
    std::printf("BandEnvelope\n");
    testBandEnvelopeSine();
    std::printf("Segmenter + matcher\n");
    testSegmenterAndMatcher();
    std::printf("Feedback state machine\n");
    testStateMachine();
    std::printf("End to end (synthetic song)\n");
    testEndToEndSynthetic();
    std::printf("Butterworth bandpass\n");
    testButterworth();
    std::printf("Song detector\n");
    testSongDetector();
    std::printf("Song gate\n");
    testSongGate();
    std::printf("Offline analysis == real time\n");
    testOfflineMatchesRealtime();
    std::printf("Syllable classifier\n");
    testSyllableClassifier();
    std::printf("Target = first match\n");
    testTargetIsFirstMatch();
    std::printf("Legacy settings migrate\n");
    testLegacySettingsMigrate();
    std::printf("Syllable codes on synthetic song\n");
    testSyllableCodesOnSong();
    std::printf("Song-triggered files\n");
    testSongFiles();
    std::printf("Settings sections (multi-box files)\n");
    testParamSections();
    std::printf("Params round trip\n");
    testParamsRoundTrip();
    std::printf("Spectral templates\n");
    testSpectralTemplates();
    std::printf("Discriminative template\n");
    testDiscriminativeTemplate();
    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All tests passed\n");
    return 0;
}

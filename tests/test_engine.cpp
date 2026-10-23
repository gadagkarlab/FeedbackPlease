// Runs the real-time engine headless on simulated sources for a few seconds and
// checks that frames flow, the loop keeps up, and feedback is delivered: first one
// box (the single-box launch modes), then three boxes at once (multi-box mode).
// Also lists NI-DAQmx devices (read-only) so you can see what the app will find.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "app/Engine.h"

#ifdef FP_HAVE_DAQMX
#include <NIDAQmx.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif

using namespace fp;

int main() {
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
#ifdef FP_HAVE_DAQMX
    char devs[1024] = {};
    const int32 code = DAQmxGetSysDevNames(devs, sizeof devs);
    std::printf("NI-DAQmx devices: %s\n", code < 0 ? "(query failed)" : (devs[0] ? devs : "(none)"));
    for (char* tok = std::strtok(devs, ", "); tok; tok = std::strtok(nullptr, ", ")) {
        char type[256] = {}, ai[2048] = {}, ao[1024] = {}, lines[2048] = {};
        DAQmxGetDevProductType(tok, type, sizeof type);
        DAQmxGetDevAIPhysicalChans(tok, ai, sizeof ai);
        DAQmxGetDevAOPhysicalChans(tok, ao, sizeof ao);
        DAQmxGetDevDOLines(tok, lines, sizeof lines);
        std::printf("  %s: %s\n    AI: %.80s\n    AO: %.80s\n    DO: %.80s\n", tok, type, ai, ao, lines);
    }
#endif

    int fails = 0;
    auto failIf = [&](bool bad, const char* what) {
        if (bad) {
            std::printf("FAIL %s\n", what);
            ++fails;
        }
    };

    // ---- single box (the single-box launch modes)
    AppParams p;
    p.source.kind = SourceKind::Simulated;
    p.rec.saveEvents = false;
    p.rec.songFiles = false;
    // One syllable code: the motif's 124 ms IOI (the 4th syllable).
    SyllableCode sylD;
    sylD.name = "D";
    sylD.code = 4;
    sylD.type = TriggerType::IOI;
    sylD.ioi = {124.0, 8.0, 116.0, 132.0};
    p.codes.list = {sylD};  // the only syllable, and the feedback target
    p.codes.target = 0;
    {
        Engine e;
        std::string err;
        if (!e.start({p}, true, err)) {
            std::printf("FAIL start: %s\n", err.c_str());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(4));
        const auto s = e.box(0)->stats();
        ScopeSnapshot snap;
        e.box(0)->snapshot(2.0, snap);
        e.stop();

        std::printf("single: frames %llu in %.2f s, proc avg %.1f us max %.1f us, max backlog %.2f ms\n",
                    (unsigned long long)s.frames, s.elapsedSec, s.procUsAvg, s.procUsMax, s.maxBacklogMs);
        std::printf("single: onsets %llu matches %llu feedback %llu; scope raw %zu env %zu\n",
                    (unsigned long long)s.onsets, (unsigned long long)s.matches, (unsigned long long)s.fires1,
                    snap.raw.size(), snap.env.size());
        failIf(s.frames < 3500, "too few frames");
        failIf(s.matches < 2 || s.fires1 != s.matches, "matches/feedback");
        failIf(snap.raw.size() != 80000 || snap.env.size() != 2000, "scope sizes");
        failIf(!e.error().empty(), "engine error");
        const uint64_t dCount = s.codeCounts.empty() ? 0 : s.codeCounts[0];
        std::printf("single: syllable code 'D': %llu (unmatched IOI ends %llu)\n", (unsigned long long)dCount,
                    (unsigned long long)s.codeUnmatched);
        failIf(dCount < 2 || dCount != s.matches, "syllable code count");
    }

    // ---- three boxes at once (multi-box mode, simulated sources)
    {
        std::vector<AppParams> boxes(3, p);
        boxes[0].boxName = "Box A";
        boxes[1].boxName = "Box B";
        boxes[1].codes.target = -1;  // no feedback target: matches never fire
        boxes[2].boxName = "Box C";
        boxes[2].fb.enabled = false;  // target on, feedback switched off
        Engine e;
        std::string err;
        if (!e.start(boxes, false, err)) {
            std::printf("FAIL multi start: %s\n", err.c_str());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(4));
        failIf(!e.running(), "multi engine stopped early");
        EngineStats s[3];
        for (int i = 0; i < 3; ++i) s[i] = e.box(i)->stats();
        e.stop();
        for (int i = 0; i < 3; ++i)
            std::printf("multi %s: frames %llu, onsets %llu, matches %llu, feedback %llu, proc avg %.1f us\n",
                        boxes[size_t(i)].boxName.c_str(), (unsigned long long)s[i].frames,
                        (unsigned long long)s[i].onsets, (unsigned long long)s[i].matches,
                        (unsigned long long)s[i].fires1, s[i].procUsAvg);
        for (int i = 0; i < 3; ++i) failIf(s[i].frames < 3500 || s[i].onsets < 10, "multi: a box did not run");
        failIf(s[0].matches < 2 || s[0].fires1 != s[0].matches, "multi: box A feedback");
        failIf(s[1].matches != 0 || s[1].fires1 != 0, "multi: box B (no target) gave feedback");
        failIf(s[2].matches < 2 || s[2].fires1 != 0, "multi: box C (feedback off) gave feedback");
        failIf(!e.error().empty(), "multi engine error");
    }

    std::printf(fails ? "Engine test FAILED\n" : "Engine test passed\n");
    return fails ? 1 : 0;
}

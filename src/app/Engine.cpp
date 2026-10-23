#include "app/Engine.h"

#include <cctype>
#include <cmath>
#include <ctime>
#include <map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fp {

namespace {

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

// Box name as it appears in file names.
std::string fileTag(const std::string& name) {
    std::string t;
    for (char c : name) t += std::isalnum((unsigned char)c) || c == '-' ? c : '_';
    return t.empty() ? "box" : t;
}

}  // namespace

// ---------------------------------------------------------------- sources

bool SingleSource::start(std::string& err) {
    if (!io_->start(p_, err)) return false;
    fs_ = io_->sampleRate();
    // Keep the frame duration when a simulated WAV has a different rate than the settings.
    n_ = std::max(1, int(std::lround(p_.hw.samplesPerFrame * fs_ / p_.hw.sampleRate)));
    buf_.assign(size_t(n_), 0.0f);
    return true;
}

std::string deviceOf(const std::string& channel) {
    size_t a = channel.find_first_not_of('/');
    if (a == std::string::npos) return {};
    const size_t b = channel.find('/', a);
    return channel.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// ---------------------------------------------------------------- Engine

bool Engine::start(const std::vector<AppParams>& boxes, bool single, std::string& err) {
    stop();
    boxes_.clear();
    runners_.clear();
    if (boxes.empty()) {
        err = "no boxes";
        return false;
    }
    if (single && boxes.size() != 1) {
        err = "single-box mode runs exactly one box";
        return false;
    }

    // ---- group boxes into frame sources
    std::vector<std::unique_ptr<Runner>> runners;
    if (single) {
        auto r = std::make_unique<Runner>();
        const AppParams& p = boxes[0];
        r->src = std::make_unique<SingleSource>(p.source.kind == SourceKind::Daq ? makeDaqBackend() : makeSimBackend(), p);
        r->boxes = {0};
        runners.push_back(std::move(r));
    } else {
        std::map<std::string, std::vector<int>> byCard;  // keeps cards in a stable order
        for (int i = 0; i < int(boxes.size()); ++i) {
            if (boxes[size_t(i)].source.kind == SourceKind::Daq) {
                const std::string dev = deviceOf(boxes[size_t(i)].hw.aiChannel);
                if (dev.empty()) {
                    err = boxes[size_t(i)].boxName + ": no AI channel";
                    return false;
                }
                byCard[dev].push_back(i);
            } else {
                auto r = std::make_unique<Runner>();
                r->src = std::make_unique<SingleSource>(makeSimBackend(), boxes[size_t(i)]);
                r->boxes = {i};
                runners.push_back(std::move(r));
            }
        }
        for (auto& [dev, idx] : byCard) {
            std::vector<AppParams> ps;
            for (int i : idx) ps.push_back(boxes[size_t(i)]);
            auto r = std::make_unique<Runner>();
            r->src = makeDaqCard(dev, ps);
            if (!r->src) {
                err = "this build has no NI-DAQmx support; set the boxes' Source to Simulated";
                return false;
            }
            r->boxes = idx;
            runners.push_back(std::move(r));
        }
    }

    // ---- start hardware, then the boxes (files), then the threads
    auto fail = [&](const std::string& e) {
        for (auto& r : runners) r->src->stop();
        for (auto& b : boxes_) b->end();
        err = e;
        return false;
    };
    for (auto& r : runners) {
        std::string e;
        if (!r->src->start(e)) {
            std::string who;
            for (int i : r->boxes) who += (who.empty() ? "" : ", ") + boxes[size_t(i)].boxName;
            return fail(single ? e : who + ": " + e);
        }
    }
    boxes_.resize(boxes.size());
    for (auto& b : boxes_) b = std::make_unique<Box>();
    const std::string stamp = timestamp();
    for (auto& r : runners) {
        for (int k = 0; k < int(r->boxes.size()); ++k) {
            const int i = r->boxes[size_t(k)];
            AppParams p = boxes[size_t(i)];
            p.hw.sampleRate = r->src->sampleRate();
            p.hw.samplesPerFrame = r->src->frameSamples();
            std::string e;
            if (!boxes_[size_t(i)]->begin(p, r->src->name(), stamp, single ? "" : fileTag(p.boxName), e))
                return fail(p.boxName + ": " + e);
        }
    }

    stop_ = false;
    runners_ = std::move(runners);
    alive_ = int(runners_.size());
    for (auto& r : runners_) {
        Runner* rp = r.get();
        r->th = std::thread([this, rp] { run(*rp); });
    }
    return true;
}

void Engine::run(Runner& r) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    FrameSource& src = *r.src;
    const int n = src.frameSamples();
    std::string err;
    while (!stop_) {
        if (!src.read(err)) {
            for (int i : r.boxes) boxes_[size_t(i)]->setError(err);
            break;
        }
        const size_t backlog = src.backlog();
        for (int k = 0; k < int(r.boxes.size()); ++k)
            boxes_[size_t(r.boxes[size_t(k)])]->process(src.frame(k), n, src.out(k), backlog);
        src.endFrame();
    }
    for (int k = 0; k < int(r.boxes.size()); ++k) boxes_[size_t(r.boxes[size_t(k)])]->finish(src.out(k));
    src.endFrame();
    --alive_;
}

void Engine::stop() {
    stop_ = true;
    for (auto& r : runners_)
        if (r->th.joinable()) r->th.join();
    for (auto& r : runners_) r->src->stop();
    runners_.clear();
    for (auto& b : boxes_) b->end();  // boxes stay, so the GUI can still show their last state
    alive_ = 0;
}

std::string Engine::error() const {
    for (const auto& b : boxes_) {
        const std::string e = b->error();
        if (!e.empty()) return boxes_.size() > 1 ? "box " + std::to_string(&b - &boxes_[0] + 1) + ": " + e : e;
    }
    return {};
}

}  // namespace fp

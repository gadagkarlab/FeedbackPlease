#include "app/MsnFinder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace fp {

#ifndef FP_MSN_DEFAULT_SCRIPT
#define FP_MSN_DEFAULT_SCRIPT "tools/msn_finder/msn_server.py"
#endif

std::string msnDefaultScript() { return FP_MSN_DEFAULT_SCRIPT; }

namespace {

std::string exeDir() {
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return ".";
    const std::string path(buf, n);
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string resolveRelativeToExe(const std::string& path) {
    if (path.empty()) return path;
    const bool isAbsolute = (path.size() >= 2 && path[1] == ':') || path[0] == '/' || path[0] == '\\';
    return isAbsolute ? path : exeDir() + "\\" + path;
}

constexpr size_t kMaxLogLines = 2000;

// Buffered reader over a pipe handle: lines and exact byte counts.
class PipeReader {
public:
    explicit PipeReader(HANDLE h) : h_(h), buf_(1 << 16) {}

    bool readLine(std::string& line) {
        line.clear();
        for (;;) {
            for (; pos_ < len_; ++pos_) {
                const char c = buf_[pos_];
                if (c == '\n') {
                    ++pos_;
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    return true;
                }
                line.push_back(c);
            }
            if (!fill()) return !line.empty();
        }
    }

    bool readExact(void* dst, size_t n) {
        char* d = static_cast<char*>(dst);
        while (n > 0) {
            if (pos_ == len_ && !fill()) return false;
            const size_t k = std::min(n, len_ - pos_);
            std::memcpy(d, buf_.data() + pos_, k);
            pos_ += k;
            d += k;
            n -= k;
        }
        return true;
    }

private:
    bool fill() {
        DWORD got = 0;
        if (!ReadFile(h_, buf_.data(), static_cast<DWORD>(buf_.size()), &got, nullptr) || got == 0) return false;
        pos_ = 0;
        len_ = got;
        return true;
    }
    HANDLE h_;
    std::vector<char> buf_;
    size_t pos_ = 0, len_ = 0;
};

struct Arr {
    std::string dtype;
    std::vector<char> bytes;
    size_t size() const { return bytes.size() / (dtype == "f8" ? 8 : 4); }
    template <class T, class S>
    std::vector<T> as() const {
        const size_t n = size();
        std::vector<T> out(n);
        const S* p = reinterpret_cast<const S*>(bytes.data());
        for (size_t i = 0; i < n; ++i) out[i] = static_cast<T>(p[i]);
        return out;
    }
    std::vector<float> f32() const {
        return dtype == "f8" ? as<float, double>() : dtype == "i4" ? as<float, int32_t>() : as<float, float>();
    }
    std::vector<double> f64() const {
        return dtype == "f8" ? as<double, double>() : dtype == "i4" ? as<double, int32_t>() : as<double, float>();
    }
    std::vector<int> i32() const {
        return dtype == "i4" ? as<int, int32_t>() : dtype == "f8" ? as<int, double>() : as<int, float>();
    }
};

}  // namespace

struct MsnResponse {
    std::map<std::string, std::string> vals;
    std::map<std::string, Arr> arrs;
    double num(const std::string& k, double def = 0) const {
        auto it = vals.find(k);
        return it == vals.end() ? def : std::atof(it->second.c_str());
    }
    std::vector<float> f32(const std::string& k) const {
        auto it = arrs.find(k);
        return it == arrs.end() ? std::vector<float>() : it->second.f32();
    }
    std::vector<double> f64(const std::string& k) const {
        auto it = arrs.find(k);
        return it == arrs.end() ? std::vector<double>() : it->second.f64();
    }
    std::vector<int> i32(const std::string& k) const {
        auto it = arrs.find(k);
        return it == arrs.end() ? std::vector<int>() : it->second.i32();
    }
};

struct MsnBackend::Impl {
    HANDLE process = nullptr, thread = nullptr, in = nullptr, out = nullptr, err = nullptr;
    std::mutex sendMu;
    std::atomic<bool> alive{false};
};

MsnBackend::MsnBackend() = default;

MsnBackend::~MsnBackend() { kill(); }

void MsnBackend::log(const std::string& line) {
    std::lock_guard<std::mutex> lk(mu_);
    log_.push_back(line);
    while (log_.size() > kMaxLogLines) {
        log_.pop_front();
        ++logBase_;
    }
}

void MsnBackend::setError(const std::string& e) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        error_ = e;
    }
    log("ERROR: " + e);
}

std::string MsnBackend::lastError() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

void MsnBackend::clearError() {
    std::lock_guard<std::mutex> lk(mu_);
    error_.clear();
}

int MsnBackend::pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(pending_.size());
}

size_t MsnBackend::fetchLog(size_t from, std::vector<std::string>& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    const size_t end = logBase_ + log_.size();
    for (size_t i = std::max(from, logBase_); i < end; ++i) out.push_back(log_[i - logBase_]);
    return end;
}

#define MSN_GETTER(T, name, member)                      \
    std::shared_ptr<const T> MsnBackend::name() const { \
        std::lock_guard<std::mutex> lk(mu_);            \
        return member;                                   \
    }
MSN_GETTER(MsnSession, session, session_)
MSN_GETTER(MsnRanking, ranking, ranking_)
MSN_GETTER(MsnSpec, spec, spec_)
MSN_GETTER(MsnFigure, figure, figure_)
#undef MSN_GETTER

// ---------------------------------------------------------------------------------------------
// Process

void MsnBackend::kill() {
    std::shared_ptr<Impl> impl;
    {
        std::lock_guard<std::mutex> lk(mu_);
        impl = impl_;
        impl_.reset();
    }
    if (impl) {
        {
            std::lock_guard<std::mutex> lk(impl->sendMu);
            if (impl->alive.load() && impl->in) {
                const char quit[] = "QUIT\n";
                DWORD w = 0;
                WriteFile(impl->in, quit, sizeof quit - 1, &w, nullptr);
            }
        }
        if (impl->process && WaitForSingleObject(impl->process, 300) != WAIT_OBJECT_0) TerminateProcess(impl->process, 1);
    }
    if (outThread_.joinable()) outThread_.join();
    if (errThread_.joinable()) errThread_.join();
    if (impl) {
        for (HANDLE h : {impl->in, impl->out, impl->err, impl->process, impl->thread})
            if (h) CloseHandle(h);
    }
    std::lock_guard<std::mutex> lk(mu_);
    pending_.clear();
    specSent_ = false;
    specLabel_ = -1;
}

void MsnBackend::restart() {
    kill();
    log("Server stopped.");
}

bool MsnBackend::ensureProcess(const MsnParams& p, std::string& err) {
    const std::string script = resolveRelativeToExe(p.script.empty() ? msnDefaultScript() : p.script);
    const std::string key = p.pythonExe + "|" + script;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (impl_ && impl_->alive.load() && key == launchedWith_) return true;
    }
    kill();  // not running, died, or different interpreter / script

    if (p.pythonExe.empty() || p.pythonExe == "python" || p.pythonExe == "python.exe") {
        err = "Set Python to the full path of the python.exe that has numpy and scipy";
        return false;
    }
    if (GetFileAttributesA(p.pythonExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "Python not found: " + p.pythonExe;
        return false;
    }
    if (GetFileAttributesA(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "Server script not found: " + script;
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        err = "could not create pipes";
        return false;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    const std::string cmd = "\"" + p.pythonExe + "\" -u \"" + script + "\"";
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    const BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        CloseHandle(inW);
        CloseHandle(outR);
        CloseHandle(errR);
        err = "could not launch " + p.pythonExe;
        return false;
    }

    auto impl = std::make_shared<Impl>();
    impl->process = pi.hProcess;
    impl->thread = pi.hThread;
    impl->in = inW;
    impl->out = outR;
    impl->err = errR;
    impl->alive = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        impl_ = impl;
        launchedWith_ = key;
        error_.clear();
    }
    log("Started " + cmd);
    outThread_ = std::thread(&MsnBackend::stdoutLoop, this, impl);
    errThread_ = std::thread(&MsnBackend::stderrLoop, this, impl);
    return true;
}

bool MsnBackend::send(const std::string& line, Pending pend, std::string& err) {
    std::shared_ptr<Impl> impl;
    {
        std::lock_guard<std::mutex> lk(mu_);
        impl = impl_;
        if (!impl || !impl->alive.load()) {
            err = "server is not running";
            return false;
        }
        pending_.push_back(pend);  // before the write, so the reply can't beat it
    }
    const std::string msg = line + "\n";
    DWORD w = 0;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(impl->sendMu);
        ok = WriteFile(impl->in, msg.data(), static_cast<DWORD>(msg.size()), &w, nullptr) && w == msg.size();
    }
    if (!ok) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!pending_.empty()) pending_.pop_back();
        err = "could not write to the server";
        return false;
    }
    return true;
}

namespace {
bool badField(const std::string& s) { return s.find_first_of("\t\r\n") != std::string::npos; }
std::string num(double v) {
    std::ostringstream s;
    s.precision(12);
    s << v;
    return s.str();
}
}  // namespace

bool MsnBackend::requestLoad(const MsnParams& p, std::string& err) {
    if (p.runDir.empty()) {
        err = "Set the run folder";
        return false;
    }
    if (badField(p.runDir) || badField(p.ksName) || badField(p.cacheDir)) {
        err = "paths must not contain tabs or newlines";
        return false;
    }
    if (!ensureProcess(p, err)) return false;
    Pending pend{Pending::Load};
    return send("LOAD\t" + p.runDir + "\t" + p.ksName + "\t" + (p.goodOnly ? "1" : "0") + "\t" + num(p.maxRateHz) + "\t" + p.cacheDir,
                pend, err);
}

bool MsnBackend::requestRank(int label, int nbins, int minSpikes, int nUnits, std::string& err) {
    Pending pend{Pending::Rank};
    pend.label = label;
    return send("RANK\t" + std::to_string(label) + "\t" + std::to_string(std::max(2, nbins)) + "\t" +
                    std::to_string(std::max(0, minSpikes)) + "\t" + std::to_string(std::max(0, nUnits)),
                pend, err);
}

bool MsnBackend::requestFigure(int label, int unit, double pad, std::string& err) {
    bool wantSpec;
    {
        std::lock_guard<std::mutex> lk(mu_);
        wantSpec = !(specSent_ && specLabel_ == label);
    }
    Pending pend{Pending::Fig};
    pend.label = label;
    pend.unit = unit;
    return send("FIG\t" + std::to_string(label) + "\t" + std::to_string(unit) + "\t" + num(pad) + "\t" + (wantSpec ? "1" : "0"), pend,
                err);
}

// ---------------------------------------------------------------------------------------------
// Reader threads

void MsnBackend::stderrLoop(std::shared_ptr<Impl> impl) {
    PipeReader rd(impl->err);
    std::string line;
    while (rd.readLine(line))
        if (!line.empty()) log("py: " + line);
}

void MsnBackend::stdoutLoop(std::shared_ptr<Impl> impl) {
    PipeReader rd(impl->out);
    MsnResponse resp;
    std::string line;
    while (rd.readLine(line)) {
        if (line.rfind("ARR ", 0) == 0) {
            std::istringstream s(line.substr(4));
            std::string name, dtype;
            size_t count = 0;
            s >> name >> dtype >> count;
            Arr a;
            a.dtype = dtype;
            a.bytes.resize(count * (dtype == "f8" ? 8 : 4));
            if (!a.bytes.empty() && !rd.readExact(a.bytes.data(), a.bytes.size())) break;
            resp.arrs[name] = std::move(a);
        } else if (line.rfind("VAL ", 0) == 0) {
            const size_t sp = line.find(' ', 4);
            if (sp != std::string::npos) resp.vals[line.substr(4, sp - 4)] = line.substr(sp + 1);
        } else if (line.rfind("LOG ", 0) == 0) {
            log(line.substr(4));
        } else if (line.rfind("OK", 0) == 0) {
            finish(resp, true, "");
            resp = MsnResponse();
        } else if (line.rfind("ERR", 0) == 0) {
            finish(resp, false, line.size() > 4 ? line.substr(4) : "request failed");
            resp = MsnResponse();
        }
    }
    // Pipe closed: the process exited (or was killed).
    const bool wasAlive = impl->alive.exchange(false);
    bool current;
    {
        std::lock_guard<std::mutex> lk(mu_);
        current = impl_ == impl;
        if (current) pending_.clear();
    }
    if (wasAlive && current) setError("The Python server stopped unexpectedly (see the log).");
}

void MsnBackend::finish(const MsnResponse& r, bool ok, const std::string& msg) {
    Pending pend;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_.empty()) return;
        pend = pending_.front();
        pending_.pop_front();
    }
    if (!ok) {
        setError(msg);
        return;
    }

    if (pend.kind == Pending::Load) {
        auto s = std::make_shared<MsnSession>();
        auto it = r.vals.find("name");
        if (it != r.vals.end()) s->name = it->second;
        s->durationSec = r.num("duration_s");
        s->nUnits = static_cast<int>(r.num("n_units"));
        s->labels = r.i32("labels");
        s->labelCounts = r.i32("label_counts");
        {
            std::lock_guard<std::mutex> lk(mu_);
            session_ = s;
            ranking_.reset();
            spec_.reset();
            figure_.reset();
            specSent_ = false;
            specLabel_ = -1;
        }
        ++sessionVer_;
        ++rankingVer_;
        ++figureVer_;
        log("Session " + s->name + ": " + std::to_string(s->nUnits) + " units, " + std::to_string(s->labels.size()) + " syllable labels");
    } else if (pend.kind == Pending::Rank) {
        auto rk = std::make_shared<MsnRanking>();
        rk->label = pend.label;
        rk->nRenditions = static_cast<int>(r.num("n_renditions"));
        rk->medianMs = r.num("median_ms");
        const auto units = r.i32("rank_units");
        const auto var = r.f64("rank_var"), n = r.f64("rank_nspikes"), pk = r.f64("rank_peak_ms");
        for (size_t i = 0; i < units.size() && i < var.size() && i < n.size() && i < pk.size(); ++i) {
            MsnRankRow row;
            row.unit = units[i];
            row.variance = var[i];
            row.nSpikes = n[i];
            row.peakMs = pk[i];
            rk->rows.push_back(row);
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            ranking_ = rk;
        }
        ++rankingVer_;
        log("Ranked " + std::to_string(rk->rows.size()) + " units for syllable " + std::to_string(rk->label));
    } else {
        auto f = std::make_shared<MsnFigure>();
        f->label = pend.label;
        f->unit = pend.unit;
        f->nRenditions = static_cast<int>(r.num("n_renditions"));
        f->medianMs = r.num("median_ms");
        f->peakMs = r.num("peak_ms", NAN);
        f->peakChannel = static_cast<int>(r.num("peak_channel"));
        f->peakShank = static_cast<int>(r.num("peak_shank"));
        f->peakYum = r.num("peak_y_um");
        f->endMs = r.f32("end_ms");
        f->spikeMs = r.f32("spike_ms");
        f->spikeRendition = r.i32("spike_rendition");
        f->wfT = r.f32("wf_t_ms");
        f->wf = r.f32("wf");
        f->fpXY = r.f32("fp_xy");
        f->fpShank = r.i32("fp_shank");
        f->fpAmp = r.f32("fp_amp");
        std::shared_ptr<MsnSpec> sp;
        if (r.arrs.count("spec_db")) {
            sp = std::make_shared<MsnSpec>();
            sp->label = pend.label;
            sp->freqHz = r.f32("spec_freq_hz");
            sp->timeFrac = r.f32("spec_time_frac");
            sp->db = r.f32("spec_db");
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (sp) {
                spec_ = sp;
                specSent_ = true;
                specLabel_ = pend.label;
            }
            figure_ = f;
        }
        ++figureVer_;
    }
}

}  // namespace fp

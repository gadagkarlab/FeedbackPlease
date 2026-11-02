#include "app/Widgets.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

#include "imgui_stdlib.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace fp::ui {

const ImVec4 kGreen(0.30f, 0.80f, 0.35f, 1.0f);
const ImVec4 kRed(0.90f, 0.30f, 0.30f, 1.0f);
const ImVec4 kAmber(0.95f, 0.70f, 0.20f, 1.0f);
const ImVec4 kBlue(0.35f, 0.60f, 1.00f, 1.0f);
const ImVec4 kPurple(0.65f, 0.45f, 1.00f, 1.0f);
const ImVec4 kGrey(0.60f, 0.60f, 0.60f, 1.0f);

bool& changed() {
    static bool c = false;
    return c;
}

bool InputD(const char* label, double* v, const char* fmt, float widthEm) {
    ImGui::SetNextItemWidth(widthEm * em());
    const bool r = ImGui::InputDouble(label, v, 0.0, 0.0, fmt);
    changed() |= r;
    return r;
}

bool InputI(const char* label, int* v, float widthEm) {
    ImGui::SetNextItemWidth(widthEm * em());
    const bool r = ImGui::InputInt(label, v, 0, 0);
    changed() |= r;
    return r;
}

bool Check(const char* label, bool* v) {
    const bool r = ImGui::Checkbox(label, v);
    changed() |= r;
    return r;
}

bool InputStr(const char* label, std::string* s, float widthEm) {
    ImGui::SetNextItemWidth(widthEm * em());
    const bool r = ImGui::InputText(label, s);
    changed() |= r;
    return r;
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// The common item dialogs (Vista and later): the same Explorer-style window as other apps,
// with sidebar, address bar and search; folders are picked in the same window.
enum class DialogKind { Open, Save, Folder };

std::string runDialog(DialogKind kind, const char* filter, const char* defaultExt, const char* title,
                      const std::string& start) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string result;
    IFileDialog* dlg = nullptr;
    const HRESULT made = kind == DialogKind::Save
                             ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(
                                                    reinterpret_cast<IFileSaveDialog**>(&dlg)))
                             : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(
                                                    reinterpret_cast<IFileOpenDialog**>(&dlg)));
    if (SUCCEEDED(made)) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;  // keep the working dir for relative paths
        if (kind == DialogKind::Open) opts |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        if (kind == DialogKind::Save) opts |= FOS_OVERWRITEPROMPT;
        if (kind == DialogKind::Folder) opts |= FOS_PICKFOLDERS;
        dlg->SetOptions(opts);
        if (title) dlg->SetTitle(widen(title).c_str());

        // "Name\0pattern\0Name\0pattern\0\0" -> filter list.
        std::vector<std::wstring> parts;
        for (const char* p = filter; p && *p; p += std::strlen(p) + 1) parts.push_back(widen(p));
        std::vector<COMDLG_FILTERSPEC> specs;
        for (size_t i = 0; i + 1 < parts.size(); i += 2) specs.push_back({parts[i].c_str(), parts[i + 1].c_str()});
        if (!specs.empty()) dlg->SetFileTypes(UINT(specs.size()), specs.data());
        if (defaultExt) dlg->SetDefaultExtension(widen(defaultExt).c_str());

        // Start in the current value's folder (and suggest its name when saving).
        if (!start.empty()) {
            std::error_code ec;
            std::filesystem::path sp = std::filesystem::u8path(start);
            if (!std::filesystem::is_directory(sp, ec)) {
                if (kind == DialogKind::Save && sp.has_filename()) dlg->SetFileName(sp.filename().wstring().c_str());
                sp = sp.parent_path();
            }
            if (!sp.empty() && std::filesystem::is_directory(sp, ec)) {
                IShellItem* folder = nullptr;
                const std::wstring abs = std::filesystem::absolute(sp, ec).wstring();
                if (SUCCEEDED(SHCreateItemFromParsingName(abs.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                    dlg->SetFolder(folder);
                    folder->Release();
                }
            }
        }

        // Owned by the app window: modal, and it can't open behind it.
        if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = narrow(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

}  // namespace

std::string openFileDialog(const char* filter, const std::string& start) {
    return runDialog(DialogKind::Open, filter, nullptr, nullptr, start);
}

std::string saveFileDialog(const char* filter, const char* defaultExt, const std::string& start) {
    return runDialog(DialogKind::Save, filter, defaultExt, nullptr, start);
}

std::string pickFolderDialog(const char* title, const std::string& start) {
    return runDialog(DialogKind::Folder, nullptr, nullptr, title, start);
}
#else
std::string openFileDialog(const char*, const std::string&) { return {}; }
std::string saveFileDialog(const char*, const char*, const std::string&) { return {}; }
std::string pickFolderDialog(const char*, const std::string&) { return {}; }
#endif

bool PathInput(const char* label, std::string* s, float widthEm) {
    ImGui::PushID(label);
    bool r = InputStr("##path", s, widthEm);
    ImGui::SameLine();
    if (ImGui::Button("...")) {
        const std::string f = openFileDialog("WAV files (*.wav)\0*.wav\0All files\0*.*\0", *s);
        if (!f.empty()) {
            *s = f;
            r = changed() = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
        s->clear();
        r = changed() = true;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return r;
}

void decimate(const float* y, size_t n, double x0, double rate, int buckets, std::vector<double>& xs,
              std::vector<double>& ys) {
    xs.clear();
    ys.clear();
    if (n == 0 || rate <= 0 || buckets <= 0) return;
    auto tx = [&](size_t i) { return x0 + double(i) / rate; };
    if (n <= size_t(2 * buckets)) {
        for (size_t i = 0; i < n; ++i) {
            xs.push_back(tx(i));
            ys.push_back(y[i]);
        }
        return;
    }
    const double per = double(n) / buckets;
    for (int b = 0; b < buckets; ++b) {
        const size_t a = size_t(b * per), e = std::min(n, size_t((b + 1) * per));
        if (a >= e) continue;
        float lo = y[a], hi = y[a];
        size_t ilo = a, ihi = a;
        for (size_t i = a; i < e; ++i) {
            if (y[i] < lo) lo = y[i], ilo = i;
            if (y[i] > hi) hi = y[i], ihi = i;
        }
        // Keep time order so the trace doesn't zig-zag backwards.
        const bool loFirst = ilo <= ihi;
        xs.push_back(tx(loFirst ? ilo : ihi));
        ys.push_back(loFirst ? lo : hi);
        xs.push_back(tx(loFirst ? ihi : ilo));
        ys.push_back(loFirst ? hi : lo);
    }
}

}  // namespace fp::ui

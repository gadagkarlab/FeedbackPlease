#pragma once
// ImGui helpers shared by the live and tuning views. Every editing helper sets
// changed() so the GUI knows to push new parameters to the engine.

#include <string>
#include <vector>

#include "imgui.h"

namespace fp::ui {

bool& changed();
inline float em() { return ImGui::GetFontSize(); }

bool InputD(const char* label, double* v, const char* fmt = "%.6g", float widthEm = 6.0f);
bool InputI(const char* label, int* v, float widthEm = 6.0f);
bool Check(const char* label, bool* v);
bool InputStr(const char* label, std::string* s, float widthEm = 14.0f);
void HelpMarker(const char* text);
// Text box + browse button for a WAV file.
bool PathInput(const char* label, std::string* s, float widthEm = 16.0f);

template <class E>
bool EnumCombo(const char* label, E* v, int count, float widthEm = 10.0f) {
    bool r = false;
    ImGui::SetNextItemWidth(widthEm * em());
    if (ImGui::BeginCombo(label, toString(*v))) {
        for (int i = 0; i < count; ++i) {
            const E e = static_cast<E>(i);
            if (ImGui::Selectable(toString(e), e == *v)) {
                *v = e;
                r = true;
            }
        }
        ImGui::EndCombo();
    }
    changed() |= r;
    return r;
}

// Native dialogs (the standard Windows Explorer-style ones); return "" when cancelled or
// unsupported. filter: "Name\0*.ext\0Name\0*.*\0" pairs. start: a file or folder to open
// at (its folder is used for a file; ignored if it doesn't exist). Paths are UTF-8.
std::string openFileDialog(const char* filter, const std::string& start = {});
std::string saveFileDialog(const char* filter, const char* defaultExt, const std::string& start = {});
std::string pickFolderDialog(const char* title, const std::string& start = {});

// Min/max decimation so long traces draw as ~2 points per pixel column.
// x of sample i = x0 + i / rate.
void decimate(const float* y, size_t n, double x0, double rate, int buckets, std::vector<double>& xs,
              std::vector<double>& ys);

extern const ImVec4 kGreen, kRed, kAmber, kBlue, kPurple, kGrey;

}  // namespace fp::ui

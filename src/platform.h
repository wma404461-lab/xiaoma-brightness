#pragma once
#include "display.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace Platform {
inline void EnableDpi() {
    using Function = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto function = reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (function) function(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    else SetProcessDPIAware();
}
inline UINT WindowDpi(HWND window) {
    using Function = UINT(WINAPI*)(HWND);
    auto function = reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (function) { UINT dpi = function(window); if (dpi) return dpi; }
    HDC dc = GetDC(window); UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(window, dc); return dpi ? dpi : 96;
}
inline int SmallIconSize(UINT dpi) {
    using Function = int(WINAPI*)(int, UINT);
    auto function = reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    return function ? function(SM_CXSMICON, dpi) : GetSystemMetrics(SM_CXSMICON);
}
struct PanelLayout { int x = 0, y = 0, width = 0, height = 0; float scale = 1; bool valid = false; };
inline PanelLayout PlacePanel(const RECT& work, UINT dpi) {
    PanelLayout result;
    const int width = work.right - work.left, height = work.bottom - work.top;
    if (width <= 0 || height <= 0) return result;
    const int gap = std::min(8, std::max(0, height - 1));
    result.scale = std::min({(dpi ? dpi : 96) / 96.0f, width / 344.0f, (height - gap) / 120.0f});
    result.width = std::min(width, std::max(1, static_cast<int>(std::lround(344 * result.scale))));
    result.height = std::min(height - gap, std::max(1, static_cast<int>(std::lround(120 * result.scale))));
    result.x = work.right - result.width; result.y = work.bottom - result.height - gap; result.valid = true;
    return result;
}
inline BYTE OverlayAlpha(int percent) {
    return static_cast<BYTE>(std::min(242, ((100 - std::clamp(percent, 0, 100)) * 255 + 50) / 100));
}
inline int PointerValue(int pixelX, float scale) {
    if (!std::isfinite(scale) || scale <= 0) scale = 1;
    float fraction = (pixelX / scale + 18 - 28) / (316 - 28);
    return static_cast<int>(std::lround(std::clamp(fraction, 0.0f, 1.0f) * 100));
}
inline std::wstring OverlayKey(const RECT& bounds) {
    return std::to_wstring(bounds.left) + L":" + std::to_wstring(bounds.top) + L":" + std::to_wstring(bounds.right) + L":" + std::to_wstring(bounds.bottom);
}
// Cloned monitors share desktop pixels and must receive exactly one software layer.
inline std::map<std::wstring, RECT> SoftwareRegions(const Displays& displays, const std::vector<bool>& software) {
    std::map<std::wstring, RECT> regions;
    for (size_t i = 0; i < displays.size(); ++i) if (i < software.size() && software[i]) {
        const auto& bounds = displays[i]->bounds;
        if (bounds.right > bounds.left && bounds.bottom > bounds.top) regions[OverlayKey(bounds)] = bounds;
    }
    return regions;
}
}

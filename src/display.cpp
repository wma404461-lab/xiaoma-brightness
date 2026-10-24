#include "display.h"
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <iomanip>
#include <cwctype>

// Physical-monitor handles are opaque tokens: zero is a valid returned handle.
Display::~Display() { if (physicalValid) DestroyPhysicalMonitor(physical); }
int ToPercent(DWORD raw, DWORD minimum, DWORD maximum) {
    if (maximum <= minimum) return 100;
    if (raw <= minimum) return 0;
    if (raw >= maximum) return 100;
    const double fraction = (static_cast<double>(raw) - minimum) / (maximum - minimum);
    return std::clamp(static_cast<int>(std::lround(fraction * 100.0)), 0, 100);
}
DWORD ToRaw(int percent, DWORD minimum, DWORD maximum) {
    if (maximum <= minimum) return minimum;
    return minimum + static_cast<DWORD>(std::llround(static_cast<double>(maximum - minimum) * std::clamp(percent, 0, 100) / 100.0));
}
DWORD NativeRaw(const Display& display, int percent) {
    DWORD raw = ToRaw(percent, display.minimum, display.maximum);
    if (display.backend == Backend::Wmi && !display.levels.empty()) {
        const auto best = std::min_element(display.levels.begin(), display.levels.end(), [raw](DWORD a, DWORD b) {
            return std::abs(static_cast<long>(a) - static_cast<long>(raw)) < std::abs(static_cast<long>(b) - static_cast<long>(raw));
        });
        raw = *best;
    }
    return raw;
}
int ClampPercent(int value, Backend backend) { return std::clamp(value, backend == Backend::Soft ? 5 : 0, 100); }
std::wstring BackendName(Backend backend) {
    switch (backend) { case Backend::Vcp: return L"DDC/CI"; case Backend::HighLevel: return L"Monitor API"; case Backend::Wmi: return L"WMI"; default: return L"画面调暗"; }
}
template<class T> void Release(T*& value) { if (value) { value->Release(); value = nullptr; } }
struct WmiDisplay { std::wstring instance, path; int brightness = 100; std::vector<DWORD> levels; };
struct DisplayControl::Wmi {
    IWbemServices* services = nullptr;
    IWbemClassObject* inputClass = nullptr;
    std::vector<WmiDisplay> records;
    bool com = false;
    Wmi() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return;
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
        IWbemLocator* locator = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))) return;
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        const HRESULT connect = locator->ConnectServer(ns, nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &services);
        SysFreeString(ns); Release(locator);
        if (FAILED(connect)) return;
        CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        IWbemClassObject* cls = nullptr;
        BSTR className = SysAllocString(L"WmiMonitorBrightnessMethods");
        if (SUCCEEDED(services->GetObject(className, 0, nullptr, &cls, nullptr))) {
            cls->GetMethod(L"WmiSetBrightness", 0, &inputClass, nullptr);
            Release(cls);
        }
        SysFreeString(className);
    }
    ~Wmi() { Release(inputClass); Release(services); if (com) CoUninitialize(); }
    void Refresh() {
        records.clear(); if (!services) return;
        IEnumWbemClassObject* objects = nullptr;
        BSTR lang = SysAllocString(L"WQL"), query = SysAllocString(L"SELECT InstanceName,CurrentBrightness,Level FROM WmiMonitorBrightness WHERE Active=TRUE");
        const HRESULT hr = services->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &objects);
        SysFreeString(lang); SysFreeString(query);
        if (FAILED(hr)) return;
        IWbemClassObject* obj = nullptr; ULONG count = 0;
        while (objects->Next(1500, 1, &obj, &count) == S_OK && count) {
            VARIANT instance{}, brightness{}, normalized{}, levels{};
            obj->Get(L"InstanceName", 0, &instance, nullptr, nullptr); obj->Get(L"CurrentBrightness", 0, &brightness, nullptr, nullptr);
            obj->Get(L"Level", 0, &levels, nullptr, nullptr);
            bool valid = SUCCEEDED(VariantChangeType(&normalized, &brightness, 0, VT_UI4)) && normalized.ulVal <= 100;
            if (valid && instance.vt == VT_BSTR && instance.bstrVal) {
                WmiDisplay record; record.instance = instance.bstrVal;
                record.brightness = static_cast<int>(normalized.ulVal);
                if (levels.vt == (VT_ARRAY | VT_UI1) && levels.parray) {
                    LONG begin = 0, end = -1; BYTE* values = nullptr;
                    SafeArrayGetLBound(levels.parray, 1, &begin); SafeArrayGetUBound(levels.parray, 1, &end);
                    if (end >= begin && end - begin < 256 && SUCCEEDED(SafeArrayAccessData(levels.parray, reinterpret_cast<void**>(&values)))) {
                        for (LONG i = 0; i <= end - begin; ++i) if (values[i] <= 100) record.levels.push_back(values[i]);
                        SafeArrayUnaccessData(levels.parray);
                        std::sort(record.levels.begin(), record.levels.end()); record.levels.erase(std::unique(record.levels.begin(), record.levels.end()), record.levels.end());
                    }
                }
                // WMI object paths require backslash and quote escaping.
                record.path = L"WmiMonitorBrightnessMethods.InstanceName=\"";
                for (wchar_t c : record.instance) { if (c == L'\\' || c == L'\"') record.path += L'\\'; record.path += c; }
                record.path += L'\"'; records.push_back(std::move(record));
            }
            VariantClear(&instance); VariantClear(&brightness); VariantClear(&normalized); VariantClear(&levels); Release(obj);
        }
        Release(objects);
    }
    bool Set(const std::wstring& path, DWORD value) {
        if (!services || !inputClass) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        IWbemClassObject* params = nullptr; IWbemClassObject* output = nullptr;
        if (FAILED(inputClass->SpawnInstance(0, &params))) { SetLastError(ERROR_INVALID_FUNCTION); return false; }
        VARIANT timeout{}, brightness{}; timeout.vt = VT_I4; timeout.lVal = 0; brightness.vt = VT_UI1; brightness.bVal = static_cast<BYTE>(std::min(value, 100ul));
        params->Put(L"Timeout", 0, &timeout, 0); params->Put(L"Brightness", 0, &brightness, 0);
        BSTR object = SysAllocString(path.c_str()), method = SysAllocString(L"WmiSetBrightness");
        const HRESULT hr = services->ExecMethod(object, method, 0, nullptr, params, &output, nullptr);
        SysFreeString(object); SysFreeString(method); Release(params);
        bool success = false;
        if (SUCCEEDED(hr) && output) { VARIANT result{}; if (SUCCEEDED(output->Get(L"ReturnValue", 0, &result, nullptr, nullptr))) success = (result.vt == VT_I4 && result.lVal == 0) || (result.vt == VT_UI4 && result.ulVal == 0); VariantClear(&result); }
        Release(output); if (!success) SetLastError(ERROR_WRITE_FAULT); return success;
    }
};
DisplayControl::DisplayControl() = default;
DisplayControl::~DisplayControl() = default;
struct DisplayIdentity { std::wstring name, key; };
namespace {
uint64_t TopologyHash(const std::map<std::wstring,std::pair<HMONITOR,RECT>>& records) {
    uint64_t hash=14695981039346656037ull;
    auto mix=[&](uint64_t value){hash^=value;hash*=1099511628211ull;};
    for(const auto& record:records){
        for(wchar_t c:record.first)mix(static_cast<unsigned>(c));
        mix(reinterpret_cast<UINT_PTR>(record.second.first));
        const auto& r=record.second.second;
        mix(static_cast<DWORD>(r.left));mix(static_cast<DWORD>(r.top));mix(static_cast<DWORD>(r.right));mix(static_cast<DWORD>(r.bottom));
    }
    mix(records.size());return hash;
}
}
uint64_t DisplayTopology(const Displays& displays) {
    std::map<std::wstring,std::pair<HMONITOR,RECT>> records;
    for(const auto& display:displays)records[display->device]={display->logical,display->bounds};
    return TopologyHash(records);
}
uint64_t CurrentDisplayTopology() {
    std::map<std::wstring,std::pair<HMONITOR,RECT>> records;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL{
        MONITORINFOEXW info{};info.cbSize=sizeof(info);
        if(GetMonitorInfoW(monitor,&info))(*reinterpret_cast<decltype(records)*>(data))[info.szDevice]={monitor,info.rcMonitor};
        return TRUE;
    },reinterpret_cast<LPARAM>(&records));
    return TopologyHash(records);
}
std::map<std::wstring, DisplayIdentity> Identities() {
    std::map<std::wstring, DisplayIdentity> identities;
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount); std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        LONG status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) continue;
        if (status != ERROR_SUCCESS) break;
        for (UINT32 i = 0; i < pathCount; ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{}; source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; source.header.size = sizeof(source); source.header.adapterId = paths[i].sourceInfo.adapterId; source.header.id = paths[i].sourceInfo.id;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{}; target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME; target.header.size = sizeof(target); target.header.adapterId = paths[i].targetInfo.adapterId; target.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) identities[source.viewGdiDeviceName] = {target.monitorFriendlyDeviceName, target.monitorDevicePath};
        }
        break;
    }
    return identities;
}
static std::wstring NormalizeKey(std::wstring key) {
    if (key.compare(0, 4, L"\\\\?\\") == 0) key.erase(0, 4);
    size_t guid = key.find(L"#{"); if (guid != std::wstring::npos) key.erase(guid);
    std::replace(key.begin(), key.end(), L'#', L'\\');
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); }); return key;
}
bool MatchesWmiInstance(const std::wstring& key, const std::wstring& instance) {
    const auto normalized = NormalizeKey(key);
    if (normalized.empty() || normalized.rfind(L"DISPLAY\\", 0) != 0) return false;
    auto record = NormalizeKey(instance);
    const size_t suffix = record.rfind(L'_');
    if (suffix != std::wstring::npos && suffix + 1 < record.size() && std::all_of(record.begin() + suffix + 1, record.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) record.erase(suffix);
    return record == normalized;
}
Displays DisplayControl::Scan() {
    if (!wmi) wmi = std::make_unique<Wmi>();
    wmi->Refresh(); auto identities = Identities();
    std::vector<HMONITOR> logical;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL { reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor); return TRUE; }, reinterpret_cast<LPARAM>(&logical));
    Displays result;
    for (HMONITOR handle : logical) {
        MONITORINFOEXW info{}; info.cbSize = sizeof(info); if (!GetMonitorInfoW(handle, &info)) continue;
        auto identity = identities.find(info.szDevice);
        std::wstring name = identity != identities.end() ? identity->second.name : L"";
        std::wstring key = identity != identities.end() ? identity->second.key : info.szDevice;
        if (key.empty()) key = info.szDevice;
        if (name.empty()) name = L"显示器";
        DWORD physicalCount = 0;
        std::vector<PHYSICAL_MONITOR> physical;
        BOOL numberOK = GetNumberOfPhysicalMonitorsFromHMONITOR(handle, &physicalCount);
        std::wstring enumeration = L"numberOK=" + std::to_wstring(numberOK) + L",count=" + std::to_wstring(physicalCount) + L",error=" + std::to_wstring(numberOK ? 0 : GetLastError()) + L"; ";
        if (numberOK && physicalCount && physicalCount <= 16) {
            physical.resize(physicalCount);
            if (!GetPhysicalMonitorsFromHMONITOR(handle, physicalCount, physical.data())) { enumeration += L"handlesError=" + std::to_wstring(GetLastError()) + L"; "; physical.clear(); }
        }
        const WmiDisplay* wmiRecord = nullptr;
        for (const auto& record : wmi->records) if (MatchesWmiInstance(key, record.instance)) { wmiRecord = &record; break; }
        const size_t count = std::max<size_t>(physical.size(), 1);
        for (size_t i = 0; i < count; ++i) {
            auto monitor = std::make_shared<Display>(); monitor->logical = handle; monitor->device = info.szDevice; monitor->bounds = info.rcMonitor; monitor->primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0; monitor->name = name; monitor->key = key;
            monitor->diagnostics = enumeration;
            if (count > 1) { monitor->name += L" · " + std::to_wstring(i + 1); monitor->key += L":" + std::to_wstring(i); }
            if (i < physical.size()) { monitor->physical = physical[i].hPhysicalMonitor; monitor->physicalValid = true; monitor->diagnostics += L"handle=" + std::to_wstring(reinterpret_cast<UINT_PTR>(monitor->physical)) + L"; "; }
            DWORD current = 100, maximum = 0, minimum = 0;
            if (wmiRecord) { monitor->backend = Backend::Wmi; monitor->wmiPath = wmiRecord->path; monitor->levels = wmiRecord->levels; current = static_cast<DWORD>(wmiRecord->brightness); maximum = 100; }
            else if (monitor->physicalValid) {
                BOOL readable = FALSE;
                for (int attempt = 0; attempt < 3; ++attempt) {
                    readable = GetVCPFeatureAndVCPFeatureReply(monitor->physical, 0x10, nullptr, &current, &maximum);
                    const DWORD error = readable ? ERROR_SUCCESS : GetLastError();
                    monitor->diagnostics += L"attempt=" + std::to_wstring(attempt + 1) + L",ok=" + std::to_wstring(readable) + L",error=" + std::to_wstring(error) + L",current=" + std::to_wstring(current) + L",maximum=" + std::to_wstring(maximum) + L"; ";
                    if (readable && maximum > 0 && current <= maximum) break;
                    if (attempt < 2) Sleep(120 * (attempt + 1));
                }
                if (readable && maximum > 0 && current <= maximum) monitor->backend = Backend::Vcp;
                else if (GetMonitorBrightness(monitor->physical, &minimum, &current, &maximum) && maximum > minimum && current <= maximum) monitor->backend = Backend::HighLevel;
                else { monitor->backend = Backend::Soft; minimum = 0; current = maximum = 100; }
            } else { current = maximum = 100; }
            monitor->minimum = minimum; monitor->maximum = maximum; monitor->originalRaw = current; monitor->lastRaw = current;
            monitor->original = ToPercent(current, minimum, maximum); monitor->desired = monitor->original; monitor->applied = monitor->original; monitor->verified = monitor->backend != Backend::Soft;
            result.push_back(std::move(monitor));
        }
    }
    // The primary screen comes first; others follow their physical left-to-right position.
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { if (a->primary != b->primary) return a->primary; if (a->bounds.left != b->bounds.left) return a->bounds.left < b->bounds.left; return a->bounds.top < b->bounds.top; });
    if (wmi->records.empty()) { wmi.reset(); CoFreeUnusedLibraries(); }
    return result;
}
bool DisplayControl::Read(Display& monitor, DWORD& raw) {
    switch (monitor.backend) {
    case Backend::Vcp: { DWORD maximum = 0; return GetVCPFeatureAndVCPFeatureReply(monitor.physical, 0x10, nullptr, &raw, &maximum) && maximum > 0; }
    case Backend::HighLevel: { DWORD lo = 0, hi = 0; return GetMonitorBrightness(monitor.physical, &lo, &raw, &hi); }
    case Backend::Wmi: { if (!wmi) wmi = std::make_unique<Wmi>(); wmi->Refresh(); for (const auto& record : wmi->records) if (record.path == monitor.wmiPath) { raw = static_cast<DWORD>(record.brightness); return true; } SetLastError(ERROR_NOT_FOUND); return false; }
    default: raw = static_cast<DWORD>(monitor.applied.load()); return true;
    }
}
bool DisplayControl::Write(Display& monitor, DWORD raw) {
    switch (monitor.backend) {
    case Backend::Vcp: return SetVCPFeature(monitor.physical, 0x10, raw) != FALSE;
    case Backend::HighLevel: return SetMonitorBrightness(monitor.physical, raw) != FALSE;
    case Backend::Wmi: if (!wmi) wmi = std::make_unique<Wmi>(); return wmi->Set(monitor.wmiPath, raw);
    default: return true;
    }
}
static std::wstring JsonString(const std::wstring& value) {
    std::wstring s = L"\""; for (wchar_t c : value) { if (c == L'\\' || c == L'\"') s += L'\\'; if (c == L'\n') s += L"\\n"; else if (c == L'\r') s += L"\\r"; else s += c; } return s + L'\"';
}
bool WriteProbe(const std::wstring& path, bool testHardware) {
    DisplayControl control; auto displays = control.Scan(); std::wstringstream report;
    report << L"{\n  \"displays\": ["; bool first = true, allPassed = !displays.empty();
    for (const auto& monitor : displays) {
        DWORD before = monitor->originalRaw, after = before, roundtrip = before, requested = before;
        bool readOK = monitor->backend != Backend::Soft, writeOK = false, restored = true, changedOK = false;
        DWORD failure = 0;
        if (testHardware && monitor->backend != Backend::Soft) {
            readOK = control.Read(*monitor, before);
            if (readOK) {
                requested = before < monitor->maximum ? before + 1 : (before > monitor->minimum ? before - 1 : before);
                // Always attempt restoration, even if writing/reading the temporary value fails.
                writeOK = control.Write(*monitor, requested); if (!writeOK) failure = GetLastError();
                Sleep(100); changedOK = control.Read(*monitor, roundtrip) && roundtrip == requested;
                bool restoreWrite = control.Write(*monitor, before);
                Sleep(100); restored = restoreWrite && control.Read(*monitor, after) && after == before;
                if (!restored) { control.Write(*monitor, before); Sleep(150); restored = control.Read(*monitor, after) && after == before; }
            }
            allPassed = allPassed && readOK && writeOK && changedOK && restored;
        }
        if (!first) report << L","; first = false;
        report << L"\n    {\"name\": " << JsonString(monitor->name) << L", \"device\": " << JsonString(monitor->device) << L", \"backend\": " << JsonString(BackendName(monitor->backend)) << L", \"brightness\": " << monitor->original << L", \"primary\": " << (monitor->primary ? L"true" : L"false") << L", \"minimum\": " << monitor->minimum << L", \"maximum\": " << monitor->maximum << L", \"diagnostics\": " << JsonString(monitor->diagnostics);
        if (testHardware) report << L", \"readOK\": " << (readOK ? L"true" : L"false") << L", \"writeOK\": " << (writeOK ? L"true" : L"false") << L", \"roundtripOK\": " << (changedOK ? L"true" : L"false") << L", \"restored\": " << (restored ? L"true" : L"false") << L", \"before\": " << before << L", \"requested\": " << requested << L", \"readBack\": " << roundtrip << L", \"after\": " << after << L", \"error\": " << failure;
        report << L"}";
    }
    report << L"\n  ], \"passed\": " << (allPassed ? L"true" : L"false") << L"\n}\n";
    const auto content = report.str(); const int size = WideCharToMultiByte(CP_UTF8, 0, content.data(), static_cast<int>(content.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0'); WideCharToMultiByte(CP_UTF8, 0, content.data(), static_cast<int>(content.size()), utf8.data(), size, nullptr, nullptr);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; bool saved = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) && written == utf8.size(); CloseHandle(file); return saved && allPassed;
}

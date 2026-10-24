#pragma once
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

enum class Backend { Vcp, HighLevel, Wmi, Soft };
struct Display {
    std::wstring key, name, device, wmiPath, diagnostics;
    HMONITOR logical = nullptr;
    HANDLE physical = nullptr;
    bool physicalValid = false;
    RECT bounds{};
    Backend backend = Backend::Soft;
    bool primary = false;
    DWORD minimum = 0, maximum = 100, originalRaw = 100;
    int original = 100;
    std::vector<DWORD> levels;
    std::atomic<int> desired{100}, applied{100};
    std::atomic<unsigned> revision{0}, completed{0};
    std::atomic<DWORD> error{0};
    std::atomic<bool> verified{false};
    DWORD lastRaw = 100;
    ULONGLONG lastWrite = 0;
    ~Display();
};
using Displays = std::vector<std::shared_ptr<Display>>;
int ToPercent(DWORD raw, DWORD minimum, DWORD maximum);
DWORD ToRaw(int percent, DWORD minimum, DWORD maximum);
DWORD NativeRaw(const Display& display, int percent);
bool MatchesWmiInstance(const std::wstring& displayKey, const std::wstring& instance);
int ClampPercent(int value, Backend backend);
std::wstring BackendName(Backend backend);
uint64_t DisplayTopology(const Displays& displays);
uint64_t CurrentDisplayTopology();

// All slow monitor I/O lives on this thread (or in the explicit CLI diagnostic).
class DisplayControl {
public:
    DisplayControl();
    ~DisplayControl();
    Displays Scan();
    bool Read(Display& display, DWORD& raw);
    bool Write(Display& display, DWORD raw);
private:
    struct Wmi;
    std::unique_ptr<Wmi> wmi;
};
bool WriteProbe(const std::wstring& path, bool testHardware);

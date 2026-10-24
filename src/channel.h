#pragma once
#include "display.h"
#include <functional>

// An I/O adapter is created and destroyed on its device's thread (important for WMI COM).
class BrightnessIo {
public:
    virtual ~BrightnessIo() = default;
    virtual bool Set(int percent, bool force, DWORD& error) = 0;
    virtual bool Verify(int percent, DWORD& error) = 0;
};
struct ChannelSnapshot {
    int wanted = 100, applied = 100;
    uint64_t requested = 0, completed = 0, verified = 0;
    DWORD error = 0;
    bool software = false, finished = false, inFlight = false, needsDiscovery = false;
    uint64_t writes = 0, reads = 0;
    double lastWriteMs = 0;
};
class BrightnessChannel {
public:
    using Factory = std::function<std::unique_ptr<BrightnessIo>()>;
    using Notification = std::function<void()>;
    BrightnessChannel(int initial, bool software, Factory factory, Notification notify = {});
    ~BrightnessChannel();
    BrightnessChannel(const BrightnessChannel&) = delete;
    BrightnessChannel& operator=(const BrightnessChannel&) = delete;
    uint64_t Submit(int percent, bool force = false);
    void Refresh();
    void Revalidate();
    bool CheckTimeout(DWORD limitMs = 1000);
    ChannelSnapshot Snapshot() const;
    bool Stop(DWORD timeoutMs = 0);
private:
    struct State;
    std::shared_ptr<State> state;
    HANDLE thread = nullptr;
    static unsigned __stdcall Run(void* context);
};
BrightnessChannel::Factory NativeBrightnessFactory(std::shared_ptr<Display> monitor);
bool RunStressTests(const std::wstring& path);
bool RunHardwareBenchmark(const std::wstring& path);
bool WriteUtf8File(const std::wstring& path, const std::string& text);

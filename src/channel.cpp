#include "channel.h"
#include <process.h>
#include <algorithm>
#include <chrono>

namespace {
uint64_t Pack(uint64_t generation, int value, bool force) {
    return (generation << 8) | static_cast<unsigned>(std::clamp(value, 0, 100)) | (force ? 128ull : 0ull);
}
int Percent(uint64_t command) { return static_cast<int>(command & 127); }
uint64_t Revision(uint64_t command) { return command >> 8; }
double Milliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}
struct BrightnessChannel::State {
    alignas(8) std::atomic<uint64_t> requested{0}, completed{0}, verified{0};
    std::atomic<bool> stopping{false}, refresh{false}, software{false}, finished{false};
    std::atomic<bool> stalled{false}, inFlight{false};
    std::atomic<bool> revalidate{false}, needsDiscovery{false};
    std::atomic<uint64_t> callStarted{0};
    std::atomic<DWORD> error{0};
    std::atomic<uint64_t> errorCommand{0};
    std::atomic<int> applied{100};
    std::atomic<uint64_t> writes{0}, reads{0}, writeMicros{0};
    HANDLE event = nullptr;
    Factory factory;
    Notification notify;
    bool nativeSoftware = false;
    State() { event = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
    ~State() { if (event) CloseHandle(event); }
    void Notify() { if (notify) { try { notify(); } catch (...) {} } }
};
BrightnessChannel::BrightnessChannel(int initial, bool software, Factory factory, Notification notify)
    : state(std::make_shared<State>()) {
    initial = std::clamp(initial, 0, 100);
    state->requested = state->completed = state->verified = Pack(1, initial, false);
    state->applied = initial; state->nativeSoftware = software; state->software = software;
    state->factory = std::move(factory); state->notify = std::move(notify);
    if (software) { state->finished = true; return; }
    auto argument = new std::shared_ptr<State>(state);
    thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 256 * 1024, Run, argument, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    if (!thread) { delete argument; state->software = true; state->error = ERROR_NOT_ENOUGH_MEMORY; state->finished = true; }
}
BrightnessChannel::~BrightnessChannel() { Stop(0); if (thread) CloseHandle(thread); }
uint64_t BrightnessChannel::Submit(int percent, bool force) {
    percent = std::clamp(percent, 0, 100);
    uint64_t previous = state->requested.load(std::memory_order_acquire), next;
    do {
        if (Percent(previous) == percent && !force && !state->error) return Revision(previous);
        next = Pack(Revision(previous) + 1, percent, force);
    } while (!state->requested.compare_exchange_weak(previous, next, std::memory_order_acq_rel));
    if (state->finished && state->software) {
        state->applied = percent; state->completed = state->verified = next;
        state->error = 0; state->Notify(); return Revision(next);
    }
    SetEvent(state->event);
    return Revision(next);
}
void BrightnessChannel::Refresh() { state->refresh = true; SetEvent(state->event); }
void BrightnessChannel::Revalidate() { if(!state->nativeSoftware&&!state->finished){state->revalidate=true;SetEvent(state->event);} }
bool BrightnessChannel::CheckTimeout(DWORD limitMs) {
    if (state->inFlight && GetTickCount64() - state->callStarted.load() >= limitMs) {
        if (!state->stalled.exchange(true)) state->Notify();
    }
    return state->stalled;
}
ChannelSnapshot BrightnessChannel::Snapshot() const {
    ChannelSnapshot result;
    const uint64_t requested = state->requested.load(std::memory_order_acquire);
    result.wanted = Percent(requested); result.requested = Revision(requested);
    const uint64_t completed = state->completed.load();
    result.applied = Percent(completed); result.completed = Revision(completed);
    result.verified = Revision(state->verified.load());
    result.error = state->errorCommand.load() == requested ? state->error.load() : 0;
    result.software = state->software.load() || state->stalled.load(); result.finished = state->finished.load(); result.inFlight = state->inFlight.load();
    result.needsDiscovery=state->needsDiscovery.load()||(!state->nativeSoftware&&state->software.load());
    result.writes = state->writes.load(); result.reads = state->reads.load();
    result.lastWriteMs = state->writeMicros.load() / 1000.0;
    return result;
}
bool BrightnessChannel::Stop(DWORD timeoutMs) {
    state->stopping = true; SetEvent(state->event);
    return !thread || WaitForSingleObject(thread, timeoutMs) == WAIT_OBJECT_0;
}
unsigned __stdcall BrightnessChannel::Run(void* argument) {
    std::unique_ptr<std::shared_ptr<State>> supplied(static_cast<std::shared_ptr<State>*>(argument));
    auto state = *supplied; supplied.reset();
    std::unique_ptr<BrightnessIo> io;
    try { if (!state->nativeSoftware) io = state->factory(); }
    catch (...) { state->software = true; }
    uint64_t processed = state->completed.load(), verificationDue = 0, retryDue = 0, retryCommand = 0;
    unsigned failures = 0;
    bool force = false, attemptRecovery = false;
    while (!state->stopping) {
        uint64_t command = state->requested.load(std::memory_order_acquire);
        const uint64_t now = GetTickCount64();
        if (state->refresh.exchange(false)) { attemptRecovery = !state->nativeSoftware; force = true; failures = 0; retryDue = 0; }
        if(state->revalidate.exchange(false)&&!state->software&&io&&command==processed&&!verificationDue)verificationDue=now;
        bool retry = retryDue && now >= retryDue;
        if (retryDue && command != retryCommand) { retryDue = 0; failures = 0; retry = false; }
        if (command != processed || force || retry) {
            if (command != processed) failures = 0;
            const bool software = state->software && !attemptRecovery;
            DWORD error = 0; bool ok = software;
            const auto start = std::chrono::steady_clock::now();
            if (!software && io) {
                state->callStarted = GetTickCount64(); state->inFlight = true;
                try { ok = io->Set(Percent(command), force || retry || (command & 128) != 0, error); }
                catch (...) { ok = false; error = ERROR_INVALID_FUNCTION; }
                state->inFlight = false;
                state->writes.fetch_add(1);
            }
            state->writeMicros = static_cast<uint64_t>(Milliseconds(start) * 1000);
            force = false; retryDue = 0;
            // A completed obsolete write can never mark a newer request verified or failed.
            processed = command;
            if (ok) {
                state->applied = Percent(command); state->completed = command;
                if (software) { state->verified = command; verificationDue = 0; }
                else { state->verified = 0; verificationDue = GetTickCount64() + 100; }
                if (state->requested.load() == command) { state->error = 0; state->stalled = false; }
            } else if (state->requested.load() == command) {
                state->needsDiscovery=true;
                state->error = error ? error : ERROR_WRITE_FAULT; state->errorCommand = command;
                if (++failures >= 3 || !io) {
                    state->software = true; attemptRecovery = false;
                    state->applied = Percent(command); state->completed = state->verified = command;
                    state->error = 0; failures = 0; verificationDue = 0;
                } else { retryCommand = command; retryDue = GetTickCount64() + 100 * failures; }
            }
            state->Notify();
            if (state->requested.load() != command) continue;
        }
        if (!state->stopping && verificationDue && GetTickCount64() >= verificationDue && !state->nativeSoftware && io && (!state->software || attemptRecovery)) {
            command = state->requested.load();
            if (command == processed && !retryDue) {
                DWORD error = 0; bool ok = false;
                state->callStarted = GetTickCount64(); state->inFlight = true;
                try { ok = io->Verify(Percent(command), error); } catch (...) { error = ERROR_INVALID_FUNCTION; }
                state->inFlight = false;
                state->reads.fetch_add(1); verificationDue = 0;
                if (state->requested.load() == command) {
                    if (ok) { state->verified = command; state->error = 0; state->software = false; state->stalled=false; state->needsDiscovery=false; attemptRecovery = false; failures = 0; }
                    else {
                        state->needsDiscovery=true;
                        state->error = error ? error : ERROR_READ_FAULT; state->errorCommand = command;
                        if (++failures >= 3) {
                            state->software = true; attemptRecovery = false; state->error = 0;
                            state->completed = state->verified = command; state->applied = Percent(command); failures = 0;
                        } else { retryCommand = command; retryDue = GetTickCount64() + 100 * failures; }
                    }
                    state->Notify();
                }
                if (state->requested.load() != command) continue;
            }
        }
        if (state->stopping) break;
        DWORD wait = INFINITE;
        const uint64_t tick = GetTickCount64();
        if (retryDue) wait = static_cast<DWORD>(retryDue > tick ? retryDue - tick : 0);
        else if (verificationDue) wait = static_cast<DWORD>(verificationDue > tick ? verificationDue - tick : 0);
        WaitForSingleObject(state->event, wait);
    }
    // Release COM and adapter state on the same thread that created them.
    io.reset(); state->finished = true; state->Notify();
    return 0;
}
namespace {
class NativeIo final : public BrightnessIo {
    std::shared_ptr<Display> monitor;
    DisplayControl control;
    DWORD lastRaw;
public:
    explicit NativeIo(std::shared_ptr<Display> monitor) : monitor(std::move(monitor)), lastRaw(this->monitor->originalRaw) {}
    bool Set(int percent, bool force, DWORD& error) override {
        DWORD raw = NativeRaw(*monitor, percent);
        if (!force && raw == lastRaw) return true;
        if (!control.Write(*monitor, raw)) { error = GetLastError(); return false; }
        lastRaw = raw; return true;
    }
    bool Verify(int percent, DWORD& error) override {
        DWORD actual = 0;
        if (!control.Read(*monitor, actual)) { error = GetLastError(); return false; }
        lastRaw = actual;
        DWORD expected = NativeRaw(*monitor, percent);
        if (actual == expected) return true;
        error = ERROR_INVALID_DATA; return false;
    }
};
}
BrightnessChannel::Factory NativeBrightnessFactory(std::shared_ptr<Display> monitor) {
    return [monitor] { return std::make_unique<NativeIo>(monitor); };
}
bool WriteUtf8File(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool result = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(file); return result;
}

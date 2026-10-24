#include "channel.h"
#include "platform.h"
#include "reconnect.h"
#include <thread>
#include <chrono>
#include <sstream>
#include <numeric>
#include <iomanip>
#include <limits>
#include <psapi.h>

namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
double CpuMs() {
    FILETIME created{}, ended{}, kernel{}, user{}; GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user);
    ULARGE_INTEGER k{}, u{}; k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;
    return (k.QuadPart+u.QuadPart)/10000.0;
}
size_t PrivateBytes() { PROCESS_MEMORY_COUNTERS_EX stats{}; GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&stats), sizeof(stats)); return stats.PrivateUsage; }
double Quantile(std::vector<double> values, double fraction) {
    if(values.empty())return 0;
    std::sort(values.begin(),values.end());return values[static_cast<size_t>((values.size()-1)*fraction)];
}
bool Idle(BrightnessChannel& channel,DWORD timeout=2500) {
    ULONGLONG deadline=GetTickCount64()+timeout;
    do { auto s=channel.Snapshot();if(s.requested==s.completed&&s.requested==s.verified&&s.wanted==s.applied&&!s.error)return true;Sleep(1); } while(GetTickCount64()<deadline);
    return false;
}
struct FakeState {
    std::atomic<int> actual{50}, delay{0}, failWrites{0};
    std::atomic<bool> rejectWrites{false}, rejectReads{false}, writing{false};
    std::atomic<bool> online{true};
    std::atomic<unsigned> generation{1};
    std::atomic<uint64_t> calls{0};
};
class FakeIo final:public BrightnessIo {
    std::shared_ptr<FakeState> state;
    unsigned generation;
public:
    explicit FakeIo(std::shared_ptr<FakeState> state):state(std::move(state)),generation(this->state->generation.load()){}
    bool Set(int value,bool,DWORD& error) override {
        state->writing=true;state->calls.fetch_add(1);int delay=state->delay.load();if(delay)Sleep(delay);
        if(!state->online||generation!=state->generation){state->writing=false;error=ERROR_INVALID_HANDLE;return false;}
        if(state->rejectWrites||(state->failWrites>0&&state->failWrites.fetch_sub(1)>0)){state->writing=false;error=ERROR_ACCESS_DENIED;return false;}
        state->actual=value;state->writing=false;return true;
    }
    bool Verify(int value,DWORD& error) override {if(!state->online||generation!=state->generation){error=ERROR_INVALID_HANDLE;return false;}if(state->rejectReads||state->actual!=value){error=ERROR_INVALID_DATA;return false;}return true;}
};
BrightnessChannel::Factory FakeFactory(const std::shared_ptr<FakeState>& state) {return [state]{return std::make_unique<FakeIo>(state);};}
bool Writing(const std::shared_ptr<FakeState>& state,DWORD timeout=1000){ULONGLONG deadline=GetTickCount64()+timeout;while(!state->writing&&GetTickCount64()<deadline)Sleep(1);return state->writing;}
struct Results {
    struct Item {std::string name;bool ok;uint64_t count;};
    std::vector<Item> items;
    void Add(const char* name,bool ok,uint64_t count=1){items.push_back({name,ok,count});}
    bool Passed()const{return std::all_of(items.begin(),items.end(),[](const Item& item){return item.ok;});}
    void Json(std::ostringstream& out)const{
        out<<"\"passed\":"<<(Passed()?"true":"false")<<",\"cases\":[";
        for(size_t i=0;i<items.size();++i){if(i)out<<',';out<<"{\"name\":\""<<items[i].name<<"\",\"passed\":"<<(items[i].ok?"true":"false")<<",\"checks\":"<<items[i].count<<'}';}out<<']';
    }
};
std::string Utf8(const std::wstring& value){int n=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),out.data(),n,nullptr,nullptr);return out;}
std::string Quote(const std::wstring& value){std::string out="\"";for(char c:Utf8(value)){if(c=='\\'||c=='\"')out+='\\';if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";else out+=c;}return out+'\"';}
}
bool RunStressTests(const std::wstring& path){
    const auto started=Clock::now();Results results;
    uint64_t checks=0;bool correct=true;
    const std::vector<std::pair<DWORD,DWORD>> ranges={{0,100},{20,240},{0,255},{5,17},{0,65535},{0,0xfffffffful},{10,10}};
    for(const auto& range:ranges){DWORD last=range.first;for(int p=-200;p<=300;++p){DWORD raw=ToRaw(p,range.first,range.second);int roundtrip=ToPercent(raw,range.first,range.second);correct=correct&&raw>=range.first&&raw<=range.second&&raw>=last&&roundtrip>=0&&roundtrip<=100;last=raw;++checks;}}
    results.Add("brightness_ranges_clamping_and_overflow",correct,checks);
    Display laptop;laptop.backend=Backend::Wmi;laptop.levels={0,10,30,60,100};
    correct=NativeRaw(laptop,50)==60&&NativeRaw(laptop,20)==10&&NativeRaw(laptop,-100)==0&&NativeRaw(laptop,1000)==100;
    for(int p=0;p<=100;++p)correct=correct&&std::find(laptop.levels.begin(),laptop.levels.end(),NativeRaw(laptop,p))!=laptop.levels.end();
    results.Add("laptop_discrete_acpi_brightness_levels",correct,105);
    const std::wstring key=L"\\\\?\\DISPLAY#AUO1234#SERIAL&UID12#{GUID}";
    correct=MatchesWmiInstance(key,L"DISPLAY\\AUO1234\\SERIAL&UID12_0")&&!MatchesWmiInstance(key,L"DISPLAY\\AUO1234\\SERIAL&UID123_0")&&!MatchesWmiInstance(L"",L"DISPLAY\\AUO1234\\SERIAL&UID12_0")&&!MatchesWmiInstance(L"\\\\.\\DISPLAY1",L"DISPLAY\\AUO1234\\SERIAL&UID12_0");
    results.Add("wmi_identity_exact_match_and_empty_ids",correct,4);
    {
        ReconnectPlan plan;plan.Changed(1000,200);
        for(uint64_t tick=1010;tick<1200;tick+=10)plan.Changed(tick,200);
        correct=plan.Deadline()==1200&&!plan.Ready(1199)&&plan.Ready(1200);
        plan.Observe(1200,true);correct=correct&&plan.Ready(1200);
        plan.Started();plan.Finished(1300);correct=correct&&plan.Deadline()==2300;
        plan.Observe(1400,false);correct=correct&&plan.Deadline()==2300;
        plan.Started();plan.Finished(2400);correct=correct&&plan.Deadline()==4400;
        plan.Changed(2500,200);plan.Observe(2600,true);correct=correct&&plan.Deadline()==2700;
        plan.Started();plan.Finished(2800);plan.Observe(2900,true);correct=correct&&!plan.Deadline();
        results.Add("hotplug_events_coalesced_delayed_bus_retries_and_healthy_stop",correct,27);
        uint64_t clock=10000;plan.Observe(clock,false);
        for(unsigned round=0;round<10;++round){const auto due=plan.Deadline();correct=correct&&due>clock&&due<=clock+30000;plan.Started();clock=due;plan.Finished(clock);}
        results.Add("unsupported_display_rescan_backoff_is_bounded",correct,10);
    }
    {
        Displays topology;
        auto first=std::make_shared<Display>();first->device=L"DISPLAY1";first->logical=reinterpret_cast<HMONITOR>(1);first->bounds={0,0,1920,1080};topology.push_back(first);
        const auto one=DisplayTopology(topology);
        auto clone=std::make_shared<Display>();clone->device=first->device;clone->logical=first->logical;clone->bounds=first->bounds;topology.push_back(clone);
        correct=DisplayTopology(topology)==one;
        clone->device=L"DISPLAY2";clone->logical=reinterpret_cast<HMONITOR>(2);clone->bounds={1920,0,3840,1080};
        correct=correct&&DisplayTopology(topology)!=one;
        results.Add("logical_topology_return_detected_and_physical_clones_deduplicated",correct,2);
    }
    {
        auto fake=std::make_shared<FakeState>();BrightnessChannel channel(50,false,FakeFactory(fake));
        channel.Revalidate();ULONGLONG deadline=GetTickCount64()+1000;
        while(!channel.Snapshot().reads&&GetTickCount64()<deadline)Sleep(1);
        results.Add("idle_connection_check_reads_without_brightness_write",channel.Snapshot().reads==1&&fake->calls==0&&!channel.Snapshot().needsDiscovery);
        channel.Stop(500);
    }
    {
        auto a=std::make_shared<FakeState>(),b=std::make_shared<FakeState>();
        BrightnessChannel first(50,false,FakeFactory(a));auto second=std::make_unique<BrightnessChannel>(50,false,FakeFactory(b));
        first.Submit(61,true);second->Submit(61,true);correct=Idle(first)&&Idle(*second);
        b->online=false;b->generation.fetch_add(1);second->Revalidate();
        ULONGLONG deadline=GetTickCount64()+2000;while(!second->Snapshot().software&&GetTickCount64()<deadline)Sleep(1);
        correct=correct&&second->Snapshot().needsDiscovery&&second->Snapshot().software&&a->actual==61;
        first.Submit(72,true);second->Submit(72,true);correct=correct&&Idle(first)&&Idle(*second)&&a->actual==72;
        b->online=true;second->Refresh();Sleep(500);
        correct=correct&&second->Snapshot().needsDiscovery; // Old handle is still invalid after power returns.
        second->Stop(500);second=std::make_unique<BrightnessChannel>(72,false,FakeFactory(b));second->Submit(72,true);
        correct=correct&&Idle(*second)&&b->actual==72&&!second->Snapshot().software&&!second->Snapshot().needsDiscovery&&a->actual==72;
        results.Add("silent_power_cycle_stale_handle_rediscovery_preserves_two_screen_target",correct,9);
        first.Stop(500);second->Stop(500);
    }
    checks=0;correct=true;
    const std::vector<RECT> work={{0,0,1920,1040},{-3840,-2160,0,0},{1920,0,3840,2080},{0,0,640,360},{0,0,320,200},{0,0,1024,740}};
    for(const auto& rectangle:work)for(UINT dpi:{96u,120u,144u,168u,192u,240u,288u,384u,768u}){
        auto layout=Platform::PlacePanel(rectangle,dpi);correct=correct&&layout.valid&&layout.x>=rectangle.left&&layout.y>=rectangle.top&&layout.x+layout.width==rectangle.right&&layout.y+layout.height<=rectangle.bottom;
        for(int p=0;p<=100;++p){int x=static_cast<int>(std::lround((10+288*p/100.0f)*layout.scale));correct=correct&&std::abs(Platform::PointerValue(x,layout.scale)-p)<=1;++checks;}
    }
    correct=correct&&!Platform::PlacePanel(RECT{0,0,0,0},96).valid&&Platform::PointerValue(std::numeric_limits<int>::max(),1)==100&&Platform::PointerValue(std::numeric_limits<int>::min(),1)==0;
    results.Add("dpi_100_to_800_percent_negative_desktops_small_displays",correct,checks+3);
    Displays mirrors;for(int i=0;i<3;++i){auto d=std::make_shared<Display>();d->bounds=i<2?RECT{0,0,1920,1080}:RECT{-1920,0,0,1080};mirrors.push_back(d);}
    correct=Platform::SoftwareRegions(mirrors,{true,true,true}).size()==2&&Platform::SoftwareRegions(mirrors,{false,false,false}).empty();
    for(int p=-100;p<=200;++p)correct=correct&&Platform::OverlayAlpha(p)<=242;
    correct=correct&&Platform::OverlayAlpha(100)==0&&Platform::OverlayAlpha(0)==242&&Platform::OverlayAlpha(50)==128;
    results.Add("cloned_displays_deduplicated_no_blackout_overlay",correct,306);
    std::vector<double> submitLatency;submitLatency.reserve(10000);uint64_t burstWrites=0;size_t memoryGrowth=0;
    {
        auto fake=std::make_shared<FakeState>();fake->delay=2;BrightnessChannel channel(50,false,FakeFactory(fake));
        const size_t before=PrivateBytes();
        for(int i=0;i<1000000;++i){const auto begin=Clock::now();channel.Submit((i*37)%101);if(i%100==0)submitLatency.push_back(Ms(begin));}
        channel.Submit(73,true);bool idle=Idle(channel);auto snapshot=channel.Snapshot();burstWrites=snapshot.writes;
        size_t after=PrivateBytes();memoryGrowth=after>before?after-before:0;
        results.Add("one_million_rapid_changes_last_value_wins",idle&&fake->actual==73&&snapshot.applied==73&&snapshot.writes<10000,1000000);
        results.Add("bounded_queue_memory_under_4_mb",memoryGrowth<4*1024*1024,1000000);
        channel.Stop(1000);
    }
    {
        auto fake=std::make_shared<FakeState>();fake->delay=1;BrightnessChannel channel(50,false,FakeFactory(fake));
        std::vector<std::thread> producers;for(int i=0;i<8;++i)producers.emplace_back([&channel,i]{for(int j=0;j<25000;++j)channel.Submit((j*17+i)%101);});
        for(auto& producer:producers)producer.join();channel.Submit(67,true);
        results.Add("eight_concurrent_producers_atomic_latest_command",Idle(channel)&&fake->actual==67,200000);channel.Stop(1000);
    }
    double fastLatency=0;
    {
        auto slow=std::make_shared<FakeState>();slow->delay=650;auto fast=std::make_shared<FakeState>();fast->delay=2;
        BrightnessChannel a(50,false,FakeFactory(slow)),b(50,false,FakeFactory(fast));a.Submit(40,true);bool active=Writing(slow);
        const auto begin=Clock::now();b.Submit(80,true);
        ULONGLONG deadline=GetTickCount64()+500;while(b.Snapshot().completed!=b.Snapshot().requested&&GetTickCount64()<deadline)Sleep(1);
        fastLatency=Ms(begin);results.Add("slow_monitor_does_not_delay_other_monitors",active&&fast->actual==80&&fastLatency<100);
        a.Stop(1000);b.Stop(1000);
    }
    {
        auto fake=std::make_shared<FakeState>();fake->failWrites=2;BrightnessChannel channel(50,false,FakeFactory(fake));channel.Submit(45,true);
        results.Add("temporary_write_failures_recover",Idle(channel)&&fake->actual==45&&!channel.Snapshot().software);channel.Stop(1000);
    }
    double idleCpu=0;
    {
        auto fake=std::make_shared<FakeState>();fake->rejectWrites=true;BrightnessChannel channel(50,false,FakeFactory(fake));channel.Submit(22,true);
        bool fallback=Idle(channel)&&channel.Snapshot().software;double cpu=CpuMs();Sleep(250);idleCpu=CpuMs()-cpu;
        results.Add("read_only_hardware_auto_software_fallback",fallback&&fake->calls==3);
        results.Add("software_fallback_has_no_busy_loop",idleCpu<10);
        fake->rejectWrites=false;channel.Refresh();
        ULONGLONG deadline=GetTickCount64()+2500;while(channel.Snapshot().software&&GetTickCount64()<deadline)Sleep(1);
        results.Add("hardware_recovery_removes_software_fallback",Idle(channel)&&!channel.Snapshot().software&&fake->actual==22);channel.Stop(1000);
    }
    {
        auto fake=std::make_shared<FakeState>();fake->rejectReads=true;BrightnessChannel channel(50,false,FakeFactory(fake));channel.Submit(58,true);
        results.Add("unreliable_readback_falls_back_without_infinite_retries",Idle(channel)&&channel.Snapshot().software);channel.Stop(1000);
    }
    {
        auto fake=std::make_shared<FakeState>();fake->delay=300;BrightnessChannel channel(50,false,FakeFactory(fake));channel.Submit(10,true);Writing(fake);channel.Submit(81,true);
        results.Add("obsolete_inflight_write_cannot_win",Idle(channel)&&fake->actual==81);channel.Stop(1000);
    }
    double stopLatency=0;
    {
        auto fake=std::make_shared<FakeState>();fake->delay=450;auto channel=std::make_unique<BrightnessChannel>(50,false,FakeFactory(fake));channel->Submit(60,true);bool active=Writing(fake);
        const auto begin=Clock::now();bool stopped=channel->Stop(20);channel.reset();stopLatency=Ms(begin);
        results.Add("blocked_driver_shutdown_is_bounded_and_keeps_context_alive",active&&!stopped&&stopLatency<100);Sleep(500);
    }
    correct=true;
    for(int cycle=0;cycle<250;++cycle){auto fake=std::make_shared<FakeState>();BrightnessChannel channel(50,false,FakeFactory(fake));channel.Submit(cycle%101,true);correct=correct&&Idle(channel,500)&&channel.Stop(500);}
    results.Add("250_device_remove_reconnect_cycles",correct,250);
    {
        auto fake=std::make_shared<FakeState>();fake->delay=700;BrightnessChannel channel(50,false,FakeFactory(fake));channel.Submit(33,true);bool active=Writing(fake);Sleep(100);
        bool fallback=channel.CheckTimeout(50)&&channel.Snapshot().software;
        results.Add("stalled_driver_temporary_software_control_and_recovery",active&&fallback&&Idle(channel,2000)&&!channel.Snapshot().software&&fake->actual==33);channel.Stop(1000);
    }
    {
        BrightnessChannel software(100,true,{});for(int i=0;i<100000;++i)software.Submit(i%101);software.Submit(0,true);auto s=software.Snapshot();
        results.Add("unsupported_monitor_instant_software_channel",s.wanted==0&&s.applied==0&&s.completed==s.requested&&s.verified==s.requested&&s.writes==0,100001);
    }
    std::ostringstream out;out<<std::fixed<<std::setprecision(6)<<'{';results.Json(out);
    out<<",\"submissionLatencyMs\":{\"p50\":"<<Quantile(submitLatency,.5)<<",\"p95\":"<<Quantile(submitLatency,.95)<<",\"p99\":"<<Quantile(submitLatency,.99)<<"},\"burstNativeWrites\":"<<burstWrites<<",\"queuePrivateGrowthBytes\":"<<memoryGrowth<<",\"fastMonitorWithSlowPeerMs\":"<<fastLatency<<",\"fallbackIdleCpuMsIn250ms\":"<<idleCpu<<",\"boundedStopMs\":"<<stopLatency<<",\"totalTestMs\":"<<Ms(started)<<",\"hardwareWasSimulated\":true}\n";
    return WriteUtf8File(path,out.str())&&results.Passed();
}
bool RunHardwareBenchmark(const std::wstring& path){
    DisplayControl control;auto monitors=control.Scan();std::vector<DWORD> original;bool passed=!monitors.empty();
    for(const auto& d:monitors){DWORD raw=d->originalRaw;if(d->backend!=Backend::Soft)passed=control.Read(*d,raw)&&passed;original.push_back(raw);}
    struct Restore {DisplayControl& io;Displays& monitors;std::vector<DWORD>& values;~Restore(){for(size_t i=0;i<monitors.size();++i)if(monitors[i]->backend!=Backend::Soft)io.Write(*monitors[i],values[i]);}} restore{control,monitors,original};
    std::vector<double> serial,legacy,parallel;std::vector<std::unique_ptr<BrightnessChannel>> channels;
    auto signal=std::shared_ptr<void>(CreateEventW(nullptr,FALSE,FALSE,nullptr),[](void* handle){if(handle)CloseHandle(handle);});
    for(const auto& d:monitors)channels.push_back(std::make_unique<BrightnessChannel>(d->original,d->backend==Backend::Soft,NativeBrightnessFactory(d),[signal]{SetEvent(signal.get());}));
    for(int round=0;round<12;++round){
        std::vector<int> values;for(const auto& d:monitors)values.push_back(std::clamp(d->original+(round%2?0:(d->original<100?1:-1)),0,100));
        auto start=Clock::now();for(size_t i=0;i<monitors.size();++i)if(monitors[i]->backend!=Backend::Soft)passed=control.Write(*monitors[i],NativeRaw(*monitors[i],values[i]))&&passed;
        serial.push_back(Ms(start));
    }
    std::vector<ULONGLONG> lastWrite(monitors.size(),0);
    for(int round=0;round<12;++round){
        std::vector<bool> complete(monitors.size(),false);auto start=Clock::now();
        while(!std::all_of(complete.begin(),complete.end(),[](bool value){return value;})){
            for(size_t i=0;i<monitors.size();++i)if(!complete[i]){
                if(monitors[i]->backend==Backend::Soft){complete[i]=true;continue;}
                if(GetTickCount64()-lastWrite[i]<60)continue;
                int desired=std::clamp(monitors[i]->original+(round%2?0:(monitors[i]->original<100?1:-1)),0,100);
                passed=control.Write(*monitors[i],NativeRaw(*monitors[i],desired))&&passed;lastWrite[i]=GetTickCount64();complete[i]=true;
            }
            if(!std::all_of(complete.begin(),complete.end(),[](bool value){return value;}))Sleep(25);
        }
        legacy.push_back(Ms(start));
    }
    for(int round=0;round<12;++round){
        const auto start=Clock::now();for(size_t i=0;i<channels.size();++i)channels[i]->Submit(std::clamp(monitors[i]->original+(round%2?0:(monitors[i]->original<100?1:-1)),0,100),true);
        ULONGLONG deadline=GetTickCount64()+5000;bool complete=false;
        do{complete=true;for(size_t i=0;i<channels.size();++i){auto s=channels[i]->Snapshot();complete=complete&&s.completed==s.requested&&!s.error&&(monitors[i]->backend==Backend::Soft||!s.software);}if(!complete)WaitForSingleObject(signal.get(),static_cast<DWORD>(deadline>GetTickCount64()?deadline-GetTickCount64():0));}while(!complete&&GetTickCount64()<deadline);
        parallel.push_back(Ms(start));passed=passed&&complete;
    }
    // A high-rate burst uses only adjacent values, then restores each monitor's own original.
    const auto burstStart=Clock::now();for(int n=0;n<50000;++n)for(size_t i=0;i<channels.size();++i)channels[i]->Submit(std::clamp(monitors[i]->original+(n%2),0,100));
    double burstSubmitMs=Ms(burstStart);for(size_t i=0;i<channels.size();++i)channels[i]->Submit(monitors[i]->original,true);
    for(const auto& c:channels)passed=Idle(*c,5000)&&passed;
    std::vector<ChannelSnapshot> statistics;for(const auto& c:channels){statistics.push_back(c->Snapshot());passed=c->Stop(2000)&&passed;}channels.clear();
    std::ostringstream out;out<<std::fixed<<std::setprecision(3)<<"{\"serialWriteMs\":{\"p50\":"<<Quantile(serial,.5)<<",\"p95\":"<<Quantile(serial,.95)<<"},\"legacyScheduledWriteMs\":{\"p50\":"<<Quantile(legacy,.5)<<",\"p95\":"<<Quantile(legacy,.95)<<"},\"parallelWriteMs\":{\"p50\":"<<Quantile(parallel,.5)<<",\"p95\":"<<Quantile(parallel,.95)<<"},\"burstRequests\":"<<50000*monitors.size()<<",\"burstSubmitMs\":"<<burstSubmitMs<<",\"displays\":[";
    for(size_t i=0;i<monitors.size();++i){
        bool restored=true;DWORD after=original[i];if(monitors[i]->backend!=Backend::Soft){restored=control.Write(*monitors[i],original[i]);Sleep(50);restored=control.Read(*monitors[i],after)&&after==original[i]&&restored;}passed=passed&&restored;
        if(i)out<<',';out<<"{\"name\":"<<Quote(monitors[i]->name)<<",\"backend\":"<<Quote(BackendName(monitors[i]->backend))<<",\"physicalHandle\":"<<reinterpret_cast<UINT_PTR>(monitors[i]->physical)<<",\"beforeRaw\":"<<original[i]<<",\"afterRaw\":"<<after<<",\"restored\":"<<(restored?"true":"false")<<",\"writes\":"<<statistics[i].writes<<",\"reads\":"<<statistics[i].reads<<'}';
    }
    out<<"],\"passed\":"<<(passed?"true":"false")<<",\"note\":\"API completion latency; not input-to-photon latency\"}\n";
    return WriteUtf8File(path,out.str())&&passed;
}

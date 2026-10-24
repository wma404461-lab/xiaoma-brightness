#pragma once
#include <cstdint>

// Mirrors the remote app's startup/wake grace and sustained-failure checks.
// Monitor/DDC failures do not count as a frozen UI; only the UI heartbeat does.
class ResidentHealthPlan {
    uint64_t started_,last_,grace_,badSince_=0;
    bool unhealthy_=false;
public:
    explicit ResidentHealthPlan(uint64_t now):started_(now),last_(now),grace_(now+20000){}
    bool Observe(uint64_t now,bool responsive){
        if(now<last_||now-last_>10000){grace_=now+15000;unhealthy_=false;}
        last_=now;
        if(now<started_||now<grace_||responsive){unhealthy_=false;return false;}
        if(!unhealthy_){unhealthy_=true;badSince_=now;}
        return now-badSince_>=10000;
    }
};

class ResidentRestartPlan {
    unsigned failures_=0;
public:
    unsigned Next(uint64_t uptimeMs){
        if(uptimeMs>=60000)failures_=0;
        if(++failures_>=5){failures_=0;return 60000;}
        constexpr unsigned delay[]{1000,2000,5000,10000};return delay[failures_-1];
    }
};

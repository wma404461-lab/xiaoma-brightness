#pragma once
#include <algorithm>
#include <cstdint>

// Coalesce plug/power events and back off while a sleeping DDC bus wakes up.
// No allocation, polling thread, or brightness value is owned by this policy.
class ReconnectPlan {
    uint64_t due=0, poll=0;
    unsigned attempt=0;
    bool eventPending=false;
    void Retry(uint64_t now) {
        constexpr unsigned delays[]={1000,2000,4000,10000,30000};
        due=now+delays[std::min(attempt,4u)];
        if(attempt<4)++attempt;
    }
public:
    void Changed(uint64_t now,unsigned delay=200) {
        eventPending=true;attempt=0;
        const uint64_t next=now+delay;
        if(!due||next<due)due=next;
    }
    void Started(){due=0;eventPending=false;}
    void Finished(uint64_t now){if(!due)Retry(now);}
    void Observe(uint64_t now,bool healthy) {
        if(healthy&&!eventPending){due=0;attempt=0;}
        else if(!healthy&&!due)Retry(now);
    }
    bool Ready(uint64_t now)const{return due&&now>=due;}
    uint64_t Deadline()const{return due;}
    bool PollDue(uint64_t now){if(now<poll)return false;poll=now+5000;return true;}
};

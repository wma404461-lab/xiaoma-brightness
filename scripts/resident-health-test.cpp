#include "../src/resident-health.h"
#include <iostream>
int main(){
    unsigned checks=0,failed=0;auto check=[&](bool ok){++checks;if(!ok)++failed;};
    ResidentHealthPlan frozen(0);
    for(unsigned now=2000;now<=28000;now+=2000)check(!frozen.Observe(now,false));
    check(frozen.Observe(30000,false));
    ResidentHealthPlan transient(0);
    for(unsigned now=2000;now<=24000;now+=2000)check(!transient.Observe(now,false));
    check(!transient.Observe(26000,true));
    for(unsigned now=28000;now<=36000;now+=2000)check(!transient.Observe(now,false));
    check(transient.Observe(38000,false));
    ResidentHealthPlan waking(0);
    for(unsigned now=2000;now<=40000;now+=2000)check(!waking.Observe(now,true));
    for(unsigned now=100000;now<=124000;now+=2000)check(!waking.Observe(now,false));
    check(waking.Observe(126000,false));
    check(!waking.Observe(1000,false)); // clock rollback is also a grace period
    ResidentRestartPlan restarts;
    for(unsigned cycle=0;cycle<1000;++cycle){for(auto expected:{1000u,2000u,5000u,10000u,60000u})check(restarts.Next(100)==expected);}
    check(restarts.Next(100)==1000);check(restarts.Next(100)==2000);
    check(restarts.Next(60000)==1000);check(restarts.Next(100)==2000);
    std::cout<<"{\"passed\":"<<(!failed?"true":"false")<<",\"checks\":"<<checks<<",\"failed\":"<<failed<<",\"simulatedWakeGrace\":true,\"physicalSleepPerformed\":false,\"rapidFailureCooldownCycles\":1000}\n";
    return failed?2:0;
}

#include "../src/tray-toggle.h"
#include <iostream>

int main() {
    bool passed=true;
    unsigned checks=0;
    auto check=[&](bool result){passed=passed&&result;++checks;};
    for(int order=0;order<3;++order){
        TrayToggle toggle;
        bool visible=false;
        for(int click=0;click<10000;++click){
            const bool expected=!visible;
            if(visible&&order==1){toggle.Deactivated(true,true);visible=false;}
            toggle.MouseDown(visible);
            if(visible&&order==2){toggle.Deactivated(true,true);visible=false;}
            visible=toggle.Select(visible);
            check(visible==expected);
        }
    }
    for(int i=0;i<10000;++i){
        TrayToggle toggle;
        toggle.Deactivated(true,false);
        check(toggle.Select(false)); // An outside click must not consume the next tray click.
        toggle.MouseDown(true);
        toggle.Deactivated(true,true);
        toggle.MouseDown(false); // A drag cancelled the first press; a new press opens.
        check(toggle.Select(false));
        toggle.Deactivated(true,true);
        toggle.MouseDown(false);
        check(!toggle.Select(false)); // Focus loss before mouse-down still closes once.
        check(toggle.Select(false)); // No stale close intent on a later activation.
        toggle.MouseDown(true);toggle.Deactivated(true,true);toggle.Reset();
        check(toggle.Select(false)); // Escape, explicit show/hide, and menus clear the press.
    }
    std::cout<<"{\"passed\":"<<(passed?"true":"false")<<",\"checks\":"<<checks
             <<",\"cases\":[\"repeated_toggle\",\"focus_before_press\",\"focus_after_press\",\"outside_click_then_open\",\"cancelled_press_then_open\",\"reset_clears_stale_press\"]}\n";
    return passed?0:2;
}

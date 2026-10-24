#pragma once
#include <windows.h>
#include <dwmapi.h>

namespace BrightnessCapture {
inline bool ExcludeSoftOverlay(HWND overlay) {
    OSVERSIONINFOEXW version{};version.dwOSVersionInfoSize=sizeof(version);
    version.dwMajorVersion=10;version.dwBuildNumber=19041;
    const DWORD fields[]={VER_MAJORVERSION,VER_MINORVERSION,VER_SERVICEPACKMAJOR,VER_SERVICEPACKMINOR,VER_BUILDNUMBER};
    DWORD mask=0;DWORDLONG condition=0;
    for(DWORD field:fields){mask|=field;condition=VerSetConditionMask(condition,field,VER_GREATER_EQUAL);}
    // Earlier Windows versions turn 0x11 into a black rectangle in captures.
    if(!VerifyVersionInfoW(&version,mask,condition))return false;
    BOOL composing=FALSE;
    if(FAILED(DwmIsCompositionEnabled(&composing))||!composing)return false;
    return SetWindowDisplayAffinity(overlay,WDA_EXCLUDEFROMCAPTURE)!=FALSE;
}
}

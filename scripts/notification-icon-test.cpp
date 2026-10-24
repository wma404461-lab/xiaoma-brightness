#include "../src/notification-icon.h"
#include <iostream>
#include <vector>

namespace {
bool exists=false,shell=true,version=true,flags=true;
unsigned adds=0,modifies=0,versions=0,deletes=0;
GUID expected{};
BOOL WINAPI Fake(DWORD action,PNOTIFYICONDATAW data){
    flags=flags&&data->cbSize==sizeof(*data)&&(data->uFlags&NIF_GUID)&&IsEqualGUID(data->guidItem,expected)&&!(data->uFlags&NIF_INFO);
    if(action==NIM_DELETE){++deletes;const bool before=exists;exists=false;return before;}
    if(action==NIM_SETVERSION){++versions;return shell&&exists&&version&&data->uVersion==NOTIFYICON_VERSION_4;}
    flags=flags&&(data->uFlags&(NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP))==(NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP);
    if(action==NIM_ADD){++adds;if(!shell||exists)return FALSE;exists=true;return TRUE;}
    if(action==NIM_MODIFY){++modifies;return shell&&exists;}
    return FALSE;
}
void Reset(){exists=false;shell=version=flags=true;adds=modifies=versions=deletes=0;}
}
int main(){
    std::vector<const char*> failed;unsigned checks=0;
    auto check=[&](bool result,const char* name){++checks;if(!result)failed.push_back(name);};
    expected=NotificationIdentity(L"C:\\Test\\Brightness.exe");
    check(IsEqualGUID(expected,NotificationIdentity(L"c:\\test\\BRIGHTNESS.EXE")),"identity_case_stable");
    check(!IsEqualGUID(expected,NotificationIdentity(L"D:\\Test\\Brightness.exe")),"portable_path_identity");
    Reset();NotificationIcon icon(Fake);icon.Initialize(reinterpret_cast<HWND>(1),WM_APP+1,reinterpret_cast<HICON>(2),expected);
    check(icon.Sync(0)&&adds==1&&versions==1&&flags,"initial_registration");
    icon.Tick(4999);check(modifies==0,"no_idle_poll_before_deadline");
    icon.Tick(5000);check(modifies==1&&adds==1&&icon.Healthy(),"lightweight_idle_health_check");
    exists=false;icon.Tick(9999);check(!exists,"wait_for_due_check");
    icon.Tick(10000);check(exists&&icon.Healthy()&&adds==2&&versions==2,"missing_icon_without_broadcast");
    const auto before=adds;icon.ShellRecreated(10001);check(exists&&icon.Healthy()&&adds==before+1,"duplicate_taskbar_message_no_duplicate_icon");
    shell=false;exists=false;icon.ShellRecreated(10002);check(!icon.Healthy()&&icon.Deadline()==11002,"shell_not_ready_retry");
    shell=true;icon.Tick(11002);check(icon.Healthy()&&exists,"shell_ready_recovery");
    icon.Tip(L"test 42%");icon.Icon(reinterpret_cast<HICON>(3));check(icon.Sync(12000)&&flags,"full_metadata_and_standard_tooltip");
    version=false;exists=false;icon.ShellRecreated(12001);check(!icon.Healthy()&&icon.Deadline()==13001,"version_failure_retries");
    version=true;icon.Tick(13001);check(icon.Healthy(),"version_recovery");
    for(unsigned i=0;i<10000;++i){exists=false;icon.Tick(icon.Deadline());if(!exists||!icon.Healthy()||!flags){failed.push_back("repeated_icon_loss");break;}}
    check(icon.Registrations()>=10000&&flags,"10000_recovery_cycles");
    icon.Remove();const auto count=adds+modifies+versions;icon.Tick(999999999);icon.ShellRecreated(999999999);
    check(!exists&&!icon.Healthy()&&count==adds+modifies+versions&&deletes==1,"maintenance_stop_does_not_recreate");
    shell=false;Reset();NotificationIcon unavailable(Fake);unavailable.Initialize(reinterpret_cast<HWND>(1),WM_APP+1,reinterpret_cast<HICON>(2),expected);shell=false;
    check(!unavailable.Sync(0)&&unavailable.Deadline()==1000,"first_add_failure");shell=true;unavailable.Tick(1000);check(unavailable.Healthy(),"initial_shell_delay_recovery");unavailable.Remove();

    // Real Shell API checks use only this test's hidden window and icon.
    const HINSTANCE app=GetModuleHandleW(nullptr);WNDCLASSW cls{};cls.hInstance=app;cls.lpfnWndProc=DefWindowProcW;cls.lpszClassName=L"XiaomaBrightness.TrayRegression";RegisterClassW(&cls);
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,app,nullptr);
    NotificationIcon real;const GUID testId=NotificationIdentity(L"XiaomaBrightness.TrayRegression.1.1.5");
    real.Initialize(window,WM_APP+77,LoadIconW(nullptr,IDI_INFORMATION),testId);
    check(window&&real.Sync(0),"real_shell_registration");
    NOTIFYICONDATAW data{};data.cbSize=sizeof(data);data.hWnd=window;data.uID=1;data.uFlags=NIF_GUID;data.guidItem=testId;
    check(Shell_NotifyIconW(NIM_DELETE,&data)!=FALSE,"real_external_removal");
    real.Tick(5000);check(real.Healthy()&&real.Registrations()==2,"real_missing_icon_recovery");
    real.ShellRecreated(5001);check(real.Healthy(),"real_taskbar_message_recovery");
    real.Remove();check(!Shell_NotifyIconW(NIM_MODIFY,&data),"real_maintenance_cleanup");
    check(!IsWindowVisible(window),"test_never_shows_panel");DestroyWindow(window);
    std::cout<<"{\"checks\":"<<checks<<",\"recoveryCycles\":10000,\"realShellChecks\":6,\"passed\":"<<(failed.empty()?"true":"false")<<",\"failures\":[";
    for(size_t i=0;i<failed.size();++i){if(i)std::cout<<',';std::cout<<'\"'<<failed[i]<<'\"';}std::cout<<"]}\n";return failed.empty()?0:2;
}

#pragma once
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <cstdio>
#include "resident-health.h"

namespace Resident {
inline constexpr wchar_t MutexName[]=L"Local\\XiaomaBrightness.Resident.1";
inline constexpr wchar_t StopName[]=L"Local\\XiaomaBrightness.Stop.1";
inline constexpr wchar_t PanelClass[]=L"XiaomaBrightness.Panel.1";
inline constexpr UINT QuitMessage=WM_APP+5;
inline constexpr UINT HeartbeatMessage=WM_APP+11;
inline constexpr LRESULT HeartbeatReply=0x584d0116;

inline std::wstring Directory(){
    wchar_t path[MAX_PATH]{};
    if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,path)))return {};
    std::wstring directory=std::wstring(path)+L"\\XiaomaBrightness";CreateDirectoryW(directory.c_str(),nullptr);return directory;
}
inline void Log(const char* event,DWORD detail=0){
    const auto directory=Directory();if(directory.empty())return;
    const auto path=directory+L"\\lifecycle.log";
    HANDLE file=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    LARGE_INTEGER size{};
    if(GetFileSizeEx(file,&size)&&size.QuadPart>65536){
        CloseHandle(file);MoveFileExW(path.c_str(),(directory+L"\\lifecycle.previous.log").c_str(),MOVEFILE_REPLACE_EXISTING);
        file=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return;
    }
    SYSTEMTIME time{};GetSystemTime(&time);char line[192]{};
    const int count=sprintf_s(line,"%04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu %s detail=%lu\r\n",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId(),event,detail);
    DWORD written=0;if(count>0)WriteFile(file,line,static_cast<DWORD>(count),&written,nullptr);CloseHandle(file);
}
inline bool IsRunning(const wchar_t* name=MutexName){
    HANDLE mutex=OpenMutexW(SYNCHRONIZE,FALSE,name);if(!mutex)return false;CloseHandle(mutex);return true;
}
inline void RequestStop(const wchar_t* name=StopName){
    HANDLE stop=OpenEventW(EVENT_MODIFY_STATE,FALSE,name);if(stop){SetEvent(stop);CloseHandle(stop);}
}
struct Configuration {
    const wchar_t* mutexName=MutexName;
    const wchar_t* stopName=StopName;
    const wchar_t* panelClass=PanelClass;
    void(*log)(const char*,DWORD)=Log;
};
inline bool Responsive(DWORD pid,const wchar_t* panelClass){
    HWND panel=FindWindowW(panelClass,nullptr);DWORD owner=0;if(panel)GetWindowThreadProcessId(panel,&owner);
    if(owner!=pid)return false;
    DWORD_PTR reply=0;
    return SendMessageTimeoutW(panel,HeartbeatMessage,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK|SMTO_ERRORONEXIT,500,&reply)!=0&&reply==HeartbeatReply;
}
inline void CloseChild(DWORD pid,HANDLE process,const Configuration& config){
    HWND panel=FindWindowW(config.panelClass,nullptr);DWORD owner=0;if(panel)GetWindowThreadProcessId(panel,&owner);
    if(owner==pid)PostMessageW(panel,QuitMessage,0,0);
    if(WaitForSingleObject(process,5000)==WAIT_TIMEOUT){config.log("maintenance_force_stop",pid);TerminateProcess(process,0);WaitForSingleObject(process,1000);}
}

// One blocked wait, no polling thread or mutual resurrection. An administrator
// can always stop the process tree. --quit is the maintenance/uninstall path.
inline int Run(bool hidden,bool inspect=false,const Configuration& config=Configuration{}){
    HANDLE mutex=CreateMutexW(nullptr,FALSE,config.mutexName);
    if(!mutex)return 2;
    if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(mutex);return 0;}
    HANDLE stop=CreateEventW(nullptr,TRUE,FALSE,config.stopName);
    if(!stop){CloseHandle(mutex);return 2;}
    ResetEvent(stop);
    wchar_t executable[32768]{};
    const DWORD length=GetModuleFileNameW(nullptr,executable,ARRAYSIZE(executable));
    if(!length||length>=ARRAYSIZE(executable)){CloseHandle(stop);CloseHandle(mutex);return 2;}
    ResidentRestartPlan restart;bool first=true;int result=0;
    config.log("resident_start",0);
    while(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0){
        std::wstring command=L"\""+std::wstring(executable)+L"\" --resident-child";
        if(hidden||!first)command+=L" --tray";
        if(inspect&&first)command+=L" --inspect";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION child{};const auto began=GetTickCount64();
        const BOOL launched=CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child);
        first=false;
        if(launched){
            CloseHandle(child.hThread);config.log("panel_started",child.dwProcessId);
            HANDLE wait[]{stop,child.hProcess};DWORD reason=WAIT_FAILED;
            ResidentHealthPlan health(began);
            for(;;){
                reason=WaitForMultipleObjects(2,wait,FALSE,2000);
                if(reason!=WAIT_TIMEOUT)break;
                if(health.Observe(GetTickCount64(),Responsive(child.dwProcessId,config.panelClass))&&WaitForSingleObject(stop,0)!=WAIT_OBJECT_0&&WaitForSingleObject(child.hProcess,0)==WAIT_TIMEOUT){
                    config.log("panel_heartbeat_stalled",child.dwProcessId);
                    // This handle belongs to the exact child created above; never
                    // find/kill processes by name or touch remote/other apps.
                    if(TerminateProcess(child.hProcess,0x584d0001)){
                        WaitForSingleObject(child.hProcess,3000);reason=WAIT_OBJECT_0+1;break;
                    }
                }
            }
            if(reason==WAIT_OBJECT_0||reason==WAIT_FAILED){CloseChild(child.dwProcessId,child.hProcess,config);CloseHandle(child.hProcess);break;}
            DWORD code=0;GetExitCodeProcess(child.hProcess,&code);CloseHandle(child.hProcess);
            if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0)break;
            config.log("panel_unexpected_exit",code);
        }else{
            const DWORD error=GetLastError();config.log("panel_launch_failed",error);
            if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||error==ERROR_ACCESS_DENIED||error==ERROR_BAD_EXE_FORMAT){result=2;break;}
        }
        const DWORD delay=restart.Next(GetTickCount64()-began);
        if(delay==60000)config.log("recovery_cooldown",delay);
        if(WaitForSingleObject(stop,delay)==WAIT_OBJECT_0)break;
    }
    config.log("resident_stop",static_cast<DWORD>(result));CloseHandle(stop);CloseHandle(mutex);return result;
}
}

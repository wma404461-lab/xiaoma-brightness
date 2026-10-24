#pragma once
#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <string>

namespace Startup {
struct ShortcutInfo { bool readable=false;std::wstring target,arguments; };
inline ShortcutInfo InspectShortcut(){
    ShortcutInfo result;wchar_t directory[MAX_PATH]{};
    if(FAILED(SHGetFolderPathW(nullptr,CSIDL_STARTUP,nullptr,SHGFP_TYPE_CURRENT,directory)))return result;
    const std::wstring shortcut=std::wstring(directory)+L"\\小马亮度.lnk";
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized)&&initialized!=RPC_E_CHANGED_MODE)return result;
    [&]{
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.GetAddressOf()))))return;
        Microsoft::WRL::ComPtr<IPersistFile> file;if(FAILED(link.As(&file))||FAILED(file->Load(shortcut.c_str(),STGM_READ)))return;
        wchar_t path[32768]{},arguments[32]{};
        if(FAILED(link->GetPath(path,ARRAYSIZE(path),nullptr,SLGP_RAWPATH))||FAILED(link->GetArguments(arguments,ARRAYSIZE(arguments))))return;
        result.readable=true;result.target=path;result.arguments=arguments;
    }();
    if(SUCCEEDED(initialized))CoUninitialize();return result;
}
inline bool RegisterShortcut(const wchar_t* executable) {
    wchar_t directory[MAX_PATH]{};
    if(FAILED(SHGetFolderPathW(nullptr,CSIDL_STARTUP|CSIDL_FLAG_CREATE,nullptr,SHGFP_TYPE_CURRENT,directory)))return false;
    const std::wstring shortcut=std::wstring(directory)+L"\\小马亮度.lnk";
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized)&&initialized!=RPC_E_CHANGED_MODE)return false;
    bool registered=[&]{
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.GetAddressOf()))))return false;
        Microsoft::WRL::ComPtr<IPersistFile> file;if(FAILED(link.As(&file)))return false;
        if(SUCCEEDED(file->Load(shortcut.c_str(),STGM_READ))){
            wchar_t path[32768]{},arguments[32]{};
            if(SUCCEEDED(link->GetPath(path,ARRAYSIZE(path),nullptr,SLGP_RAWPATH))&&SUCCEEDED(link->GetArguments(arguments,ARRAYSIZE(arguments)))&&_wcsicmp(path,executable)==0&&wcscmp(arguments,L"--tray")==0)return true;
        }
        if(FAILED(link->SetPath(executable))||FAILED(link->SetArguments(L"--tray"))||FAILED(link->SetShowCmd(SW_SHOWMINNOACTIVE))||FAILED(link->SetIconLocation(executable,0))||FAILED(link->SetDescription(L"小马亮度 · 登录后静默常驻托盘")))return false;
        return SUCCEEDED(file->Save(shortcut.c_str(),TRUE));
    }();
    if(SUCCEEDED(initialized))CoUninitialize();
    return registered;
}
}

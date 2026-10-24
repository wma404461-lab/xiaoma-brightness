#pragma once
#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <cwchar>
#include <cwctype>

// An unsigned portable app needs a stable identity at each executable location.
// This hash identifies the icon only; it is not used for security or trust.
inline GUID NotificationIdentity(const wchar_t* path) {
    uint64_t a=14695981039346656037ull,b=7809847782465536322ull;
    for(;*path;++path){const auto c=static_cast<uint64_t>(towlower(*path));a=(a^c)*1099511628211ull;b=(b^(c+0x9e37))*1099511628211ull;}
    GUID id{0x4bd36404,0x6f60,0x4cf6,{0xaa,0xf6,0xcd,0xd7,0x40,0x61,0x4b,0x2c}};
    id.Data1^=static_cast<DWORD>(a);id.Data2^=static_cast<WORD>(a>>32);
    for(unsigned i=0;i<8;++i)id.Data4[i]^=static_cast<BYTE>(b>>(i*8));
    id.Data4[0]=static_cast<BYTE>((id.Data4[0]&0x3f)|0x80);return id;
}

class NotificationIcon {
public:
    using Notify=BOOL(WINAPI*)(DWORD,PNOTIFYICONDATAW);
    explicit NotificationIcon(Notify notify=Shell_NotifyIconW):notify_(notify){}
    void Initialize(HWND window,UINT callback,HICON icon,const GUID& identity){
        data_={};data_.cbSize=sizeof(data_);data_.hWnd=window;data_.uID=1;
        data_.uCallbackMessage=callback;data_.hIcon=icon;data_.guidItem=identity;
        wcscpy_s(data_.szTip,L"小马亮度");enabled_=true;installed_=versioned_=false;due_=0;
    }
    void Icon(HICON icon){data_.hIcon=icon;}
    void Tip(const wchar_t* tip){wcsncpy_s(data_.szTip,tip,_TRUNCATE);}
    bool Sync(uint64_t now){
        if(!enabled_)return false;
        const bool previouslyHealthy=Healthy();bool registered=false;
        data_.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_GUID|NIF_SHOWTIP;
        if(installed_&&!notify_(NIM_MODIFY,&data_)){installed_=versioned_=false;}
        if(!installed_){
            installed_=notify_(NIM_ADD,&data_)!=FALSE;
            if(!installed_)installed_=notify_(NIM_MODIFY,&data_)!=FALSE;
            registered=installed_;
            versioned_=false;
        }
        if(installed_&&!versioned_){data_.uVersion=NOTIFYICON_VERSION_4;versioned_=notify_(NIM_SETVERSION,&data_)!=FALSE;}
        due_=now+(Healthy()?5000:1000);
        if(!Healthy())++failures_;
        else if(registered||!previouslyHealthy)++registrations_;
        return Healthy();
    }
    void Tick(uint64_t now){if(enabled_&&now>=due_)Sync(now);}
    void ShellRecreated(uint64_t now){installed_=versioned_=false;Sync(now);}
    void Remove(){
        if(!enabled_)return;
        enabled_=false;data_.uFlags=NIF_GUID;notify_(NIM_DELETE,&data_);
        installed_=versioned_=false;
    }
    bool Healthy()const{return enabled_&&installed_&&versioned_;}
    unsigned Registrations()const{return registrations_;}
    unsigned Failures()const{return failures_;}
    uint64_t Deadline()const{return due_;}
    NOTIFYICONIDENTIFIER Identifier()const{
        NOTIFYICONIDENTIFIER id{};id.cbSize=sizeof(id);id.hWnd=data_.hWnd;id.uID=data_.uID;id.guidItem=data_.guidItem;return id;
    }
    bool Rect(RECT& bounds)const{auto id=Identifier();return enabled_&&SUCCEEDED(Shell_NotifyIconGetRect(&id,&bounds));}
private:
    Notify notify_;NOTIFYICONDATAW data_{};
    bool enabled_=false,installed_=false,versioned_=false;
    uint64_t due_=0;unsigned registrations_=0,failures_=0;
};

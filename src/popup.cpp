#include "display.h"
#include "channel.h"
#include "platform.h"
#include "capture-overlay.h"
#include "tray-toggle.h"
#include "startup.h"
#include "reconnect.h"
#include "notification-icon.h"
#include "resident.h"
#include <dbt.h>
#include <sstream>
#include <chrono>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <imm.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <initguid.h>
#include <ntddvdeo.h>

namespace {
constexpr wchar_t ClassName[] = L"XiaomaBrightness.Panel.1";
constexpr UINT TrayMessage=WM_APP+1, Discovered=WM_APP+2, Applied=WM_APP+3, ShowPanel=WM_APP+4, Quit=WM_APP+5, Dock=WM_APP+6, Rescan=WM_APP+7, RuntimeStateMessage=WM_APP+8, RepairTray=WM_APP+9, DiscoveryFailed=WM_APP+10;
constexpr UINT SaveTimer=2, TrayTimer=3, ScanTimer=4, HealthTimer=5, ConnectionTimer=7;
constexpr float Width=344, Height=120, TrackLeft=28, TrackRight=316, TrackY=85;
HWND window=nullptr, slider=nullptr;
HINSTANCE instance=nullptr;
HANDLE wake=nullptr, worker=nullptr;
std::atomic<bool> stopping{false}, scanRequested{true};
std::atomic<unsigned> channelEpoch{0};
std::atomic<bool> notificationPending{false}, staleWrite{false};
std::vector<std::unique_ptr<BrightnessChannel>> channels;
struct Overlay { HWND window=nullptr; RECT bounds{}; BYTE alpha=255; };
Displays displays;
std::map<std::wstring,Overlay> overlays;
int value=62;
float visual=62, dpiScale=1;
bool scanning=true, savedBrightness=false, userAdjusted=false, benchmarkMode=false, overlayFailed=false, startupRegistered=false;
std::wstring settingsPath;
UINT taskbarCreated=0;
NotificationIcon tray;
TrayToggle trayToggle;
ID2D1Factory* factory=nullptr;
ID2D1HwndRenderTarget* target=nullptr;
ID2D1SolidColorBrush* brush=nullptr;
IDWriteFactory* writeFactory=nullptr;
IDWriteTextFormat* titleFont=nullptr;
IDWriteTextFormat* smallFont=nullptr;
IDWriteTextFormat* valueFont=nullptr;
HICON icon=nullptr;
HDEVNOTIFY monitorNotification=nullptr;
HPOWERNOTIFY powerNotification=nullptr;
ReconnectPlan reconnect;

template<class T> void Release(T*& p) { if(p){p->Release();p=nullptr;} }
int Pixel(float dip) { return static_cast<int>(std::lround(dip*dpiScale)); }
void Repaint(bool immediate=false) { if(IsWindowVisible(window)) { InvalidateRect(window,nullptr,FALSE); if(immediate)UpdateWindow(window); } }
void SaveSoon() { SetTimer(window,SaveTimer,500,nullptr); }
std::wstring DisplayKey(const std::wstring& key) {
    unsigned long long hash=14695981039346656037ull;
    for(wchar_t c:key){hash^=static_cast<unsigned long long>(c);hash*=1099511628211ull;}
    return std::to_wstring(hash);
}
void InitializeSettings() {
    wchar_t path[MAX_PATH]{};
    if(SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,path))){
        std::wstring directory=std::wstring(path)+L"\\XiaomaBrightness";CreateDirectoryW(directory.c_str(),nullptr);settingsPath=directory+L"\\settings.ini";
        int stored=GetPrivateProfileIntW(L"Brightness",L"Value",-1,settingsPath.c_str());
        if(stored>=0&&stored<=100){value=stored;visual=static_cast<float>(value);savedBrightness=true;}
    }
}
void SaveSettings() {
    if(settingsPath.empty())return;
    const auto text=std::to_wstring(value);
    WritePrivateProfileStringW(L"Brightness",L"Value",text.c_str(),settingsPath.c_str());
}
void EnsureStartup() {
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,ARRAYSIZE(path));
    if(Startup::RegisterShortcut(path)){
        startupRegistered=true;
        HKEY previous=nullptr;
        if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_SET_VALUE,&previous)==ERROR_SUCCESS){RegDeleteValueW(previous,L"XiaomaBrightness");RegCloseKey(previous);}
        return;
    }
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return;
    std::wstring command=L"\""+std::wstring(path)+L"\" --tray";
    wchar_t current[32768]{};DWORD size=sizeof(current);
    if(RegQueryValueExW(key,L"XiaomaBrightness",nullptr,nullptr,reinterpret_cast<BYTE*>(current),&size)!=ERROR_SUCCESS||command!=current)
        RegSetValueExW(key,L"XiaomaBrightness",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
    size=sizeof(current);startupRegistered=RegQueryValueExW(key,L"XiaomaBrightness",nullptr,nullptr,reinterpret_cast<BYTE*>(current),&size)==ERROR_SUCCESS&&command==current;
    RegCloseKey(key);
}

void FreeDrawing() {
    Release(brush);Release(target);Release(titleFont);Release(smallFont);Release(valueFont);Release(writeFactory);Release(factory);
}
bool DrawingReady() {
    if(!factory&&FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&factory)))return false;
    if(!writeFactory&&FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&writeFactory))))return false;
    auto createFont=[](IDWriteTextFormat*& font,float size,DWRITE_FONT_WEIGHT weight){
        if(font)return;
        if(SUCCEEDED(writeFactory->CreateTextFormat(L"Microsoft YaHei UI",nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"zh-CN",&font))){font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);}
    };
    createFont(titleFont,14,DWRITE_FONT_WEIGHT_SEMI_BOLD);createFont(smallFont,10,DWRITE_FONT_WEIGHT_NORMAL);createFont(valueFont,25,DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if(!titleFont||!smallFont||!valueFont)return false;
    if(!target){
        RECT client{};GetClientRect(window,&client);
        auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),dpiScale*96,dpiScale*96);
        if(FAILED(factory->CreateHwndRenderTarget(properties,D2D1::HwndRenderTargetProperties(window,D2D1::SizeU(static_cast<UINT>(client.right),static_cast<UINT>(client.bottom)),D2D1_PRESENT_OPTIONS_IMMEDIATELY),&target)))return false;
        if(FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0xffffff),&brush))){Release(target);return false;}
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    }
    return true;
}
void Color(UINT32 color){brush->SetColor(D2D1::ColorF(color));}
void Line(float x1,float y1,float x2,float y2,UINT32 color,float stroke=1){Color(color);target->DrawLine(D2D1::Point2F(x1,y1),D2D1::Point2F(x2,y2),brush,stroke);}
void Circle(float x,float y,float radius,UINT32 color,bool outline=false,float stroke=1){Color(color);auto c=D2D1::Ellipse(D2D1::Point2F(x,y),radius,radius);if(outline)target->DrawEllipse(c,brush,stroke);else target->FillEllipse(c,brush);}
void Round(float x,float y,float width,float height,float radius,UINT32 color,bool outline=false){Color(color);auto r=D2D1::RoundedRect(D2D1::RectF(x,y,x+width,y+height),radius,radius);if(outline)target->DrawRoundedRectangle(r,brush,1);else target->FillRoundedRectangle(r,brush);}
void Text(const std::wstring& text,IDWriteTextFormat* font,float x,float y,float width,float height,UINT32 color,DWRITE_TEXT_ALIGNMENT align=DWRITE_TEXT_ALIGNMENT_LEADING){font->SetTextAlignment(align);Color(color);target->DrawText(text.c_str(),static_cast<UINT>(text.size()),font,D2D1::RectF(x,y,x+width,y+height),brush,D2D1_DRAW_TEXT_OPTIONS_CLIP);}
std::wstring Status() {
    if(overlayFailed)return L"暂时无法调节，请重试";
    if(scanning)return L"正在连接显示器…";
    if(displays.empty())return L"正在等待显示器连接";
    bool error=false,pending=false,soft=false;
    for(const auto& channel:channels){auto state=channel->Snapshot();error=error||state.error!=0;pending=pending||state.requested!=state.completed;soft=soft||state.software;}
    if(error)return L"正在重新同步屏幕…";
    if(pending)return L"正在同步所有屏幕…";
    std::wstring count=displays.size()==2?L"两块屏幕":(displays.size()==1?L"屏幕":std::to_wstring(displays.size())+L" 块屏幕");
    return count+(soft?L"统一调节 · 含画面调暗":L"亮度已锁定同步");
}
void DrawFrame(){
    target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Identity());target->Clear(D2D1::ColorF(0x171f26));
    Round(0.5f,0.5f,Width-1,Height-1,13,0x35424b,true);
    Text(L"小马亮度",titleFont,49,15,190,27,0xe4edf2);
    Text(std::to_wstring(value),valueFont,245,10,64,36,0xf2faf6,DWRITE_TEXT_ALIGNMENT_TRAILING);Text(L"%",smallFont,313,25,17,19,0x899fa9);
    Text(Status(),smallFont,23,45,304,18,0x81949f);
    const float thumb=TrackLeft+(TrackRight-TrackLeft)*visual/100;
    Round(TrackLeft,TrackY-2.5f,TrackRight-TrackLeft,5,2.5f,0x3b4a55);
    Round(TrackLeft,TrackY-2.5f,std::max(0.1f,thumb-TrackLeft),5,2.5f,0x8ce5c5);
    if(GetFocus()==slider)Circle(thumb,TrackY,11,0x577c6b,true,1.2f);
    Circle(thumb,TrackY+1,8,0x10171c);Circle(thumb,TrackY,7.5f,0xeffff7);Circle(thumb,TrackY,2.5f,0x8fd9bc);
    HRESULT result=target->EndDraw();if(result==D2DERR_RECREATE_TARGET){Release(brush);Release(target);}
}
void PaintFallback(HDC dc){
    RECT bounds{};GetClientRect(window,&bounds);HBRUSH background=CreateSolidBrush(RGB(23,31,38));FillRect(dc,&bounds,background);DeleteObject(background);
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,244,245));
    HFONT font=CreateFontW(-Pixel(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    HGDIOBJ previous=SelectObject(dc,font);RECT name{Pixel(49),Pixel(15),Pixel(235),Pixel(44)};DrawTextW(dc,L"小马亮度",-1,&name,DT_SINGLELINE|DT_VCENTER);
    std::wstring text=std::to_wstring(value)+L"%";RECT number{Pixel(245),Pixel(15),Pixel(325),Pixel(44)};DrawTextW(dc,text.c_str(),-1,&number,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    text=Status();RECT status{Pixel(22),Pixel(45),Pixel(325),Pixel(65)};DrawTextW(dc,text.c_str(),-1,&status,DT_SINGLELINE|DT_VCENTER);
    const int x=Pixel(TrackLeft+(TrackRight-TrackLeft)*value/100.0f),y=Pixel(TrackY);
    HPEN track=CreatePen(PS_SOLID,Pixel(5),RGB(59,74,85));HGDIOBJ oldPen=SelectObject(dc,track);MoveToEx(dc,Pixel(TrackLeft),y,nullptr);LineTo(dc,Pixel(TrackRight),y);SelectObject(dc,oldPen);DeleteObject(track);
    track=CreatePen(PS_SOLID,Pixel(5),RGB(140,229,197));SelectObject(dc,track);MoveToEx(dc,Pixel(TrackLeft),y,nullptr);LineTo(dc,x,y);SelectObject(dc,oldPen);DeleteObject(track);
    HBRUSH thumb=CreateSolidBrush(RGB(239,255,247));HGDIOBJ oldBrush=SelectObject(dc,thumb);Ellipse(dc,x-Pixel(7),y-Pixel(7),x+Pixel(7),y+Pixel(7));SelectObject(dc,oldBrush);DeleteObject(thumb);
    SelectObject(dc,previous);DeleteObject(font);
}
void Paint() {
    PAINTSTRUCT paint{};BeginPaint(window,&paint);
    if(DrawingReady())DrawFrame();else PaintFallback(paint.hdc);
    const int edge=Pixel(18);
    HICON brand=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(2),IMAGE_ICON,edge,edge,LR_SHARED));
    if(brand)DrawIconEx(paint.hdc,Pixel(22),Pixel(20),brand,edge,edge,0,nullptr,DI_NORMAL);
    EndPaint(window,&paint);
}

LRESULT CALLBACK OverlayProc(HWND hwnd,UINT message,WPARAM w,LPARAM l){
    if(message==WM_NCHITTEST)return HTTRANSPARENT;
    if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(message==WM_ERASEBKGND){RECT r{};GetClientRect(hwnd,&r);FillRect(reinterpret_cast<HDC>(w),&r,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));return 1;}
    return DefWindowProcW(hwnd,message,w,l);
}
void UpdateOverlays(){
    if(benchmarkMode)return;
    overlayFailed=false;
    std::vector<bool> software;for(const auto& channel:channels)software.push_back(channel->Snapshot().software);
    const auto regions=Platform::SoftwareRegions(displays,software);
    for(auto it=overlays.begin();it!=overlays.end();){if(regions.find(it->first)==regions.end()){DestroyWindow(it->second.window);it=overlays.erase(it);}else ++it;}
    for(const auto& region:regions){
        auto found=overlays.find(region.first);
        if(value>=100){if(found!=overlays.end())ShowWindow(found->second.window,SW_HIDE);continue;}
        if(found==overlays.end()){
            const RECT& r=region.second;Overlay overlay;overlay.bounds=r;
            overlay.window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"XiaomaBrightness.Soft",L"小马亮度柔光",WS_POPUP,r.left,r.top,r.right-r.left,r.bottom-r.top,nullptr,nullptr,instance,nullptr);
            if(!overlay.window){overlayFailed=true;continue;}
            BrightnessCapture::ExcludeSoftOverlay(overlay.window);
            found=overlays.emplace(region.first,overlay).first;
        }
        auto& overlay=found->second;const BYTE alpha=Platform::OverlayAlpha(value);
        const auto& r=region.second;
        if(!EqualRect(&overlay.bounds,&r)){
            SetWindowPos(overlay.window,HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE);
            overlay.bounds=r;
        }
        if(overlay.alpha!=alpha){if(!SetLayeredWindowAttributes(overlay.window,0,alpha,LWA_ALPHA)){overlayFailed=true;ShowWindow(overlay.window,SW_HIDE);continue;}overlay.alpha=alpha;}
        if(!IsWindowVisible(overlay.window))SetWindowPos(overlay.window,HWND_TOPMOST,0,0,0,0,SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    }
    if(IsWindowVisible(window)&&!regions.empty())SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
void UpdateTrayTip(){
    std::wstring tip=L"小马亮度 · "+std::to_wstring(value)+L"%";
    for(size_t i=0;i<displays.size();++i){auto state=channels[i]->Snapshot();tip+=L"\n"+displays[i]->name+(state.software?L" · 画面调暗":L" · 硬件亮度");if(state.error)tip+=L" · 重试中";}
    tray.Tip(tip.c_str());tray.Sync(GetTickCount64());
}
HICON LoadTrayIcon(){
    DWORD light=0,bytes=sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"SystemUsesLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&bytes);
    const UINT dpi=Platform::WindowDpi(window);
    int size=Platform::SmallIconSize(dpi?dpi:96);
    HICON result=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(light?3:2),IMAGE_ICON,size,size,LR_SHARED));
    return result?result:icon;
}
void RefreshTrayIcon(){
    tray.Icon(LoadTrayIcon());tray.Sync(GetTickCount64());
}
void InstallTray(){
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,ARRAYSIZE(executable));
    tray.Initialize(window,TrayMessage,LoadTrayIcon(),NotificationIdentity(executable));UpdateTrayTip();
}
void SetBrightness(int next,bool=true,bool force=false){
    next=std::clamp(next,0,100);
    bool errors=std::any_of(channels.begin(),channels.end(),[](const auto& channel){return channel->Snapshot().error!=0;});
    if(next==value&&!errors&&!force)return;
    value=next;visual=static_cast<float>(next);userAdjusted=true;
    for(const auto& channel:channels)channel->Submit(next,force||errors);
    SendMessageW(slider,TBM_SETPOS,FALSE,next);
    if(!benchmarkMode)NotifyWinEvent(EVENT_OBJECT_VALUECHANGE,slider,OBJID_CLIENT,CHILDID_SELF);
    UpdateOverlays();if(!benchmarkMode){SetTimer(window,TrayTimer,150,nullptr);SetTimer(window,HealthTimer,100,nullptr);SaveSoon();}Repaint(true);
}
void RequestRescan(unsigned delay=200){
    reconnect.Changed(GetTickCount64(),delay);
    const uint64_t now=GetTickCount64(),due=reconnect.Deadline();
    SetTimer(window,ScanTimer,static_cast<UINT>(due>now?due-now:1),nullptr);
}
void StartDiscovery(){
    if(scanning||stopping)return;
    reconnect.Started();scanning=true;scanRequested=true;SetEvent(wake);Repaint();
}
void CheckConnections(){
    const uint64_t now=GetTickCount64();
    const auto registrations=tray.Registrations(),failures=tray.Failures();tray.Tick(now);
    if(tray.Registrations()!=registrations)Resident::Log("tray_restored",tray.Registrations());
    if(tray.Failures()!=failures&&tray.Failures()==failures+1&&failures==0)Resident::Log("tray_not_ready");
    bool healthy=!channels.empty();
    for(const auto& channel:channels){
        channel->CheckTimeout();const auto state=channel->Snapshot();
        healthy=healthy&&!state.software&&!state.needsDiscovery&&!state.error;
    }
    if(!scanning)reconnect.Observe(now,healthy);
    if(!scanning&&reconnect.PollDue(now)){
        if(CurrentDisplayTopology()!=DisplayTopology(displays))RequestRescan();
        for(const auto& channel:channels){const auto state=channel->Snapshot();if(!state.software&&!state.inFlight&&state.requested==state.verified)channel->Revalidate();}
    }
    if(!scanning&&reconnect.Ready(now))StartDiscovery();
    UpdateOverlays();
}
DWORD WINAPI WorkerProc(void*){
    DisplayControl control;
    while(!stopping){
        if(scanRequested.exchange(false)){
            try{
                auto found=control.Scan();
                if(stopping)break;
                auto message=new Displays(std::move(found));if(!PostMessageW(window,Discovered,0,reinterpret_cast<LPARAM>(message)))delete message;
            }catch(...){if(!stopping)PostMessageW(window,DiscoveryFailed,0,0);}
        }
        if(!stopping)WaitForSingleObject(wake,INFINITE);
    }
    return 0;
}

void HidePanel(bool preserveTrayPress=false){
    if(!preserveTrayPress)trayToggle.Reset();
    ShowWindow(window,SW_HIDE);FreeDrawing();SaveSettings();
}
bool PressedOwnTrayIcon(HWND activated){
    RECT bounds{};
    POINT point{};
    if(!tray.Rect(bounds)||!GetCursorPos(&point)||!PtInRect(&bounds,point))return false;
    const int button=GetSystemMetrics(SM_SWAPBUTTON)?VK_RBUTTON:VK_LBUTTON;
    if(GetAsyncKeyState(button)<0)return true;
    // The focus notification can be delivered after a quick mouse release.
    wchar_t name[64]{};HWND root=activated?GetAncestor(activated,GA_ROOT):nullptr;
    if(root)GetClassNameW(root,name,ARRAYSIZE(name));
    return wcscmp(name,L"Shell_TrayWnd")==0||wcscmp(name,L"Shell_SecondaryTrayWnd")==0||wcscmp(name,L"NotifyIconOverflowWindow")==0||wcscmp(name,L"TopLevelWindowXamlIsland")==0;
}
void PositionPanel(){
    RECT trayBounds{};bool located=tray.Rect(trayBounds);
    POINT point{};GetCursorPos(&point);HMONITOR monitor=located?MonitorFromRect(&trayBounds,MONITOR_DEFAULTTOPRIMARY):MonitorFromPoint(point,MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};info.cbSize=sizeof(info);GetMonitorInfoW(monitor,&info);
    auto layout=Platform::PlacePanel(info.rcWork,Platform::WindowDpi(window));
    if(!layout.valid)return;
    if(std::abs(dpiScale-layout.scale)>0.001f)FreeDrawing();
    dpiScale=layout.scale;
    SetWindowPos(window,HWND_TOPMOST,layout.x,layout.y,layout.width,layout.height,SWP_NOACTIVATE);
    SetWindowPos(slider,nullptr,Pixel(18),Pixel(64),Pixel(308),Pixel(42),SWP_NOZORDER|SWP_NOACTIVATE);
}
void OpenPanel(){
    trayToggle.Reset();
    PositionPanel();ShowWindow(window,SW_SHOWNOACTIVATE);ShowWindow(window,SW_SHOWNORMAL);SetForegroundWindow(window);Repaint();
    for(const auto& channel:channels)channel->Refresh();
    RequestRescan(1);
}
LRESULT CALLBACK SliderProc(HWND hwnd,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR){
    if(message==WM_PAINT){PAINTSTRUCT paint{};BeginPaint(hwnd,&paint);EndPaint(hwnd,&paint);return 0;}
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS)Repaint();
    if(message==WM_LBUTTONDOWN||(message==WM_MOUSEMOVE&&GetCapture()==hwnd)){
        if(message==WM_LBUTTONDOWN){SetFocus(hwnd);SetCapture(hwnd);}
        SetBrightness(Platform::PointerValue(GET_X_LPARAM(l),dpiScale),true);return 0;
    }
    if(message==WM_LBUTTONUP){ReleaseCapture();Repaint();return 0;}
    if(message==WM_MOUSEWHEEL){static int remainder=0;remainder+=GET_WHEEL_DELTA_WPARAM(w);int delta=remainder/WHEEL_DELTA;remainder%=WHEEL_DELTA;if(GetKeyState(VK_CONTROL)<0)delta*=5;if(delta)SetBrightness(value+delta);return 0;}
    if(message==WM_KEYDOWN){
        if(w==VK_ESCAPE){HidePanel();return 0;}
        int next=value;bool action=true;
        switch(w){case VK_LEFT:case VK_DOWN:--next;break;case VK_RIGHT:case VK_UP:++next;break;case VK_PRIOR:next+=10;break;case VK_NEXT:next-=10;break;case VK_HOME:next=0;break;case VK_END:next=100;break;default:action=false;}
        if(action){SetBrightness(next);return 0;}
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,SliderProc,id);
    return DefSubclassProc(hwnd,message,w,l);
}
void OnDiscovered(Displays* message){
    std::unique_ptr<Displays> received(message);
    channelEpoch.fetch_add(1);
    for(const auto& channel:channels)channel->Stop(0);
    // Keep known hardware capabilities when a busy bus makes a rescan temporarily unreadable.
    for(const auto& current:*received)if(current->backend==Backend::Soft){
        auto old=std::find_if(displays.begin(),displays.end(),[&](const auto& previous){return previous->key==current->key&&previous->backend!=Backend::Soft;});
        if(old!=displays.end()&&(current->physicalValid||(*old)->backend==Backend::Wmi)){current->backend=(*old)->backend;current->minimum=(*old)->minimum;current->maximum=(*old)->maximum;current->wmiPath=(*old)->wmiPath;current->levels=(*old)->levels;}
    }
    channels.clear();displays=*received;scanning=false;
    reconnect.Finished(GetTickCount64());
    if(!savedBrightness&&!userAdjusted&&!displays.empty()){
        int legacy=GetPrivateProfileIntW(L"SoftBrightness",DisplayKey(displays.front()->key).c_str(),-1,settingsPath.c_str());
        value=std::clamp(legacy>=0&&legacy<=100?legacy:displays.front()->original,0,100);visual=static_cast<float>(value);savedBrightness=true;
    }
    const unsigned epoch=channelEpoch.load();const HWND mailbox=window;
    for(const auto& monitor:displays){
        auto notify=[epoch,mailbox]{
            if(stopping)return;
            if(epoch!=channelEpoch.load())staleWrite=true;
            if(!notificationPending.exchange(true)){if(!PostMessageW(mailbox,Applied,0,0))notificationPending=false;}
        };
        channels.push_back(std::make_unique<BrightnessChannel>(monitor->original,monitor->backend==Backend::Soft,NativeBrightnessFactory(monitor),notify));
    }
    SetBrightness(value,true,true);UpdateTrayTip();Repaint();
}
void TrayMenu(){
    HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"显示亮度条");
    POINT point{};GetCursorPos(&point);SetForegroundWindow(window);UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);DestroyMenu(menu);PostMessageW(window,WM_NULL,0,0);
    if(command==1)OpenPanel();
}
void Shutdown(){
    if(stopping.exchange(true))return;
    Resident::RequestStop();Resident::Log("panel_maintenance_or_session_stop");
    if(monitorNotification){UnregisterDeviceNotification(monitorNotification);monitorNotification=nullptr;}
    if(powerNotification){UnregisterPowerSettingNotification(powerNotification);powerNotification=nullptr;}
    KillTimer(window,ConnectionTimer);KillTimer(window,ScanTimer);
    SaveSettings();tray.Remove();ShowWindow(window,SW_HIDE);
    for(const auto& overlay:overlays)DestroyWindow(overlay.second.window);overlays.clear();if(wake)SetEvent(wake);
    ULONGLONG flushDeadline=GetTickCount64()+250;
    while(GetTickCount64()<flushDeadline){bool complete=true;for(const auto& channel:channels){auto state=channel->Snapshot();complete=complete&&state.completed==state.requested;}if(complete)break;Sleep(1);}
    for(const auto& channel:channels)channel->Stop(0);
    if(worker){WaitForSingleObject(worker,1000);CloseHandle(worker);worker=nullptr;}
    ULONGLONG stopDeadline=GetTickCount64()+500;
    for(const auto& channel:channels){ULONGLONG tick=GetTickCount64();channel->Stop(tick<stopDeadline?static_cast<DWORD>(stopDeadline-tick):0);}channels.clear();
    MSG message{};while(PeekMessageW(&message,window,Discovered,Discovered,PM_REMOVE))delete reinterpret_cast<Displays*>(message.lParam);
    if(wake){CloseHandle(wake);wake=nullptr;}FreeDrawing();DestroyWindow(window);
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM w,LPARAM l){
    if(message==taskbarCreated&&taskbarCreated){tray.ShellRecreated(GetTickCount64());Resident::Log("taskbar_recreated");return 0;}
    switch(message){
    case WM_PAINT:Paint();return 0;
    case WM_ERASEBKGND:return 1;
    case WM_SIZE:if(target)target->Resize(D2D1::SizeU(LOWORD(l),HIWORD(l)));return 0;
    case WM_CLOSE:HidePanel();return 0;
    case WM_ACTIVATE:
        if(LOWORD(w)==WA_INACTIVE&&GetCapture()==nullptr){
            trayToggle.Deactivated(IsWindowVisible(hwnd)!=FALSE,PressedOwnTrayIcon(reinterpret_cast<HWND>(l)));
            HidePanel(true);
        }
        return 0;
    case WM_KEYDOWN:if(w==VK_ESCAPE){HidePanel();return 0;}break;
    case WM_COMMAND:if(LOWORD(w)==IDCANCEL){HidePanel();return 0;}break;
    case WM_HSCROLL:if(reinterpret_cast<HWND>(l)==slider){SetBrightness(static_cast<int>(SendMessageW(slider,TBM_GETPOS,0,0)),true);return 0;}break;
    case WM_DPICHANGED:dpiScale=HIWORD(w)/96.0f;FreeDrawing();PositionPanel();RefreshTrayIcon();Repaint();return 0;
    case WM_DISPLAYCHANGE:RequestRescan();return 0;
    case WM_DEVICECHANGE:
        if(w==DBT_DEVNODES_CHANGED)RequestRescan();
        else if((w==DBT_DEVICEARRIVAL||w==DBT_DEVICEREMOVECOMPLETE)&&l){
            const auto header=reinterpret_cast<const DEV_BROADCAST_HDR*>(l);
            if(header->dbch_devicetype==DBT_DEVTYP_DEVICEINTERFACE&&header->dbch_size>=sizeof(DEV_BROADCAST_DEVICEINTERFACE_W)){
                const auto change=reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(l);
                if(IsEqualGUID(change->dbcc_classguid,GUID_DEVINTERFACE_MONITOR))RequestRescan();
            }
        }
        return TRUE;
    case WM_POWERBROADCAST:
        if(w==PBT_APMRESUMEAUTOMATIC||w==PBT_APMRESUMESUSPEND)RequestRescan(300);
        else if(w==PBT_POWERSETTINGCHANGE&&l){
            const auto setting=reinterpret_cast<const POWERBROADCAST_SETTING*>(l);
            if(IsEqualGUID(setting->PowerSetting,GUID_CONSOLE_DISPLAY_STATE)&&setting->DataLength>=sizeof(DWORD)){
                DWORD state=0;memcpy(&state,setting->Data,sizeof(state));if(state==1)RequestRescan(300);
            }
        }
        return TRUE;
    case WM_SETTINGCHANGE:if(w==SPI_SETWORKAREA)PositionPanel();RefreshTrayIcon();return 0;
    case WM_THEMECHANGED:RefreshTrayIcon();return 0;
    case TrayMessage:{
        UINT event=LOWORD(l);
        if(event==WM_LBUTTONDOWN||event==WM_LBUTTONDBLCLK)trayToggle.MouseDown(IsWindowVisible(hwnd)!=FALSE);
        else if(event==NIN_SELECT||event==NIN_KEYSELECT){if(event==NIN_KEYSELECT)trayToggle.Reset();if(trayToggle.Select(IsWindowVisible(hwnd)!=FALSE))OpenPanel();else HidePanel();}
        else if(event==WM_CONTEXTMENU||event==WM_RBUTTONUP){trayToggle.Reset();TrayMenu();}
        return 0;
    }
    case Discovered:OnDiscovered(reinterpret_cast<Displays*>(l));return 0;
    case DiscoveryFailed:scanning=false;Resident::Log("discovery_failed");RequestRescan(1000);return 0;
    case Rescan:RequestRescan(1);return 0;
    case RepairTray:tray.ShellRecreated(GetTickCount64());return 0;
    case Resident::HeartbeatMessage:return Resident::HeartbeatReply;
    case RuntimeStateMessage:
        if(w==99)return 0x116;
        if(w==0)return value;
        if(w==1)return static_cast<LRESULT>(channels.size());
        if(w==3)return scanning;
        if(w==4)return channelEpoch.load();
        if(w==5)return IsWindowVisible(window);
        if(w==6)return tray.Healthy();
        if(w==7){RECT bounds{};return tray.Rect(bounds);}
        if(w==8)return tray.Registrations();
        if(w==9)return Resident::IsRunning();
        if(w==10)return startupRegistered;
        if(w==11)return tray.Failures();
        if(w==2&&l>=0&&static_cast<size_t>(l)<channels.size()){
            const auto state=channels[static_cast<size_t>(l)]->Snapshot();
            return (state.software?1:0)|(state.error?2:0)|(state.needsDiscovery?4:0)|(state.inFlight?8:0)|(state.requested==state.verified?16:0);
        }
        return -1;
    case Applied:notificationPending=false;if(staleWrite.exchange(false)){for(const auto& channel:channels)channel->Submit(value,true);}UpdateOverlays();SetTimer(hwnd,TrayTimer,100,nullptr);SetTimer(hwnd,HealthTimer,100,nullptr);Repaint();return 0;
    case ShowPanel:OpenPanel();return 0;
    case Dock:SetWindowLongPtrW(hwnd,GWL_EXSTYLE,(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&~WS_EX_APPWINDOW)|WS_EX_TOOLWINDOW);SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);HidePanel();return 0;
    case Quit:Shutdown();return 0;
    case WM_QUERYENDSESSION:SaveSettings();return TRUE;
    case WM_ENDSESSION:if(w)Shutdown();return 0;
    case WM_TIMER:
        if(w==ConnectionTimer){CheckConnections();return 0;}
        if(w==HealthTimer){bool waiting=false;for(const auto& channel:channels){channel->CheckTimeout();auto state=channel->Snapshot();waiting=waiting||(!state.software&&(state.inFlight||state.requested!=state.verified));}UpdateOverlays();Repaint();if(!waiting)KillTimer(hwnd,HealthTimer);return 0;}
        if(w==SaveTimer){KillTimer(hwnd,SaveTimer);SaveSettings();return 0;}
        if(w==TrayTimer){KillTimer(hwnd,TrayTimer);UpdateTrayTip();return 0;}
        if(w==ScanTimer){KillTimer(hwnd,ScanTimer);if(reconnect.Ready(GetTickCount64()))StartDiscovery();return 0;}break;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
} // namespace

bool WriteRunningStatus(const std::wstring& path){
    HWND existing=FindWindowW(ClassName,nullptr);if(!existing)return false;
    bool responsive=true;
    auto read=[&](WPARAM kind,LPARAM index=0)->DWORD_PTR{
        DWORD_PTR result=0;if(!SendMessageTimeoutW(existing,RuntimeStateMessage,kind,index,SMTO_ABORTIFHUNG|SMTO_BLOCK,1000,&result))responsive=false;return result;
    };
    if(read(99)!=0x116||!responsive)return false;
    const auto epoch=read(4),brightness=read(0),count=read(1);if(count>64)return false;
    std::ostringstream out;out<<"{\"version\":\"1.1.6\",\"value\":"<<brightness<<",\"displayCount\":"<<count<<",\"scanning\":"<<(read(3)?"true":"false")<<",\"panelVisible\":"<<(read(5)?"true":"false")<<",\"trayRegistered\":"<<(read(6)?"true":"false")<<",\"trayLocated\":"<<(read(7)?"true":"false")<<",\"trayRegistrations\":"<<read(8)<<",\"residentRunning\":"<<(read(9)?"true":"false")<<",\"startupRegistered\":"<<(read(10)?"true":"false")<<",\"trayFailures\":"<<read(11)<<",\"discoveryEpoch\":"<<epoch<<",\"channels\":[";
    for(size_t index=0;index<count;++index){const auto flags=read(2,static_cast<LPARAM>(index));if(index)out<<',';out<<"{\"software\":"<<((flags&1)?"true":"false")<<",\"error\":"<<((flags&2)?"true":"false")<<",\"needsDiscovery\":"<<((flags&4)?"true":"false")<<",\"inFlight\":"<<((flags&8)?"true":"false")<<",\"verified\":"<<((flags&16)?"true":"false")<<'}';}
    if(read(4)!=epoch)return false;
    out<<"],\"responsive\":"<<(responsive?"true":"false")<<"}\n";return responsive&&WriteUtf8File(path,out.str());
}

bool WriteStartupCheck(const std::wstring& path){
    const auto shortcut=Startup::InspectShortcut();wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,ARRAYSIZE(executable));
    const bool valid=shortcut.readable&&_wcsicmp(shortcut.target.c_str(),executable)==0&&shortcut.arguments==L"--tray";
    std::ostringstream out;out<<"{\"readable\":"<<(shortcut.readable?"true":"false")<<",\"targetMatches\":"<<(_wcsicmp(shortcut.target.c_str(),executable)==0?"true":"false")<<",\"hiddenArguments\":"<<(shortcut.arguments==L"--tray"?"true":"false")<<",\"valid\":"<<(valid?"true":"false")<<"}\n";
    return WriteUtf8File(path,out.str())&&valid;
}

#ifdef XIAOMA_TEST_BUILD
bool RunUiBenchmark(const std::wstring& path){
    benchmarkMode=true;scanning=false;
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=instance;cls.lpfnWndProc=DefWindowProcW;cls.lpszClassName=L"XiaomaBrightness.HiddenRenderTest";RegisterClassExW(&cls);
    window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"",WS_POPUP,0,0,344,120,nullptr,nullptr,instance,nullptr);
    if(!window)return false;
    slider=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD|TBS_NOTICKS,0,0,1,1,window,nullptr,instance,nullptr);
    std::ostringstream out;out<<"{\"visible\":"<<(IsWindowVisible(window)?"true":"false")<<",\"profiles\":[";
    bool passed=true;size_t profile=0;
    for(UINT dpi:{96u,120u,144u,168u,192u,240u,288u,384u}){
        FreeDrawing();dpiScale=dpi/96.0f;
        SetWindowPos(window,nullptr,0,0,Pixel(Width),Pixel(Height),SWP_NOZORDER|SWP_NOACTIVATE);
        auto cold=std::chrono::steady_clock::now();bool ready=DrawingReady();if(ready)DrawFrame();
        double coldMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-cold).count();
        std::vector<double> frames;frames.reserve(1000);
        for(int i=0;i<1000&&ready;++i){
            auto start=std::chrono::steady_clock::now();SetBrightness((i*37)%101,true);ready=DrawingReady();if(ready)DrawFrame();
            frames.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        }
        std::sort(frames.begin(),frames.end());passed=passed&&ready&&frames.size()==1000;
        if(profile++)out<<',';out<<"{\"dpi\":"<<dpi<<",\"frames\":"<<frames.size()<<",\"coldMs\":"<<coldMs<<",\"p50Ms\":"<<(frames.empty()?0:frames[frames.size()/2])<<",\"p95Ms\":"<<(frames.empty()?0:frames[static_cast<size_t>(frames.size()*.95)])<<",\"maxMs\":"<<(frames.empty()?0:frames.back())<<'}';
    }
    passed=passed&&!IsWindowVisible(window);FreeDrawing();DestroyWindow(window);window=nullptr;slider=nullptr;
    out<<"],\"passed\":"<<(passed?"true":"false")<<",\"note\":\"Hidden native renderer CPU benchmark, not display input-to-photon latency\"}\n";
    return WriteUtf8File(path,out.str())&&passed;
}
#endif

int WINAPI wWinMain(HINSTANCE app,HINSTANCE,LPWSTR,int){
    instance=app;Platform::EnableDpi();
    ImmDisableIME(static_cast<DWORD>(-1));
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool startHidden=false,inspect=false,residentChild=false;
    for(int i=1;i<argc;++i){
        if(_wcsicmp(argv[i],L"--startup-check")==0&&i+1<argc){std::wstring path=argv[i+1];LocalFree(argv);return WriteStartupCheck(path)?0:2;}
        if(_wcsicmp(argv[i],L"--status")==0&&i+1<argc){std::wstring path=argv[i+1];LocalFree(argv);return WriteRunningStatus(path)?0:2;}
        if((_wcsicmp(argv[i],L"--probe")==0||_wcsicmp(argv[i],L"--hardware-test")==0)&&i+1<argc){bool test=_wcsicmp(argv[i],L"--hardware-test")==0;std::wstring path=argv[i+1];LocalFree(argv);return WriteProbe(path,test)?0:2;}
#ifdef XIAOMA_TEST_BUILD
        if((_wcsicmp(argv[i],L"--stress-test")==0||_wcsicmp(argv[i],L"--hardware-benchmark")==0||_wcsicmp(argv[i],L"--ui-benchmark")==0)&&i+1<argc){bool hardware=_wcsicmp(argv[i],L"--hardware-benchmark")==0;bool render=_wcsicmp(argv[i],L"--ui-benchmark")==0;std::wstring path=argv[i+1];LocalFree(argv);return (hardware?RunHardwareBenchmark(path):render?RunUiBenchmark(path):RunStressTests(path))?0:2;}
#endif
        if(_wcsicmp(argv[i],L"--tray")==0)startHidden=true;
        if(_wcsicmp(argv[i],L"--resident-child")==0)residentChild=true;
        if(_wcsicmp(argv[i],L"--inspect")==0)inspect=true;
        if(_wcsicmp(argv[i],L"--quit")==0||_wcsicmp(argv[i],L"--dock")==0||_wcsicmp(argv[i],L"--rescan")==0||_wcsicmp(argv[i],L"--repair-tray")==0){UINT command=_wcsicmp(argv[i],L"--quit")==0?Quit:(_wcsicmp(argv[i],L"--rescan")==0?Rescan:(_wcsicmp(argv[i],L"--repair-tray")==0?RepairTray:Dock));if(command==Quit)Resident::RequestStop();HWND existing=FindWindowW(ClassName,nullptr);if(existing)PostMessageW(existing,command,0,0);LocalFree(argv);return 0;}
    }
    if(argv)LocalFree(argv);
    if(!residentChild){
        HWND existing=FindWindowW(ClassName,nullptr);
        if(existing){if(!startHidden){AllowSetForegroundWindow(ASFW_ANY);PostMessageW(existing,ShowPanel,0,0);}return 0;}
        return Resident::Run(startHidden,inspect);
    }
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\XiaomaBrightness.1");
    if(GetLastError()==ERROR_ALREADY_EXISTS){HWND existing=FindWindowW(ClassName,nullptr);if(existing&&!startHidden){AllowSetForegroundWindow(ASFW_ANY);PostMessageW(existing,ShowPanel,0,0);}if(mutex)CloseHandle(mutex);return 0;}
    Resident::Log("panel_start");InitializeSettings();EnsureStartup();
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES};InitCommonControlsEx(&common);icon=LoadIconW(instance,MAKEINTRESOURCEW(1));
    WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=instance;cls.lpfnWndProc=WindowProc;cls.lpszClassName=ClassName;cls.hIcon=icon;cls.hIconSm=icon;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.style=CS_DROPSHADOW;RegisterClassExW(&cls);
    WNDCLASSEXW soft{};soft.cbSize=sizeof(soft);soft.hInstance=instance;soft.lpfnWndProc=OverlayProc;soft.lpszClassName=L"XiaomaBrightness.Soft";soft.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));RegisterClassExW(&soft);
    taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");UINT dpi=Platform::WindowDpi(nullptr);dpiScale=dpi?dpi/96.0f:1;
    window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_CONTROLPARENT|(inspect?WS_EX_APPWINDOW:WS_EX_TOOLWINDOW),ClassName,L"小马亮度",WS_POPUP,0,0,Pixel(Width),Pixel(Height),nullptr,nullptr,instance,nullptr);
    if(!window){if(mutex)CloseHandle(mutex);return 1;}
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};filter.dbcc_size=sizeof(filter);filter.dbcc_devicetype=DBT_DEVTYP_DEVICEINTERFACE;filter.dbcc_classguid=GUID_DEVINTERFACE_MONITOR;
    monitorNotification=RegisterDeviceNotificationW(window,&filter,DEVICE_NOTIFY_WINDOW_HANDLE);
    powerNotification=RegisterPowerSettingNotification(window,&GUID_CONSOLE_DISPLAY_STATE,DEVICE_NOTIFY_WINDOW_HANDLE);
    SetTimer(window,ConnectionTimer,500,nullptr);
    dpiScale=Platform::WindowDpi(window)/96.0f;
    DWORD dark=TRUE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));DWORD corner=2;DwmSetWindowAttribute(window,33,&corner,sizeof(corner));COLORREF border=RGB(53,66,75);DwmSetWindowAttribute(window,34,&border,sizeof(border));
    slider=CreateWindowExW(WS_EX_TRANSPARENT,TRACKBAR_CLASSW,L"所有显示器的亮度",WS_CHILD|WS_VISIBLE|WS_TABSTOP|TBS_NOTICKS|TBS_FIXEDLENGTH,0,0,1,1,window,reinterpret_cast<HMENU>(1000),instance,nullptr);
    SendMessageW(slider,TBM_SETRANGE,FALSE,MAKELPARAM(0,100));SendMessageW(slider,TBM_SETPOS,FALSE,value);SendMessageW(slider,TBM_SETLINESIZE,0,1);SendMessageW(slider,TBM_SETPAGESIZE,0,10);SetWindowSubclass(slider,SliderProc,1,0);EnableWindow(slider,TRUE);
    InstallTray();wake=CreateEventW(nullptr,FALSE,TRUE,nullptr);worker=CreateThread(nullptr,0,WorkerProc,nullptr,0,nullptr);
    if(!startHidden)OpenPanel();
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    displays.clear();if(mutex)CloseHandle(mutex);return 0;
}


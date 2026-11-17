#include "bsnes.hpp"
#include <algorithm>
#include <cstring>
#include <sfc/sfc.hpp>
#undef platform
#include <array>
#include <string>
#include <vector>
#include "fastlink/video-size.hpp"

HWND mainWindow{}, videoWindow{}, statusWindow{}, optionsWindow{};
int modalDepth = 0;
bool quitting = false;
uint selectedStateSlot = 0;
namespace {
HINSTANCE instance{};
HFONT uiFont{};
HMENU menuBar{}, fileMenu{}, recentMenu{}, outputMenu{}, settingsMenu{}, toolsMenu{}, stateSaveMenu{}, stateLoadMenu{}, portMenu[3]{};
HACCEL accelerators{};
HMENU commandMenus[1024]{};
int systemLoopDepth = 0, optionsPage = 0;
constexpr UINT RefreshVideoMessage = WM_APP + 1;
bool videoRefreshPending = false;
bool layingOut=false;
bool layoutPending=false,refreshRequested=false,dragGeometryPending=false;
RECT pendingDragRectangle{};
UINT dragEdge=0;
POINT dragOrigin{};
RECT dragRectangle{};
std::array<MSG,64> deferredMessages{};
uint deferredMessageCount=0;
constexpr UINT ApplyOptionsTabMessage=WM_APP+3;
void deferMessage(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
  // Mouse movement is replaceable; user commands retain their ordering.
  if(message==WM_MOUSEMOVE && deferredMessageCount
    && deferredMessages[deferredMessageCount-1].message==WM_MOUSEMOVE
    && deferredMessages[deferredMessageCount-1].hwnd==window) {
    deferredMessages[deferredMessageCount-1].lParam=lparam;return;
  }
  if(deferredMessageCount<deferredMessages.size()) {
    auto& pending=deferredMessages[deferredMessageCount++];
    pending.hwnd=window;pending.message=message;pending.wParam=wparam;pending.lParam=lparam;
  }
}
enum : UINT { CommandOpen=100, CommandUnload, CommandReset, CommandPower, CommandQuit,
  CommandVideo=120, CommandAudio, CommandInput, CommandPaths, CommandEmulator, CommandDrivers,
  CommandFullscreen=140, CommandHD, CommandBlur, CommandAspect, CommandOverscan, CommandStatus, CommandMute, CommandTrace,
  CommandPause=160, CommandFrame, CommandScreenshot, CommandSaveUndo, CommandLoadUndo, CommandLoadRedo,
  CommandStates=180, CommandCheats, CommandCheatFind, CommandManifest, CommandAbout, CommandSameBoy,
  CommandTiming,CommandTimingReport,
  CommandRecent=200, CommandSaveSlot=220, CommandLoadSlot=240, CommandSize=260, CommandOutput=280, CommandSpeed=290,
  CommandPort=400 };
enum : int { OptionsTab=1000, OptionsApply, OptionsClose,
  OptLuminance=1020, OptSaturation, OptGamma, OptHD, OptBlur, OptAspect, OptOverscan, OptDimming, OptStatus, OptOutput,
  OptPresent=1040, OptImages, OptMonitor, OptFlush, OptPacing, OptTimer, OptPower,
  OptAudioDriver=1060, OptAudioDevice, OptAudioFrequency, OptAudioLatency, OptAudioSync, OptAudioDynamic, OptAudioExclusive,
  OptVolume=1080, OptBalance, OptSkew, OptMute, OptMuteFocus,
  OptMapping=1100, OptDefocus, OptTrace,
  OptSaveMemory=1120, OptSaveInterval, OptSaveState, OptLoadState, OptWarn, OptFFLimit, OptFFMute, OptScreenSaver,
  OptPath=1200, OptBrowse=1220, OptReset=1240 };
enum : int { ToolText=2000, ToolList, ToolName, ToolAction, ToolSecond, ToolThird, ToolValue, ToolRestart };
const wchar_t* pageNames[] = {L"Video",L"Audio",L"Input",L"Paths",L"Emulator",L"Drivers"};
std::vector<HWND> toolWindows;

std::wstring wide(const string& value) { utf16_t text(value); return std::wstring(text); }

// Track placement in memory only. Settings are persisted when paused/closed,
// never by a timer, background writer, or disk lookup during gameplay.
bool windowGeometryReady=false;
void recordWindowGeometry() {
  if(!windowGeometryReady || !mainWindow || IsIconic(mainWindow) || video.fullScreen())return;
  auto& window=settings.general.window;
  bool maximized=IsZoomed(mainWindow)!=FALSE;
  if(!maximized) {
    RECT rectangle{};if(!GetWindowRect(mainWindow,&rectangle))return;
    window.x=rectangle.left;window.y=rectangle.top;
    window.width=rectangle.right-rectangle.left;window.height=rectangle.bottom-rectangle.top;
  }
  window.maximized=maximized;
}

string text(HWND control) {
  int length = GetWindowTextLengthW(control);
  std::wstring value(length + 1, L'\0');
  GetWindowTextW(control, value.data(), length + 1);
  return (const char*)utf8_t(value.c_str());
}
void put(HWND control, const string& value) { auto w = wide(value); SetWindowTextW(control, w.c_str()); }
int dpi(HWND window) { HDC dc=GetDC(window); int value=GetDeviceCaps(dc,LOGPIXELSX); ReleaseDC(window,dc); return value; }
int px(HWND window, int value) { return MulDiv(value,dpi(window),96); }
HWND control(HWND parent, const wchar_t* kind, const wchar_t* label, DWORD style, int id, int x, int y, int w, int h) {
  const int scale=dpi(parent);
  HWND child=CreateWindowExW((!wcscmp(kind,L"EDIT") || !wcscmp(kind,L"LISTBOX"))?WS_EX_CLIENTEDGE:0,
    kind,label,WS_CHILD|WS_VISIBLE|style,MulDiv(x,scale,96),MulDiv(y,scale,96),MulDiv(w,scale,96),MulDiv(h,scale,96),parent,(HMENU)(INT_PTR)id,instance,nullptr);
  SendMessageW(child,WM_SETFONT,(WPARAM)uiFont,FALSE);
  return child;
}
void label(HWND parent,const wchar_t* title,int x,int y,int w=200) { control(parent,L"STATIC",title,SS_LEFT,0,x,y,w,22); }
void check(HWND parent,const wchar_t* title,int id,bool value,int x,int y,int w=340) {
  HWND item=control(parent,L"BUTTON",title,BS_AUTOCHECKBOX|WS_TABSTOP,id,x,y,w,24);
  SendMessageW(item,BM_SETCHECK,value?BST_CHECKED:BST_UNCHECKED,0);
}
bool checked(HWND parent,int id) { return IsDlgButtonChecked(parent,id)==BST_CHECKED; }
void number(HWND parent,const wchar_t* title,int id,int value,int y,int low=-99999,int high=99999) {
  label(parent,title,28,y,230); HWND edit=control(parent,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,id,270,y-2,120,26);
  put(edit,string{value});
  HWND spin=control(parent,UPDOWN_CLASSW,L"",UDS_ALIGNRIGHT|UDS_ARROWKEYS|UDS_SETBUDDYINT,0,390,y-2,20,26);
  SendMessageW(spin,UDM_SETBUDDY,(WPARAM)edit,0); SendMessageW(spin,UDM_SETRANGE32,low,high);
}
int integer(HWND parent,int id,int low,int high) {
  auto value=text(GetDlgItem(parent,id)); return std::clamp((int)strtol(value,nullptr,10),low,high);
}
void combo(HWND parent,const wchar_t* title,int id,const vector<string>& values,const string& selected,int y) {
  label(parent,title,28,y,230);
  HWND box=control(parent,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,id,270,y-2,390,300);
  int index=0, selection=0;
  for(auto& value:values) { auto w=wide(value); SendMessageW(box,CB_ADDSTRING,0,(LPARAM)w.c_str()); if(value==selected)selection=index; index++; }
  SendMessageW(box,CB_SETCURSEL,selection,0);
}
string choice(HWND parent,int id) { return text(GetDlgItem(parent,id)); }
string* pathSetting(uint index) {
  string* paths[]={&settings.path.games,&settings.path.patches,&settings.path.saves,&settings.path.cheats,&settings.path.states,&settings.path.screenshots};
  return index<6?paths[index]:nullptr;
}
void append(HMENU menu,UINT id,const wchar_t* title) { AppendMenuW(menu,MF_STRING,id,title);if(id<1024)commandMenus[id]=menu; }
void submenu(HMENU menu,HMENU child,const wchar_t* title) { AppendMenuW(menu,MF_POPUP,(UINT_PTR)child,title); }
void mark(HMENU menu,UINT id,bool value) { CheckMenuItem(id<1024&&commandMenus[id]?commandMenus[id]:menu,id,MF_BYCOMMAND|(value?MF_CHECKED:MF_UNCHECKED)); }
void enabled(HMENU menu,UINT id,bool value) { EnableMenuItem(id<1024&&commandMenus[id]?commandMenus[id]:menu,id,MF_BYCOMMAND|(value?MF_ENABLED:MF_GRAYED)); }
void separator(HMENU menu) { AppendMenuW(menu,MF_SEPARATOR,0,nullptr); }
void clearMenu(HMENU menu) { while(GetMenuItemCount(menu)>0)DeleteMenu(menu,0,MF_BYPOSITION); }
int statusBarHeight() {
  if(!settings.general.statusBar || !statusWindow) return 0;
  RECT rectangle{};
  GetWindowRect(statusWindow,&rectangle);
  return rectangle.bottom-rectangle.top;
}
RECT windowFrame(HWND window,int videoWidth,int videoHeight) {
  RECT rectangle{0,0,videoWidth,videoHeight+statusBarHeight()};
  UINT scale=GetDpiForWindow(window);
  if(!scale)scale=96;
  const bool hasMenu=GetMenu(window)!=nullptr;
  AdjustWindowRectExForDpi(&rectangle,(DWORD)GetWindowLongPtrW(window,GWL_STYLE),
    hasMenu,(DWORD)GetWindowLongPtrW(window,GWL_EXSTYLE),scale);
  return rectangle;
}
SIZE minimumWindowSize(HWND window) {
  RECT outer{},client{};
  if(GetWindowRect(window,&outer) && GetClientRect(window,&client) && client.right>0 && client.bottom>0)
    return {256+outer.right-outer.left-client.right,
      (settings.video.overscan?240:224)+statusBarHeight()+outer.bottom-outer.top-client.bottom};
  RECT frame=windowFrame(window,256,settings.video.overscan?240:224);
  return {frame.right-frame.left,frame.bottom-frame.top};
}
void constrainResize(HWND window,RECT& rectangle,UINT edge,const RECT& anchor) {
  bool left=edge==HTLEFT||edge==HTTOPLEFT||edge==HTBOTTOMLEFT;
  bool right=edge==HTRIGHT||edge==HTTOPRIGHT||edge==HTBOTTOMRIGHT;
  bool top=edge==HTTOP||edge==HTTOPLEFT||edge==HTTOPRIGHT;
  bool bottom=edge==HTBOTTOM||edge==HTBOTTOMLEFT||edge==HTBOTTOMRIGHT;
  auto minimum=minimumWindowSize(window);
  int nativeHeight=settings.video.overscan?240:224;
  int chromeWidth=minimum.cx-256,chromeHeight=minimum.cy-nativeHeight;
  int width=rectangle.right-rectangle.left-chromeWidth,height=rectangle.bottom-rectangle.top-chromeHeight;
  bool heightDriven=!(left||right);
  if((left||right)&&(top||bottom)) {
    int64_t dx=int64_t(rectangle.right)-rectangle.left-(anchor.right-anchor.left);
    int64_t dy=int64_t(rectangle.bottom)-rectangle.top-(anchor.bottom-anchor.top);
    if(dx<0)dx=-dx;if(dy<0)dy=-dy;
    heightDriven=dy*256>dx*nativeHeight;
  }
  auto size=fastLinkResizeVideo(width,height,heightDriven,settings.video.overscan);
  int outerWidth=size.width+chromeWidth,outerHeight=size.height+chromeHeight;
  if(left){rectangle.right=anchor.right;rectangle.left=rectangle.right-outerWidth;}
  else if(right){rectangle.left=anchor.left;rectangle.right=rectangle.left+outerWidth;}
  else {rectangle.left=anchor.left+((anchor.right-anchor.left)-outerWidth)/2;rectangle.right=rectangle.left+outerWidth;}
  if(top){rectangle.bottom=anchor.bottom;rectangle.top=rectangle.bottom-outerHeight;}
  else if(bottom){rectangle.top=anchor.top;rectangle.bottom=rectangle.top+outerHeight;}
  else {rectangle.top=anchor.top+((anchor.bottom-anchor.top)-outerHeight)/2;rectangle.bottom=rectangle.top+outerHeight;}
}
void dragResize(HWND window) {
  POINT cursor{};if(!GetCursorPos(&cursor))return;
  RECT rectangle=dragRectangle;
  LONG dx=cursor.x-dragOrigin.x,dy=cursor.y-dragOrigin.y;
  if(dragEdge==HTLEFT||dragEdge==HTTOPLEFT||dragEdge==HTBOTTOMLEFT)rectangle.left+=dx;
  if(dragEdge==HTRIGHT||dragEdge==HTTOPRIGHT||dragEdge==HTBOTTOMRIGHT)rectangle.right+=dx;
  if(dragEdge==HTTOP||dragEdge==HTTOPLEFT||dragEdge==HTTOPRIGHT)rectangle.top+=dy;
  if(dragEdge==HTBOTTOM||dragEdge==HTBOTTOMLEFT||dragEdge==HTBOTTOMRIGHT)rectangle.bottom+=dy;
  constrainResize(window,rectangle,dragEdge,dragRectangle);
  RECT current{};GetWindowRect(window,&current);
  if(!EqualRect(&current,&rectangle)){pendingDragRectangle=rectangle;dragGeometryPending=true;}
}
void updateSizeChecks() {
  if(!menuBar || !videoWindow)return;
  RECT rectangle{};GetClientRect(videoWindow,&rectangle);
  int height=settings.video.overscan?240:224;
  for(uint i=0;i<6;i++)mark(menuBar,CommandSize+i,!video.fullScreen()
    && rectangle.right==256*(i+1) && rectangle.bottom==height*(i+1));
}
void queueVideoRefresh() {
  if(!videoRefreshPending && mainWindow)
    videoRefreshPending=PostMessageW(mainWindow,RefreshVideoMessage,0,0)!=FALSE;
}
void resizeClient(int width,int height) {
  if(IsZoomed(mainWindow) || IsIconic(mainWindow))ShowWindow(mainWindow,SW_RESTORE);
  RECT r=windowFrame(mainWindow,width,height);
  SetWindowPos(mainWindow,nullptr,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
  layoutPending=false;nativeLayout();
  // Account for menu wrapping and the status control's actual themed height.
  // Dimensions here are physical video pixels, never DPI-scaled UI units.
  for(int attempt=0;attempt<3;attempt++) {
    RECT client{},outer{};GetClientRect(videoWindow,&client);
    int dx=width-client.right,dy=height-client.bottom;
    if(!dx && !dy)break;
    GetWindowRect(mainWindow,&outer);
    SetWindowPos(mainWindow,nullptr,0,0,outer.right-outer.left+dx,outer.bottom-outer.top+dy,
      SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    layoutPending=false;nativeLayout();
  }
}
void about(bool gameBoy) {
  auto message=gameBoy?string{"SameBoy 1.0.2\nSuper Game Boy emulation by Lior Halphon.\nExpat license."}
    :string{Emulator::Name," v",Emulator::Version,"\nSuper Nintendo emulator\n",Emulator::Copyright,"\n",Emulator::License};
  auto w=wide(message); modalDepth++;program.resetFramePacer();program.timing.idle(); MessageBoxW(mainWindow,w.c_str(),L"About",MB_OK|MB_ICONINFORMATION); modalDepth--;program.resetFramePacer();program.timing.idle();
}
void command(UINT id) {
  if(program.inFrame) return;
  if(id>=CommandRecent && id<CommandRecent+9) {
    auto entry=settings[string{"Game/Recent/",id-CommandRecent+1}].text();
    if(entry) { program.gameQueue={}; for(auto game:entry.split("|"))program.gameQueue.append(game); program.load(); } return;
  }
  if(id>=CommandSaveSlot && id<CommandSaveSlot+9) { selectedStateSlot=id-CommandSaveSlot; program.saveState({"Quick/Slot ",selectedStateSlot+1}); return; }
  if(id>=CommandLoadSlot && id<CommandLoadSlot+9) { selectedStateSlot=id-CommandLoadSlot; program.loadState({"Quick/Slot ",selectedStateSlot+1}); return; }
  if(id>=CommandSize && id<CommandSize+6) {
    settings.video.multiplier=id-CommandSize+1;
    if(video.fullScreen())program.toggleVideoFullScreen();
    resizeClient(256*settings.video.multiplier,(settings.video.overscan?240:224)*settings.video.multiplier);
    updateSizeChecks();settings.save();return;
  }
  if(id>=CommandPort && id<CommandPort+96) { uint port=(id-CommandPort)/32, device=(id-CommandPort)%32; emulator->connect(port,device); nativeMenus();return; }
  if(id>=CommandOutput && id<CommandOutput+3) { const char* values[]={"Center","Scale","Stretch"};settings.video.output=values[id-CommandOutput];nativeMenus(); program.refresh();return; }
  if(id>=CommandSpeed && id<CommandSpeed+5) { const double speeds[]={.5,.75,1,1.5,2}; program.speed=speeds[id-CommandSpeed];program.updateAudioFrequency();audio.clear();program.resetFramePacer();nativeMenus();return; }
  switch(id) {
  case CommandOpen:program.load();break;
  case CommandUnload:program.unload();break;
  case CommandReset:program.reset();break;
  case CommandPower:program.power();break;
  case CommandQuit:program.quit();break;
  case CommandVideo:nativeOptions(0);break;case CommandAudio:nativeOptions(1);break;
  case CommandInput:nativeOptions(2);break;case CommandPaths:nativeOptions(3);break;
  case CommandEmulator:nativeOptions(4);break;case CommandDrivers:nativeOptions(5);break;
  case CommandFullscreen:program.toggleVideoFullScreen();break;
  case CommandHD:settings.video.hdMode7=!settings.video.hdMode7;emulator->configure("Video/HDMode7",settings.video.hdMode7);break;
  case CommandBlur:settings.video.blur=!settings.video.blur;emulator->configure("Video/BlurEmulation",settings.video.blur);break;
  case CommandAspect:settings.video.aspectCorrection=!settings.video.aspectCorrection;break;
  case CommandOverscan:settings.video.overscan=!settings.video.overscan;break;
  case CommandStatus:settings.general.statusBar=!settings.general.statusBar;nativeLayout();break;
  case CommandMute:settings.audio.mute=!settings.audio.mute;break;
  case CommandTrace:settings.fastLink.ramTraceServer=!settings.fastLink.ramTraceServer;if(settings.fastLink.ramTraceServer)fastLinkRAMTraceService.start();else fastLinkRAMTraceService.stop();break;
  case CommandPause:program.paused=program.frameAdvance?false:!program.paused;program.frameAdvance=false;audio.clear();program.resetFramePacer();if(program.paused&&settings.emulator.autoSaveMemory.enable)program.save();program.refresh();break;
  case CommandFrame:program.paused=false;program.frameAdvance=true;program.frameAdvanceLock=false;break;
  case CommandScreenshot:program.captureScreenshot();break;
  case CommandSaveUndo:program.saveState({"Quick/Slot ",selectedStateSlot+1});break;
  case CommandLoadUndo:program.loadState("Quick/Undo");break;case CommandLoadRedo:program.loadState("Quick/Redo");break;
  case CommandStates:nativeTool(0);break;case CommandCheats:nativeTool(1);break;
  case CommandCheatFind:nativeTool(2);break;case CommandManifest:nativeTool(3);break;
  case CommandAbout:about(false);return;case CommandSameBoy:about(true);return;
  case CommandTiming:
    if(program.timing.captureInMemory)program.timing.captureInMemory=false;
    else program.timing.restartCapture();
    video.captureTiming(program.timing.enabled());
    break;
  case CommandTimingReport:{
    auto report=wide(program.timing.report().c_str());
    modalDepth++;program.resetFramePacer();program.timing.idle();
    MessageBoxW(mainWindow,report.c_str(),L"Timing snapshot (RAM only)",MB_OK|MB_ICONINFORMATION);
    modalDepth--;program.resetFramePacer();program.timing.idle();return;
  }
  default:return;
  }
  nativeMenus();settings.save();
}
void buildMenus() {
  menuBar=CreateMenu();fileMenu=CreatePopupMenu();recentMenu=CreatePopupMenu();settingsMenu=CreatePopupMenu();outputMenu=CreatePopupMenu();toolsMenu=CreatePopupMenu();
  submenu(menuBar,fileMenu,L"&File");submenu(menuBar,settingsMenu,L"&Settings");submenu(menuBar,toolsMenu,L"&Tools");
  append(fileMenu,CommandOpen,L"&Load Game...\tCtrl+O");submenu(fileMenu,recentMenu,L"Load &Recent Game");
  append(fileMenu,CommandReset,L"&Reset SNES\tF9");append(fileMenu,CommandPower,L"Power Cycle");append(fileMenu,CommandUnload,L"Unload Game");separator(fileMenu);
  for(uint port=0;port<3;port++) { portMenu[port]=CreatePopupMenu();auto title=wide(port<2?string{"Controller Port ",port+1}:string{"Expansion Port"});submenu(fileMenu,portMenu[port],title.c_str());for(auto device:emulator->devices(port)){auto w=wide(device.name);append(portMenu[port],CommandPort+port*32+device.id,w.c_str());} }
  separator(fileMenu);append(fileMenu,CommandQuit,L"&Quit\tCtrl+Q");
  HMENU sizes=CreatePopupMenu();submenu(settingsMenu,sizes,L"Window Size");for(uint i=0;i<6;i++){auto w=wide(string{i+1,"x"});append(sizes,CommandSize+i,w.c_str());}
  append(settingsMenu,CommandFullscreen,L"Fullscreen\tAlt+Enter");submenu(settingsMenu,outputMenu,L"Output");
  append(outputMenu,CommandOutput,L"Fullscreen: Center");append(outputMenu,CommandOutput+1,L"Fullscreen: Scale");append(outputMenu,CommandOutput+2,L"Fullscreen: Stretch");separator(outputMenu);
  append(outputMenu,CommandHD,L"HD Mode 7 (2x)");append(outputMenu,CommandBlur,L"Hires Blur Emulation");append(outputMenu,CommandAspect,L"Fullscreen Aspect Correction");append(outputMenu,CommandOverscan,L"Show Overscan Area");
  separator(settingsMenu);append(settingsMenu,CommandMute,L"Mute Audio");append(settingsMenu,CommandStatus,L"Show Status Bar");append(settingsMenu,CommandTrace,L"Enable RAM Trace REST API");separator(settingsMenu);
  append(settingsMenu,CommandVideo,L"Video...");append(settingsMenu,CommandAudio,L"Audio...");append(settingsMenu,CommandInput,L"Input...");append(settingsMenu,CommandPaths,L"Paths...");append(settingsMenu,CommandEmulator,L"Emulator...");append(settingsMenu,CommandDrivers,L"Drivers...");
  stateSaveMenu=CreatePopupMenu();stateLoadMenu=CreatePopupMenu();submenu(toolsMenu,stateSaveMenu,L"Save State");submenu(toolsMenu,stateLoadMenu,L"Load State");
  for(uint i=0;i<9;i++){auto name=wide(string{"Slot ",i+1});append(stateSaveMenu,CommandSaveSlot+i,name.c_str());append(stateLoadMenu,CommandLoadSlot+i,name.c_str());}
  separator(stateLoadMenu);append(stateLoadMenu,CommandLoadUndo,L"Undo Last Save / Load");append(stateLoadMenu,CommandLoadRedo,L"Redo Last Undo");
  HMENU speeds=CreatePopupMenu();submenu(toolsMenu,speeds,L"Speed");const wchar_t* speedNames[]={L"50%",L"75%",L"100%",L"150%",L"200%"};for(uint i=0;i<5;i++)append(speeds,CommandSpeed+i,speedNames[i]);
  append(toolsMenu,CommandPause,L"Pause\tPause");append(toolsMenu,CommandFrame,L"Frame Advance\tF6");append(toolsMenu,CommandScreenshot,L"Capture Screenshot\tF12");separator(toolsMenu);
  append(toolsMenu,CommandStates,L"State Manager...");append(toolsMenu,CommandCheats,L"Cheat Editor...");append(toolsMenu,CommandCheatFind,L"Cheat Finder...");append(toolsMenu,CommandManifest,L"Manifest Viewer...");
  separator(toolsMenu);append(toolsMenu,CommandTiming,L"Capture Timing in RAM");append(toolsMenu,CommandTimingReport,L"Show Timing Snapshot...");
  HMENU help=CreatePopupMenu();submenu(menuBar,help,L"&Help");append(help,CommandAbout,L"About bsnes-fastlink...");append(help,CommandSameBoy,L"About SameBoy...");
  SetMenu(mainWindow,menuBar);
}
}

auto nativeError(const string& message) -> void { auto w=wide(message);modalDepth++;program.resetFramePacer();program.timing.idle();MessageBoxW(optionsWindow?optionsWindow:mainWindow,w.c_str(),L"bsnes-fastlink",MB_OK|MB_ICONERROR);modalDepth--;program.resetFramePacer();program.timing.idle(); }
auto nativeConfirm(const string& message,const string& title) -> bool {auto w=wide(message),t=wide(title);modalDepth++;program.resetFramePacer();program.timing.idle();int answer=MessageBoxW(mainWindow,w.c_str(),t.c_str(),MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION);modalDepth--;program.resetFramePacer();program.timing.idle();return answer==IDYES;}
auto nativeStatus(const string& message) -> void { if(statusWindow){auto w=wide(message);SendMessageW(statusWindow,SB_SETTEXTW,0,(LPARAM)w.c_str());} }
auto nativeTitle() -> void { string title={Emulator::Name," v",Emulator::Version};if(emulator&&emulator->loaded())title={emulator->title()," - ",title};put(mainWindow,title); }
auto nativeMenus() -> void {
  if(!menuBar) return;bool loaded=emulator&&emulator->loaded();
  for(UINT id:{CommandUnload,CommandReset,CommandPower,CommandPause,CommandFrame,CommandScreenshot,CommandStates,CommandCheats,CommandCheatFind,CommandManifest})enabled(menuBar,id,loaded);
  mark(menuBar,CommandFullscreen,video.fullScreen());mark(menuBar,CommandHD,settings.video.hdMode7);mark(menuBar,CommandBlur,settings.video.blur);mark(menuBar,CommandAspect,settings.video.aspectCorrection);mark(menuBar,CommandOverscan,settings.video.overscan);mark(menuBar,CommandStatus,settings.general.statusBar);mark(menuBar,CommandMute,settings.audio.mute);mark(menuBar,CommandTrace,settings.fastLink.ramTraceServer);mark(menuBar,CommandPause,program.paused);
  mark(menuBar,CommandTiming,program.timing.captureInMemory);
  for(uint i=0;i<3;i++){const char* outputs[]={"Center","Scale","Stretch"};mark(outputMenu,CommandOutput+i,settings.video.output==outputs[i]);}
  // A load command checks its requested state on demand. Menu refreshes must
  // not repeatedly probe files or reopen the state ZIP while gameplay runs.
  for(uint i=0;i<9;i++){enabled(stateSaveMenu,CommandSaveSlot+i,loaded);enabled(stateLoadMenu,CommandLoadSlot+i,loaded);}
  const double speeds[]={.5,.75,1,1.5,2};for(uint i=0;i<5;i++)mark(menuBar,CommandSpeed+i,program.speed==speeds[i]);
  clearMenu(recentMenu);for(uint i=0;i<9;i++){auto entry=settings[string{"Game/Recent/",i+1}].text();if(!entry)continue;auto parts=entry.split(";",1L);auto w=wide(Location::file(parts(1)));append(recentMenu,CommandRecent+i,w.c_str());}
  for(uint port=0;port<3;port++)for(auto device:emulator->devices(port))mark(portMenu[port],CommandPort+port*32+device.id,emulator->connected(port)==device.id);
  updateSizeChecks();
  DrawMenuBar(mainWindow);
}
auto nativeLayout() -> void {
  if(!mainWindow || layingOut)return;
  layingOut=true;
  struct LayoutGuard { ~LayoutGuard() { layingOut=false; } } guard;
  RECT r{};GetClientRect(mainWindow,&r);int height=0;
  if(statusWindow){if((IsWindowVisible(statusWindow)!=FALSE)!=settings.general.statusBar)ShowWindow(statusWindow,settings.general.statusBar?SW_SHOW:SW_HIDE);if(settings.general.statusBar){SendMessageW(statusWindow,WM_SIZE,0,0);RECT s{};GetWindowRect(statusWindow,&s);height=s.bottom-s.top;}}
  const int minimumHeight=settings.video.overscan?240:224;
  if(!IsIconic(mainWindow) && (r.right<256 || r.bottom-height<minimumHeight)) {
    RECT outer{};GetWindowRect(mainWindow,&outer);
    SetWindowPos(mainWindow,nullptr,0,0,outer.right-outer.left+max(0L,256-r.right),
      outer.bottom-outer.top+max(0L,minimumHeight+height-r.bottom),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    GetClientRect(mainWindow,&r);
  }
  MoveWindow(videoWindow,0,0,r.right,max(0L,r.bottom-height),TRUE);
  video.poll();updateSizeChecks();queueVideoRefresh();
}
auto nativeFile(bool save,const string& title,const string& initial) -> string {
  wchar_t filename[32768]{};auto t=wide(title),directory=wide(initial);OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=optionsWindow?optionsWindow:mainWindow;
  dialog.lpstrTitle=t.c_str();dialog.lpstrFile=filename;dialog.nMaxFile=32768;dialog.lpstrInitialDir=directory.empty()?nullptr:directory.c_str();
  dialog.lpstrFilter=L"Supported files\0*.sfc;*.smc;*.gb;*.gbc;*.bs;*.st;*.zip;*.7z;*.rom\0All files\0*.*\0\0";
  dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
  modalDepth++;program.resetFramePacer();program.timing.idle();BOOL success=save?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog);modalDepth--;program.resetFramePacer();program.timing.idle();
  return success?string{(const char*)utf8_t(filename)}.transform("\\","/"):string{};
}
auto nativeFolder(HWND owner,const string& initial) -> string {
  auto start=wide(initial);BROWSEINFOW browse{};browse.hwndOwner=owner;browse.lpszTitle=L"Select folder";browse.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
  browse.lParam=(LPARAM)start.c_str();browse.lpfn=[](HWND window,UINT message,LPARAM,LPARAM data)->int{if(message==BFFM_INITIALIZED)SendMessageW(window,BFFM_SETSELECTIONW,TRUE,data);return 0;};
  modalDepth++;program.resetFramePacer();program.timing.idle();auto item=SHBrowseForFolderW(&browse);modalDepth--;program.resetFramePacer();program.timing.idle();if(!item)return{};
  wchar_t folder[32768]{};BOOL success=SHGetPathFromIDListEx(item,folder,32768,GPFIDL_DEFAULT);CoTaskMemFree(item);
  if(!success)return{};return string{(const char*)utf8_t(folder)}.transform("\\","/").append("/");
}

namespace {
void optionsControls() {
  HWND window=optionsWindow;
  EnumChildWindows(window,[](HWND child,LPARAM parent)->BOOL{
    if(GetParent(child)==(HWND)parent){int id=GetDlgCtrlID(child);if(id!=OptionsTab&&id!=OptionsApply&&id!=OptionsClose)DestroyWindow(child);}return TRUE;
  },(LPARAM)window);
  switch(optionsPage) {
  case 0:
    number(window,L"Luminance (%)",OptLuminance,settings.video.luminance,66,0,100);
    number(window,L"Saturation (%)",OptSaturation,settings.video.saturation,106,0,200);
    number(window,L"Gamma (%)",OptGamma,settings.video.gamma,146,100,200);
    check(window,L"HD Mode 7 (2x)",OptHD,settings.video.hdMode7,28,192);
    check(window,L"Hires blur emulation",OptBlur,settings.video.blur,28,226);
    check(window,L"Fullscreen aspect correction",OptAspect,settings.video.aspectCorrection,28,260);
    check(window,L"Show overscan area",OptOverscan,settings.video.overscan,28,294);
    check(window,L"Dim paused video",OptDimming,settings.video.dimming,28,328);
    check(window,L"Show status bar",OptStatus,settings.general.statusBar,28,362);
    combo(window,L"Fullscreen output",OptOutput,{"Center","Scale","Stretch"},settings.video.output,410);
    break;
  case 1: {
    combo(window,L"Audio driver",OptAudioDriver,ruby::Audio::hasDrivers(),settings.audio.driver,66);
    combo(window,L"Output device",OptAudioDevice,audio.hasDevices(),settings.audio.device,106);
    vector<string> frequencies,latencies;for(auto value:audio.hasFrequencies())frequencies.append(string{value});for(auto value:audio.hasLatencies())latencies.append(string{value});
    combo(window,L"Frequency (Hz)",OptAudioFrequency,frequencies,string{settings.audio.frequency},146);
    combo(window,L"Latency (ms)",OptAudioLatency,latencies,string{settings.audio.latency},186);
    number(window,L"Volume (%)",OptVolume,settings.audio.volume,230,0,200);
    number(window,L"Balance (0 left, 50 center)",OptBalance,settings.audio.balance,270,0,100);
    number(window,L"Frequency skew (Hz)",OptSkew,settings.audio.skew,310,-5000,5000);
    check(window,L"Mute",OptMute,settings.audio.mute,28,352);
    check(window,L"Mute when unfocused",OptMuteFocus,settings.audio.muteUnfocused,28,384);
    check(window,L"Audio output runs independently",OptAudioSync,true,380,352,290);
    check(window,L"Dynamic rate control",OptAudioDynamic,settings.audio.dynamic,380,384,290);
    check(window,L"Exclusive audio",OptAudioExclusive,settings.audio.exclusive,380,416,290);
    EnableWindow(GetDlgItem(window,OptAudioSync),FALSE);EnableWindow(GetDlgItem(window,OptAudioDynamic),audio.hasDynamic());EnableWindow(GetDlgItem(window,OptAudioExclusive),audio.hasExclusive());
    break;
  }
  case 2:
    label(window,L"Win32 Raw Input — dedicated event-driven input thread",28,66,660);
    combo(window,L"Unknown HID button order",OptMapping,{"SNES Adapter","Standard HID"},settings.fastLink.genericHIDMapping,112);
    combo(window,L"When unfocused",OptDefocus,{"Block","Allow","Pause"},settings.input.defocus,158);
    check(window,L"RAM Trace REST API (localhost:9000)",OptTrace,settings.fastLink.ramTraceServer,28,210,650);
    label(window,L"Keyboard shortcuts",28,270,600);
    label(window,L"Ctrl+O: load    Ctrl+Q: quit    Alt+Enter / F11: fullscreen",28,310,660);
    label(window,L"F5: save selected slot    F8: load selected slot    F9: reset",28,350,660);
    label(window,L"Pause: pause    F6: frame advance    F12: screenshot    Tab: fast forward",28,390,660);
    break;
  case 3: {
    const wchar_t* titles[]={L"Games",L"Patches",L"Saves",L"Cheats",L"States",L"Screenshots"};
    for(uint i=0;i<6;i++){
      int y=66+i*62;label(window,titles[i],28,y,150);
      auto edit=control(window,L"EDIT",L"",ES_READONLY|ES_AUTOHSCROLL,OptPath+i,150,y-3,340,26);put(edit,*pathSetting(i));
      control(window,L"BUTTON",L"Browse...",BS_PUSHBUTTON|WS_TABSTOP,OptBrowse+i,500,y-3,90,28);
      control(window,L"BUTTON",L"Reset",BS_PUSHBUTTON|WS_TABSTOP,OptReset+i,600,y-3,70,28);
    }
    label(window,L"An empty path uses the existing game-folder/default location.",28,454,660);break;
  }
  case 4:
    check(window,L"Save cartridge memory when paused",OptSaveMemory,settings.emulator.autoSaveMemory.enable,28,66,620);
    label(window,L"Cartridge memory is also saved when unloading or closing.",28,110,660);
    check(window,L"Save undo state when unloading",OptSaveState,settings.emulator.autoSaveStateOnUnload,28,154,620);
    check(window,L"Load undo state when loading",OptLoadState,settings.emulator.autoLoadStateOnLoad,28,192,620);
    check(window,L"Warn about unverified ROMs",OptWarn,settings.emulator.warnOnUnverifiedGames,28,230,620);
    check(window,L"Allow screensaver during emulation",OptScreenSaver,settings.general.screenSaver,28,268,620);
    combo(window,L"Fast-forward limit",OptFFLimit,{"0","2","3","4","5","6","7","8"},string{settings.fastForward.limiter},318);
    check(window,L"Mute while fast forwarding",OptFFMute,settings.fastForward.mute,28,364,620);
    break;
  case 5: {
    label(window,L"Native Vulkan — direct C API",28,66,660);
    combo(window,L"Present mode",OptPresent,{"Immediate","Mailbox","FIFO"},settings.video.presentMode,110);
    combo(window,L"Swapchain images",OptImages,{"2","3","4"},string{settings.video.swapchainImages},154);
    vector<string> monitors;for(auto monitor:video.hasMonitors())monitors.append(monitor.name);
    combo(window,L"Fullscreen monitor",OptMonitor,monitors,settings.video.monitor,198);
    check(window,L"Nonblocking GPU uploads",OptFlush,true,28,246,650);
    EnableWindow(GetDlgItem(window,OptFlush),FALSE);
    check(window,L"Software frame pacing",OptPacing,settings.fastLink.softwareFramePacing,28,284,650);
    check(window,L"High resolution Windows timer (restart required)",OptTimer,settings.fastLink.windowsTimerResolution,28,322,650);
    check(window,L"High performance power scheme (restart required)",OptPower,settings.fastLink.windowsHighPerformancePower,28,360,650);
    label(window,L"Immediate may tear. Mailbox and FIFO synchronize presentation.",28,416,650);
    break;
  }
  }
}
void applyOptions() {
  if(!optionsWindow)return;HWND window=optionsWindow;
  switch(optionsPage){
  case 0:
    settings.video.luminance=integer(window,OptLuminance,0,100);settings.video.saturation=integer(window,OptSaturation,0,200);settings.video.gamma=integer(window,OptGamma,100,200);
    settings.video.hdMode7=checked(window,OptHD);settings.video.blur=checked(window,OptBlur);settings.video.aspectCorrection=checked(window,OptAspect);settings.video.overscan=checked(window,OptOverscan);settings.video.dimming=checked(window,OptDimming);settings.general.statusBar=checked(window,OptStatus);settings.video.output=choice(window,OptOutput);
    emulator->configure("Video/HDMode7",settings.video.hdMode7);emulator->configure("Video/BlurEmulation",settings.video.blur);program.updateVideoPalette();nativeLayout();break;
  case 1: {
    auto driver=choice(window,OptAudioDriver),device=choice(window,OptAudioDevice);
    uint frequency=(uint)integer(window,OptAudioFrequency,8000,384000),latency=(uint)integer(window,OptAudioLatency,0,1000);
    bool blocking=false,dynamic=checked(window,OptAudioDynamic),exclusive=checked(window,OptAudioExclusive);
    bool changed=driver!=settings.audio.driver||device!=settings.audio.device||frequency!=settings.audio.frequency||latency!=settings.audio.latency||exclusive!=settings.audio.exclusive;
    settings.audio.driver=driver;settings.audio.device=device;settings.audio.frequency=frequency;settings.audio.latency=latency;settings.audio.exclusive=exclusive;settings.audio.blocking=blocking;settings.audio.dynamic=dynamic;
    settings.audio.volume=integer(window,OptVolume,0,200);settings.audio.balance=integer(window,OptBalance,0,100);settings.audio.skew=integer(window,OptSkew,-5000,5000);settings.audio.mute=checked(window,OptMute);settings.audio.muteUnfocused=checked(window,OptMuteFocus);
    if(changed)program.updateAudioDriver(mainWindow);else{audio.setBlocking(blocking);audio.setDynamic(dynamic);program.updateAudioFrequency();program.updateAudioEffects();}
    break;
  }
  case 2: {
    auto mapping=choice(window,OptMapping);
    if(mapping!=settings.fastLink.genericHIDMapping){nativeInputManager.terminate();settings.fastLink.genericHIDMapping=mapping;if(!nativeInputManager.initialize())nativeError("Could not restart Raw Input.");}
    settings.input.defocus=choice(window,OptDefocus);
    bool trace=checked(window,OptTrace);if(trace!=settings.fastLink.ramTraceServer){settings.fastLink.ramTraceServer=trace;if(trace)fastLinkRAMTraceService.start();else fastLinkRAMTraceService.stop();}break;
  }
  case 4:
    settings.emulator.autoSaveMemory.enable=checked(window,OptSaveMemory);settings.emulator.autoSaveStateOnUnload=checked(window,OptSaveState);settings.emulator.autoLoadStateOnLoad=checked(window,OptLoadState);settings.emulator.warnOnUnverifiedGames=checked(window,OptWarn);settings.general.screenSaver=checked(window,OptScreenSaver);settings.fastForward.limiter=integer(window,OptFFLimit,0,8);settings.fastForward.mute=checked(window,OptFFMute);break;
  case 5: {
    auto mode=choice(window,OptPresent),monitor=choice(window,OptMonitor);uint images=integer(window,OptImages,2,4);
    settings.video.presentMode=mode;settings.video.swapchainImages=images;settings.video.monitor=monitor;settings.video.flush=false;settings.fastLink.softwareFramePacing=checked(window,OptPacing);settings.fastLink.windowsTimerResolution=checked(window,OptTimer);settings.fastLink.windowsHighPerformancePower=checked(window,OptPower);
    bool ready=video.setMonitor(monitor)&&video.setPresentMode(mode)&&video.setSwapchainImages(images);video.setFlush(settings.video.flush);
    if(!ready)nativeError("Vulkan could not apply the requested display settings.");break;
  }
  }
  settings.save();nativeMenus();
}
LRESULT CALLBACK OptionsProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
  if(program.inFrame) {
    if(message==WM_COMMAND || message==WM_CLOSE){deferMessage(window,message,wparam,lparam);return 0;}
    if(message==WM_NOTIFY && ((NMHDR*)lparam)->idFrom==OptionsTab && ((NMHDR*)lparam)->code==TCN_SELCHANGE) {
      deferMessage(window,ApplyOptionsTabMessage,SendDlgItemMessageW(window,OptionsTab,TCM_GETCURSEL,0,0),0);return 0;
    }
  }
  switch(message){
  case ApplyOptionsTabMessage:applyOptions();optionsPage=(int)wparam;optionsControls();return 0;
  case DM_GETDEFID:return MAKELONG(OptionsClose,DC_HASDEFID);
  case DM_SETDEFID:return TRUE;
  case WM_NOTIFY:if(((NMHDR*)lparam)->idFrom==OptionsTab&&((NMHDR*)lparam)->code==TCN_SELCHANGE){applyOptions();optionsPage=(int)SendDlgItemMessageW(window,OptionsTab,TCM_GETCURSEL,0,0);optionsControls();return 0;}break;
  case WM_COMMAND:{int id=LOWORD(wparam);if(id==OptionsApply&&HIWORD(wparam)==BN_CLICKED){applyOptions();optionsControls();return 0;}if(id==OptionsClose||id==IDCANCEL){SendMessageW(window,WM_CLOSE,0,0);return 0;}
    if(optionsPage==3&&HIWORD(wparam)==BN_CLICKED){if(id>=OptBrowse&&id<OptBrowse+6){auto path=pathSetting(id-OptBrowse);auto selected=nativeFolder(window,*path);if(selected)*path=selected;optionsControls();settings.save();}if(id>=OptReset&&id<OptReset+6){*pathSetting(id-OptReset)="";optionsControls();settings.save();}}break;}
  case WM_CLOSE:applyOptions();DestroyWindow(window);return 0;
  case WM_DESTROY:optionsWindow=nullptr;return 0;
  case WM_DPICHANGED:{auto rect=(RECT*)lparam;SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);optionsControls();return 0;}
  }
  return DefWindowProcW(window,message,wparam,lparam);
}
}

auto nativeOptions(int page) -> void {
  if(!optionsWindow){
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=OptionsProc;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);wc.lpszClassName=L"FastLinkOptions";RegisterClassW(&wc);
    optionsWindow=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"Settings",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,px(mainWindow,730),px(mainWindow,590),mainWindow,nullptr,instance,nullptr);
    HWND tabs=control(optionsWindow,WC_TABCONTROLW,L"",WS_TABSTOP,OptionsTab,12,12,690,490);for(int i=0;i<6;i++){TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=(wchar_t*)pageNames[i];SendMessageW(tabs,TCM_INSERTITEMW,i,(LPARAM)&item);}
    control(optionsWindow,L"BUTTON",L"Apply",BS_PUSHBUTTON|WS_TABSTOP,OptionsApply,500,514,90,30);control(optionsWindow,L"BUTTON",L"Close",BS_DEFPUSHBUTTON|WS_TABSTOP,OptionsClose,610,514,90,30);
  }else applyOptions();
  optionsPage=std::clamp(page,0,5);SendDlgItemMessageW(optionsWindow,OptionsTab,TCM_SETCURSEL,optionsPage,0);optionsControls();ShowWindow(optionsWindow,SW_SHOWNORMAL);SetForegroundWindow(optionsWindow);
}

namespace {
enum : int { ToolCompare=2020, ToolDatabase, ToolRename };
struct ToolData {
  int kind=0;
  vector<Program::State> states;
  std::vector<uint32_t> matches;
  std::array<uint8_t,128*1024> previous{};
  bool searched=false;
};
void stateNames(HWND window,ToolData& data) {
  data.states=program.availableStates("Quick/");for(auto state:program.availableStates("Managed/"))data.states.append(state);SendDlgItemMessageW(window,ToolList,LB_RESETCONTENT,0,0);
  for(auto& state:data.states){auto w=wide(state.name);SendDlgItemMessageW(window,ToolList,LB_ADDSTRING,0,(LPARAM)w.c_str());}
}
string stateName(HWND window) {
  auto name=text(GetDlgItem(window,ToolName));name.strip();
  if(!name||name.find("..")||name.find("\\")||name.find("/")){nativeError("Enter a state name without a path.");return{};}
  return {"Managed/",name};
}
void saveCheatText(HWND window) {
  auto document=text(GetDlgItem(window,ToolText));
  auto parsed=BML::unserialize(document);
  if(!parsed.find("cheat").size()&&document){
    string converted;
    for(auto code:document.split("\n")){code.strip();if(!code||code.beginsWith("#"))continue;auto decoded=code;decoded.downcase();if(!(program.gameBoy?program.decodeGB(decoded):program.decodeSNES(decoded))){nativeError({"Invalid cheat: ",code});return;}converted.append("cheat\n  name: ",code,"\n  code: ",code,"\n  enable\n\n");}
    document=converted;
  }
  if(!file::write(program.cheatPath(),document)){nativeError("Could not save cheats.");return;}
  program.updateCheats();SendDlgItemMessageW(window,ToolText,EM_SETMODIFY,FALSE,0);program.showMessage("Cheats saved");
}
LRESULT CALLBACK ToolProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
  if(program.inFrame && (message==WM_COMMAND || message==WM_CLOSE)) {
    deferMessage(window,message,wparam,lparam);return 0;
  }
  auto data=(ToolData*)GetWindowLongPtrW(window,GWLP_USERDATA);
  if(message==WM_NCCREATE){data=(ToolData*)((CREATESTRUCTW*)lparam)->lpCreateParams;SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)data);return TRUE;}
  switch(message){
  case WM_SIZE:if(data){RECT r{};GetClientRect(window,&r);HWND content=GetDlgItem(window,data->kind==0||data->kind==2?ToolList:ToolText);if(content)MoveWindow(content,px(window,14),px(window,104),max(1L,r.right-px(window,28)),max(1L,r.bottom-px(window,118)),TRUE);}return 0;
  case WM_COMMAND:{if(program.inFrame)return 0;if(!data)break;int id=LOWORD(wparam);if(id==IDCANCEL){SendMessageW(window,WM_CLOSE,0,0);return 0;}
    if(data->kind==0){
      int selected=(int)SendDlgItemMessageW(window,ToolList,LB_GETCURSEL,0,0);
      if(id==ToolAction&&HIWORD(wparam)==BN_CLICKED){auto name=stateName(window);if(name)program.saveState(name);stateNames(window,*data);}
      if((id==ToolSecond&&HIWORD(wparam)==BN_CLICKED)||(id==ToolList&&HIWORD(wparam)==LBN_DBLCLK)){if(selected>=0&&(uint)selected<data->states.size()){program.loadState(data->states[selected].name);audio.clear();program.resetFramePacer();}}
      if(id==ToolRename&&HIWORD(wparam)==BN_CLICKED&&selected>=0&&(uint)selected<data->states.size()){auto name=stateName(window);if(name)program.renameState(data->states[selected].name,name);stateNames(window,*data);nativeMenus();}
      if(id==ToolThird&&HIWORD(wparam)==BN_CLICKED&&selected>=0&&(uint)selected<data->states.size()&&nativeConfirm("Delete this save state?","Delete State")){program.removeState(data->states[selected].name);stateNames(window,*data);}
    }
    if(data->kind==1){if(id==ToolAction&&HIWORD(wparam)==BN_CLICKED)saveCheatText(window);if(id==ToolDatabase&&HIWORD(wparam)==BN_CLICKED)nativeTool(4);}
    if(data->kind==2&&HIWORD(wparam)==BN_CLICKED){
      if(id==ToolRestart){data->searched=false;data->matches.clear();SendDlgItemMessageW(window,ToolList,LB_RESETCONTENT,0,0);return 0;}
      if(id==ToolAction&&emulator->loaded()){
        int mode=(int)SendDlgItemMessageW(window,ToolCompare,CB_GETCURSEL,0,0);int value=integer(window,ToolValue,0,255);
        std::vector<uint32_t> candidates;
        if(data->searched)candidates=data->matches;else{candidates.reserve(data->previous.size());for(uint32_t i=0;i<data->previous.size();i++)candidates.push_back(i);}
        data->matches.clear();SendDlgItemMessageW(window,ToolList,LB_RESETCONTENT,0,0);
        for(auto address:candidates){auto now=SuperFamicom::cpu.wram[address];auto before=data->previous[address];bool found=mode==0?now==value:!data->searched?true:mode==1?now!=before:mode==2?now==before:mode==3?now>before:now<before;if(found){data->matches.push_back(address);if(data->matches.size()<=4096){auto line=wide(string{"$",hex(0x7e0000+address,6L)," = ",now});SendDlgItemMessageW(window,ToolList,LB_ADDSTRING,0,(LPARAM)line.c_str());}}}
        std::memcpy(data->previous.data(),SuperFamicom::cpu.wram,data->previous.size());data->searched=true;auto caption=wide(string{"Cheat Finder — ",data->matches.size()," matches (first 4096 shown)"});SetWindowTextW(window,caption.c_str());
      }
    }
    break;
  }
  case WM_CLOSE:
    if(data&&data->kind==1&&SendDlgItemMessageW(window,ToolText,EM_GETMODIFY,0,0)&&nativeConfirm("Save changes to cheats?","Cheat Editor"))saveCheatText(window);
    DestroyWindow(window);return 0;
  case WM_NCDESTROY:
    toolWindows.erase(std::remove(toolWindows.begin(),toolWindows.end(),window),toolWindows.end());delete data;SetWindowLongPtrW(window,GWLP_USERDATA,0);break;
  }
  return DefWindowProcW(window,message,wparam,lparam);
}
}

auto nativeTool(int tool) -> void {
  if(!emulator->loaded())return;
  WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=ToolProc;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);wc.lpszClassName=L"FastLinkTool";RegisterClassW(&wc);
  const wchar_t* titles[]={L"State Manager",L"Cheat Editor",L"Cheat Finder",L"Manifest Viewer",L"Cheat Database"};
  auto data=new ToolData;data->kind=tool;
  HWND window=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,titles[tool],WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,px(mainWindow,800),px(mainWindow,600),mainWindow,nullptr,instance,data);
  if(!window)return;toolWindows.push_back(window);
  if(tool==0){
    label(window,L"Name:",14,18,80);control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ToolName,95,14,240,26);
    control(window,L"BUTTON",L"Save",BS_PUSHBUTTON|WS_TABSTOP,ToolAction,355,14,100,28);control(window,L"BUTTON",L"Load",BS_PUSHBUTTON|WS_TABSTOP,ToolSecond,465,14,100,28);control(window,L"BUTTON",L"Delete",BS_PUSHBUTTON|WS_TABSTOP,ToolThird,575,14,100,28);
    control(window,L"BUTTON",L"Rename",BS_PUSHBUTTON|WS_TABSTOP,ToolRename,14,58,100,28);
    label(window,L"Double-click to load. Quick states and existing archives are supported.",130,64,600);
    control(window,L"LISTBOX",L"",LBS_NOTIFY|WS_VSCROLL|WS_TABSTOP,ToolList,14,104,740,430);stateNames(window,*data);
  }else if(tool==2){
    control(window,L"EDIT",L"0",ES_NUMBER|WS_TABSTOP,ToolValue,14,14,70,28);
    HWND comparison=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,ToolCompare,100,14,240,200);for(auto title:{L"Equal to value",L"Changed",L"Unchanged",L"Increased",L"Decreased"})SendMessageW(comparison,CB_ADDSTRING,0,(LPARAM)title);SendMessageW(comparison,CB_SETCURSEL,0,0);
    control(window,L"BUTTON",L"Search / Narrow",BS_PUSHBUTTON|WS_TABSTOP,ToolAction,355,14,160,28);control(window,L"BUTTON",L"Restart",BS_PUSHBUTTON|WS_TABSTOP,ToolRestart,535,14,100,28);
    label(window,L"WRAM byte search. Use the displayed address in the Cheat Editor: 7e1234=ff",14,64,740);
    control(window,L"LISTBOX",L"",LBS_NOTIFY|WS_VSCROLL|WS_TABSTOP,ToolList,14,104,740,430);
  }else{
    if(tool==1){control(window,L"BUTTON",L"Save and Apply",BS_PUSHBUTTON|WS_TABSTOP,ToolAction,14,14,160,28);control(window,L"BUTTON",L"Database...",BS_PUSHBUTTON|WS_TABSTOP,ToolDatabase,190,14,140,28);label(window,L"Existing cheat-file format, or one Game Genie / PAR / address=value code per line.",14,64,740);}
    HWND edit=control(window,L"EDIT",L"",ES_MULTILINE|ES_WANTRETURN|ES_AUTOVSCROLL|ES_AUTOHSCROLL|WS_VSCROLL|WS_HSCROLL|WS_TABSTOP|(tool==1?0:ES_READONLY),ToolText,14,104,740,430);SendMessageW(edit,EM_SETLIMITTEXT,8*1024*1024,0);
    string content;
    if(tool==1)content=string::read(program.cheatPath());
    if(tool==3)for(auto manifest:emulator->manifests())content.append(manifest,"\r\n\r\n");
    if(tool==4){
      auto document=BML::unserialize(string::read(locate("Database/Cheat Codes.bml")));
      for(auto game:document.find("game")){bool matches=false;for(auto hash:emulator->hashes())if(game["sha256"].text()==hash)matches=true;if(matches)content.append(BML::serialize(game),"\r\n");}
      if(!content)content="No database entry matches this game.";
    }
    put(edit,content);SendMessageW(edit,EM_SETMODIFY,FALSE,0);
  }
  ShowWindow(window,SW_SHOWNORMAL);SendMessageW(window,WM_SIZE,0,0);
}

auto nativeCloseTools() -> void {while(!toolWindows.empty())SendMessageW(toolWindows.back(),WM_CLOSE,0,0);}

namespace {
LRESULT CALLBACK ViewportProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
  if(message==WM_ERASEBKGND)return 1;
  if(message==WM_PAINT) {
    PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);
    if(!program.screenshot.data)FillRect(dc,&paint.rcPaint,(HBRUSH)GetStockObject(BLACK_BRUSH));
    EndPaint(window,&paint);queueVideoRefresh();return 0;
  }
  return DefWindowProcW(window,message,wparam,lparam);
}
LRESULT MainProcImpl(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
  if(program.inFrame && (message==WM_NCLBUTTONDOWN || message==WM_MOUSEMOVE
    || message==WM_LBUTTONUP || message==WM_NCLBUTTONUP || message==WM_COMMAND
    || message==WM_CLOSE || message==WM_KEYDOWN || message==WM_KEYUP || message==WM_ACTIVATEAPP)) {
    deferMessage(window,message,wparam,lparam);return 0;
  }
  switch(message){
  // Avoid DefWindowProc's nested sizing loop and its timer-driven emulation.
  // Mouse capture delivers each pixel of movement through the normal loop.
  case WM_NCLBUTTONDOWN:
    fastLinkCrashStep("Win32 resize-edge press");
    if(wparam>=HTLEFT && wparam<=HTBOTTOMRIGHT && !IsZoomed(window)) {
      dragEdge=(UINT)wparam;dragOrigin={(short)LOWORD(lparam),(short)HIWORD(lparam)};GetWindowRect(window,&dragRectangle);
      SetCapture(window);return 0;
    }
    break;
  case WM_MOUSEMOVE:if(dragEdge){dragResize(window);return 0;}break;
  case WM_LBUTTONUP:case WM_NCLBUTTONUP:
    if(dragEdge){dragResize(window);dragEdge=0;ReleaseCapture();recordWindowGeometry();return 0;}break;
  case WM_CAPTURECHANGED:case WM_CANCELMODE:
    if(dragEdge){dragEdge=0;if(GetCapture()==window)ReleaseCapture();recordWindowGeometry();}break;
  case WM_SETCURSOR:
    if(dragEdge) {
      LPCWSTR cursor=dragEdge==HTLEFT||dragEdge==HTRIGHT?IDC_SIZEWE:
        dragEdge==HTTOP||dragEdge==HTBOTTOM?IDC_SIZENS:
        dragEdge==HTTOPLEFT||dragEdge==HTBOTTOMRIGHT?IDC_SIZENWSE:IDC_SIZENESW;
      SetCursor(LoadCursorW(nullptr,cursor));return TRUE;
    }
    break;
  case WM_WINDOWPOSCHANGED:{
    LRESULT result=DefWindowProcW(window,message,wparam,lparam);
    recordWindowGeometry();return result;
  }
  case WM_GETMINMAXINFO:{
    fastLinkCrashStep("Win32 minimum window size");
    auto limits=(MINMAXINFO*)lparam;auto minimum=minimumWindowSize(window);
    limits->ptMinTrackSize.x=max(limits->ptMinTrackSize.x,minimum.cx);
    limits->ptMinTrackSize.y=max(limits->ptMinTrackSize.y,minimum.cy);return 0;
  }
  // Adapted from v64tng's HandleSizing/HandleSize. Keep the opposite edge
  // fixed at minimum size, but allow arbitrary width and height above it.
  case WM_SIZING:{
    const UINT edges[]={HTLEFT,HTRIGHT,HTTOP,HTTOPLEFT,HTTOPRIGHT,HTBOTTOM,HTBOTTOMLEFT,HTBOTTOMRIGHT};
    if(wparam>=WMSZ_LEFT && wparam<=WMSZ_BOTTOMRIGHT)constrainResize(window,*(RECT*)lparam,edges[wparam-WMSZ_LEFT],dragRectangle);
    return TRUE;
  }
  case WM_SIZE:if(wparam==SIZE_MINIMIZED)video.poll();else layoutPending=true;return 0;
  case WM_ERASEBKGND:return 1;
  case RefreshVideoMessage:
    videoRefreshPending=false;
    refreshRequested=true;
    return 0;
  case WM_COMMAND:
    if(program.inFrame)return 0;
    if(LOWORD(wparam)==199){program.loadState({"Quick/Slot ",selectedStateSlot+1});audio.clear();program.resetFramePacer();return 0;}
    command(LOWORD(wparam));return 0;
  case WM_DROPFILES:{HDROP drop=(HDROP)wparam;uint count=DragQueryFileW(drop,0xffffffff,nullptr,0);program.gameQueue={};for(uint i=0;i<count;i++){uint length=DragQueryFileW(drop,i,nullptr,0);std::wstring path(length+1,L'\0');DragQueryFileW(drop,i,path.data(),length+1);string filename=(const char*)utf8_t(path.c_str());filename.transform("\\","/");if(directory::exists(filename)&&!filename.endsWith("/"))filename.append("/");program.gameQueue.append({"Auto;",filename});}DragFinish(drop);program.load();return 0;}
  case WM_KEYDOWN:
    if(wparam==VK_ESCAPE && dragEdge){dragEdge=0;ReleaseCapture();SetWindowPos(window,nullptr,dragRectangle.left,dragRectangle.top,dragRectangle.right-dragRectangle.left,dragRectangle.bottom-dragRectangle.top,SWP_NOZORDER|SWP_NOACTIVATE);recordWindowGeometry();return 0;}
    if(wparam==VK_TAB&&!(lparam&(1u<<30))){program.fastForwarding=true;program.updateAudioFrequency();program.resetFramePacer();return 0;}break;
  case WM_KEYUP:if(wparam==VK_TAB){program.fastForwarding=false;program.updateAudioFrequency();audio.clear();program.resetFramePacer();return 0;}break;
  case WM_ACTIVATEAPP:if(!wparam&&program.fastForwarding){program.fastForwarding=false;program.updateAudioFrequency();program.resetFramePacer();}break;
  case WM_ENTERMENULOOP:case WM_ENTERSIZEMOVE:if(message==WM_ENTERSIZEMOVE)GetWindowRect(window,&dragRectangle);if(systemLoopDepth++==0)SetTimer(window,1,1,nullptr);return 0;
  case WM_EXITMENULOOP:case WM_EXITSIZEMOVE:
    if(systemLoopDepth>0&&--systemLoopDepth==0)KillTimer(window,1);
    recordWindowGeometry();return 0;
  case WM_TIMER:
    if(wparam==1&&systemLoopDepth&&!program.inFrame){
      if(layoutPending){layoutPending=false;nativeLayout();}
      program.main();
    }
    return 0;
  case WM_POWERBROADCAST:
    if(wparam==PBT_APMSUSPEND){program.hostSuspended=true;program.resetFramePacer();}
    if(wparam==PBT_APMRESUMEAUTOMATIC || wparam==PBT_APMRESUMESUSPEND){program.hostSuspended=false;program.resetFramePacer();}
    return TRUE;
  case WM_SYSCOMMAND:if(emulator&&emulator->loaded()&&!settings.general.screenSaver&&((wparam&0xfff0)==SC_SCREENSAVE||(wparam&0xfff0)==SC_MONITORPOWER))return 0;break;
  case WM_DPICHANGED:{pendingDragRectangle=*(RECT*)lparam;dragGeometryPending=true;layoutPending=true;return 0;}
  case WM_CLOSE:recordWindowGeometry();program.quit();return 0;
  case WM_DESTROY:PostQuitMessage(0);return 0;
  }
  return DefWindowProcW(window,message,wparam,lparam);
}
LRESULT CALLBACK MainProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
  // Catch callback faults before USER32 can turn them into a silent exit.
  __try { return MainProcImpl(window,message,wparam,lparam); }
  __except(fastLinkReportException(GetExceptionInformation())) {
    TerminateProcess(GetCurrentProcess(),1);return 0;
  }
}
}

auto nativeCreate(HINSTANCE application) -> bool {
  instance=application;INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_TAB_CLASSES|ICC_UPDOWN_CLASS|ICC_BAR_CLASSES};InitCommonControlsEx(&controls);
  NONCLIENTMETRICSW metrics{sizeof(metrics)};SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0);uiFont=CreateFontIndirectW(&metrics.lfMessageFont);
  WNDCLASSW window{};window.hInstance=instance;window.lpfnWndProc=MainProc;window.hCursor=LoadCursor(nullptr,IDC_ARROW);window.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(2));window.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);window.lpszClassName=L"FastLinkMain";if(!RegisterClassW(&window))return false;
  WNDCLASSW viewport{};viewport.hInstance=instance;viewport.lpfnWndProc=ViewportProc;viewport.hCursor=LoadCursor(nullptr,IDC_ARROW);viewport.lpszClassName=L"FastLinkViewport";if(!RegisterClassW(&viewport))return false;
  mainWindow=CreateWindowExW(WS_EX_ACCEPTFILES,window.lpszClassName,L"bsnes-fastlink",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,settings.general.window.x,settings.general.window.y,settings.general.window.width?settings.general.window.width:960,settings.general.window.height?settings.general.window.height:760,nullptr,nullptr,instance,nullptr);if(!mainWindow)return false;
  videoWindow=CreateWindowExW(0,viewport.lpszClassName,L"",WS_CHILD|WS_VISIBLE,0,0,1,1,mainWindow,nullptr,instance,nullptr);
  statusWindow=CreateWindowExW(0,STATUSCLASSNAMEW,L"",WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,0,0,0,0,mainWindow,nullptr,instance,nullptr);SendMessageW(statusWindow,WM_SETFONT,(WPARAM)uiFont,FALSE);
  buildMenus();nativeTitle();nativeMenus();nativeLayout();
  if(!settings.general.window.maximized) {
    if(!settings.general.window.width || !settings.general.window.height)
      resizeClient(256*settings.video.multiplier,(settings.video.overscan?240:224)*settings.video.multiplier);
    else {
      RECT client{};GetClientRect(videoWindow,&client);
      auto fit=fastLinkFitVideo(client.right,client.bottom,settings.video.overscan);
      auto size=fastLinkResizeVideo(fit.width,fit.height,false,settings.video.overscan);
      if(size.width!=client.right || size.height!=client.bottom)resizeClient(size.width,size.height);
    }
  }
  ACCEL keys[]={{FVIRTKEY|FCONTROL,'O',CommandOpen},{FVIRTKEY|FCONTROL,'Q',CommandQuit},{FVIRTKEY|FALT,VK_RETURN,CommandFullscreen},{FVIRTKEY,VK_F11,CommandFullscreen},{FVIRTKEY,VK_F5,CommandSaveUndo},{FVIRTKEY,VK_F8,199},{FVIRTKEY,VK_F9,CommandReset},{FVIRTKEY,VK_F6,CommandFrame},{FVIRTKEY,VK_F12,CommandScreenshot},{FVIRTKEY,VK_PAUSE,CommandPause}};
  accelerators=CreateAcceleratorTableW(keys,(int)(sizeof(keys)/sizeof(keys[0])));
  ShowWindow(mainWindow,settings.general.window.maximized?SW_SHOWMAXIMIZED:SW_SHOWNORMAL);
  UpdateWindow(mainWindow);windowGeometryReady=true;recordWindowGeometry();return true;
}
auto nativeRun() -> int {
  MSG message{};
  while(!quitting){
    // Vulkan/USER32 may dispatch user input while a driver call is active.
    // Execute those actions after the frame unwinds, with no driver on stack.
    if(deferredMessageCount) {
      auto pending=deferredMessages;uint count=deferredMessageCount;deferredMessageCount=0;
      for(uint i=0;i<count&&!quitting;i++)if(IsWindow(pending[i].hwnd))
        SendMessageW(pending[i].hwnd,pending[i].message,pending[i].wParam,pending[i].lParam);
    }
    // Bound message draining so continuous mouse traffic cannot starve SNES.
    for(uint messages=0;messages<64&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE);messages++){
      if(message.message==WM_QUIT){quitting=true;break;}
      bool dialog=optionsWindow&&IsDialogMessageW(optionsWindow,&message);
      if(!dialog)for(HWND tool:toolWindows)if(IsDialogMessageW(tool,&message)){dialog=true;break;}
      if(!dialog&&!TranslateAcceleratorW(mainWindow,accelerators,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    if(quitting)break;
    // Window procedures only record resize/redraw requests. Apply them here,
    // with no Vulkan call or SNES frame on the stack, once per host iteration.
    if(dragGeometryPending){
      RECT rectangle=pendingDragRectangle;dragGeometryPending=false;
      SetWindowPos(mainWindow,nullptr,rectangle.left,rectangle.top,rectangle.right-rectangle.left,
        rectangle.bottom-rectangle.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
    }
    if(layoutPending){layoutPending=false;nativeLayout();}
    if(refreshRequested){if(program.inactive())refreshRequested=!program.refresh();else refreshRequested=false;}
    if(program.inactive())MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);else program.main();
  }
  return (int)message.wParam;
}
auto nativeShutdown() -> void {
  recordWindowGeometry();windowGeometryReady=false;
  nativeCloseTools();if(optionsWindow)SendMessageW(optionsWindow,WM_CLOSE,0,0);
  if(mainWindow)DestroyWindow(mainWindow);mainWindow=videoWindow=statusWindow=nullptr;menuBar=nullptr;
  if(accelerators)DestroyAcceleratorTable(accelerators);if(uiFont)DeleteObject(uiFont);
}

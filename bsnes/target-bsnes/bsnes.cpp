#include "bsnes.hpp"
#include <mmsystem.h>
#include <sfc/interface/interface.hpp>
NativeVideoManager video;
ruby::Audio audio;
unique_pointer<Emulator::Interface> emulator;

auto locate(string name) -> string {
  string location={Path::program(),name};if(inode::exists(location))return location;
  location={Path::userData(),"bsnes/",name};if(inode::exists(location))return location;
  directory::create({Path::userSettings(),"bsnes/"});return {Path::userSettings(),"bsnes/",name};
}
int main() {
  ULONG crashStack=64*1024;SetThreadStackGuarantee(&crashStack);
  SetUnhandledExceptionFilter(fastLinkReportException);
  CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  settings.location=locate("settings.bml");
  int count=0;auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);
  for(int i=1;arguments&&i<count;i++){
    string argument=(const char*)utf8_t(arguments[i]);
    if(argument=="--fullscreen")program.startFullScreen=true;
    else if(argument.beginsWith("--settings="))settings.location=argument.slice(11);
    else if(inode::exists(argument)){if(directory::exists(argument)&&!argument.endsWith("/"))argument.append("/");program.gameQueue.append({"Auto;",argument});}
    else if(argument.find(";"))program.gameQueue.append(argument);
  }
  if(arguments)LocalFree(arguments);
  settings.load();emulator=new SuperFamicom::Interface;
  int result=1;
  if(nativeCreate(GetModuleHandleW(nullptr))&&program.create())result=nativeRun();
  nativeCloseTools();
  if(optionsWindow)SendMessageW(optionsWindow,WM_CLOSE,0,0);
  program.unload();fastLinkRAMTraceService.stop();nativeInputManager.terminate();audio.reset();video.reset();program.timing.save();
  nativeShutdown();settings.save();
  SetThreadExecutionState(ES_CONTINUOUS);
  program.shutdown();
  emulator.reset();CoUninitialize();return result;
}

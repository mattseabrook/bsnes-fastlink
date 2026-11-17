#pragma once
extern HWND mainWindow, videoWindow, statusWindow, optionsWindow;
extern int modalDepth;
extern bool quitting;
extern uint selectedStateSlot;
auto nativeCreate(HINSTANCE instance) -> bool;
auto nativeRun() -> int;
auto nativeShutdown() -> void;
auto nativeMenus() -> void;
auto nativeTitle() -> void;
auto nativeLayout() -> void;
auto nativeStatus(const string& text) -> void;
auto nativeOptions(int page = 0) -> void;
auto nativeTool(int tool) -> void;
auto nativeCloseTools() -> void;
auto nativeFile(bool save, const string& title, const string& initial = "") -> string;
auto nativeFolder(HWND owner, const string& initial) -> string;
auto nativeError(const string& text) -> void;
auto nativeConfirm(const string& text, const string& title) -> bool;

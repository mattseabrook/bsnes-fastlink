#if defined(Hiro_BrowserWindow)

namespace hiro {

static auto BrowserWindowCrashStep(const char* step) -> void {
}

static auto CALLBACK BrowserWindowCallbackProc(HWND hwnd, UINT msg, LPARAM lparam, LPARAM lpdata) -> signed {
  if(msg == BFFM_INITIALIZED) {
    if(lpdata) {
      auto state = (BrowserWindow::State*)lpdata;
      utf16_t wpath(string{state->path}.transform("/", "\\"));
      if(state->title) SetWindowText(hwnd, utf16_t(state->title));
      SendMessage(hwnd, BFFM_SETSELECTION, TRUE, (LPARAM)(wchar_t*)wpath);
    }
  }
  return 0;
}

static auto BrowserWindowFilters(const vector<string>& filters) -> vector<wchar_t> {
  vector<wchar_t> result;

  for(auto& filter : filters) {
    auto part = filter.split("|", 1L);
    if(part.size() != 2) continue;

    utf16_t name(part(0));
    utf16_t pattern(string{part(1)}.transform(":", ";"));

    for(uint i = 0; i < name.size(); i++) result.append(name[i]);
    result.append(L'\0');

    for(uint i = 0; i < pattern.size(); i++) result.append(pattern[i]);
    result.append(L'\0');
  }

  result.append(L'\0');
  return result;
}

static auto BrowserWindow_fileDialog(bool save, BrowserWindow::State& state) -> string {
  string path = string{state.path}.replace("/", "\\");

  auto wfilters = BrowserWindowFilters(state.filters);
  wchar_t wname[PATH_MAX + 1] = L"";
  utf16_t wpath(path);
  utf16_t wtitle(state.title);

  OPENFILENAME ofn;
  memset(&ofn, 0, sizeof(OPENFILENAME));
  ofn.lStructSize = sizeof(OPENFILENAME);
  ofn.hwndOwner = 0;
  if(state.parent) {
    auto hwnd = state.parent->self()->hwnd;
    if(hwnd && IsWindow(hwnd)) ofn.hwndOwner = hwnd;
  }
  ofn.lpstrFilter = wfilters.data();
  ofn.lpstrInitialDir = wpath;
  ofn.lpstrFile = wname;
  ofn.lpstrTitle = wtitle;
  ofn.nMaxFile = PATH_MAX;
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
  ofn.lpstrDefExt = L"";

  BrowserWindowCrashStep("BrowserWindow_fileDialog: GetOpenFileName begin");
  bool result = (save == false ? GetOpenFileName(&ofn) : GetSaveFileName(&ofn));
  BrowserWindowCrashStep("BrowserWindow_fileDialog: GetOpenFileName done");
  if(result == false) return "";
  string name = (const char*)utf8_t(wname);
  name.transform("\\", "/");
  return name;
}

auto pBrowserWindow::directory(BrowserWindow::State& state) -> string {
  wchar_t wname[PATH_MAX + 1] = L"";

  BROWSEINFO bi;
  bi.hwndOwner = 0;
  if(state.parent) {
    auto hwnd = state.parent->self()->hwnd;
    if(hwnd && IsWindow(hwnd)) bi.hwndOwner = hwnd;
  }
  bi.pidlRoot = NULL;
  bi.pszDisplayName = wname;
  bi.lpszTitle = L"\nChoose a directory:";
  bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS;
  bi.lpfn = BrowserWindowCallbackProc;
  bi.lParam = (LPARAM)&state;
  bi.iImage = 0;
  bool result = false;
  BrowserWindowCrashStep("BrowserWindow_fileDialog: SHBrowseForFolder begin");
  LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
  BrowserWindowCrashStep("BrowserWindow_fileDialog: SHBrowseForFolder done");
  if(pidl) {
    if(SHGetPathFromIDList(pidl, wname)) {
      result = true;
      IMalloc *imalloc = 0;
      if(SUCCEEDED(SHGetMalloc(&imalloc))) {
        imalloc->Free(pidl);
        imalloc->Release();
      }
    }
  }
  if(result == false) return "";
  string name = (const char*)utf8_t(wname);
  if(!name) return "";
  name.transform("\\", "/");
  if(name.endsWith("/") == false) name.append("/");
  return name;
}

auto pBrowserWindow::open(BrowserWindow::State& state) -> string {
  return BrowserWindow_fileDialog(0, state);
}

auto pBrowserWindow::save(BrowserWindow::State& state) -> string {
  return BrowserWindow_fileDialog(1, state);
}

}

#endif

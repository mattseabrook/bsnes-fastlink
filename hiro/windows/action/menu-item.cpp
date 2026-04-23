#if defined(Hiro_MenuItem)

namespace hiro {

auto pMenuItem::construct() -> void {
  _createBitmap();
}

auto pMenuItem::destruct() -> void {
  if(hbitmap) { DeleteObject(hbitmap); hbitmap = nullptr; }
}

auto pMenuItem::setIcon(const image& icon) -> void {
  _createBitmap();
  _synchronize();
}

auto pMenuItem::setText(const string& text) -> void {
  _synchronize();
}

auto pMenuItem::onActivate() -> void {
  self().doActivate();
}

auto pMenuItem::_createBitmap() -> void {
  if(hbitmap) { DeleteObject(hbitmap); hbitmap = nullptr; }

  //Use native text-only Win32 menus here; custom menu bitmaps make the menus feel heavier
  //and diverge from standard Windows rendering.
}

}

#endif

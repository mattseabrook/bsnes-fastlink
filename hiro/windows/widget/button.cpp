#if defined(Hiro_Button)

namespace hiro {

auto pButton::construct() -> void {
  hwnd = CreateWindow(
    L"BUTTON", L"", WS_CHILD | WS_TABSTOP,
    0, 0, 0, 0, _parentHandle(), nullptr, GetModuleHandle(0), 0
  );
  pWidget::construct();
  _setState();
}

auto pButton::destruct() -> void {
  DestroyWindow(hwnd);
  hwnd = nullptr;
  if(nativeImages) { ImageList_Destroy(nativeImages); nativeImages = nullptr; }
}

auto pButton::minimumSize() const -> Size {
  Size icon = {(int)state().icon.width(), (int)state().icon.height()};
  Size text = state().text ? pFont::size(self().font(true), state().text) : Size{};
  Size size;
  if(state().orientation == Orientation::Horizontal) {
    size.setWidth(icon.width() + (icon && text ? 5 : 0) + text.width());
    size.setHeight(max(icon.height(), text.height()));
  }
  if(state().orientation == Orientation::Vertical) {
    size.setWidth(max(icon.width(), text.width()));
    size.setHeight(icon.height() + (icon && text ? 5 : 0) + text.height());
  }
  size.setHeight(max(size.height(), pFont::size(self().font(true), " ").height()));
  return {size.width() + (state().bordered && text ? 20 : 10), size.height() + 10};
}

auto pButton::setBordered(bool bordered) -> void {
  _setState();
}

auto pButton::setEnabled(bool enabled) -> void {
  pWidget::setEnabled(enabled);
}

auto pButton::setFont(const Font& font) -> void {
  pWidget::setFont(font);
}

auto pButton::setIcon(const image& icon) -> void {
  _setState();
}

auto pButton::setOrientation(Orientation orientation) -> void {
  _setState();
}

auto pButton::setText(const string& text) -> void {
  _setState();
}

auto pButton::setVisible(bool visible) -> void {
  pWidget::setVisible(visible);
}

//

auto pButton::onActivate() -> void {
  self().doActivate();
}

//BUTTON cannot draw borderless buttons on its own
//BS_OWNERDRAW will send WM_DRAWITEM; but will disable hot-tracking notifications
//to gain hot-tracking plus borderless buttons; BUTTON is superclassed and WM_PAINT is hijacked
//note: letting hiro paint bordered buttons will lose the fade animations on Vista+;
//however, it will allow placing icons immediately next to text (original forces icon left alignment)
auto pButton::windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> maybe<LRESULT> {
  // Native BUTTON handles painting, focus cues, Space/Enter and click dispatch.
  return pWidget::windowProc(hwnd, msg, wparam, lparam);
}

//

auto pButton::_setState() -> void {
  auto style = GetWindowLongPtr(hwnd, GWL_STYLE);
  SetWindowLongPtr(hwnd, GWL_STYLE, state().bordered ? style & ~BS_FLAT : style | BS_FLAT);
  Button_SetNativeState(hwnd, nativeImages, state().icon, state().orientation, state().text);
}

}

#endif

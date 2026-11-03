#if defined(Hiro_RadioButton)

namespace hiro {

auto pRadioButton::construct() -> void {
  hwnd = CreateWindow(L"BUTTON", L"",
    WS_CHILD | WS_TABSTOP | BS_CHECKBOX | BS_PUSHLIKE,
    0, 0, 0, 0, _parentHandle(), nullptr, GetModuleHandle(0), 0);
  pWidget::construct();
  setGroup(state().group);
  _setState();
}

auto pRadioButton::destruct() -> void {
  DestroyWindow(hwnd);
  hwnd = nullptr;
  if(nativeImages) { ImageList_Destroy(nativeImages); nativeImages = nullptr; }
}

auto pRadioButton::minimumSize() const -> Size {
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

auto pRadioButton::setBordered(bool bordered) -> void {
  _setState();
}

auto pRadioButton::setChecked() -> void {
  if(auto& group = state().group) {
    for(auto& weak : group->state.objects) {
      if(auto object = weak.acquire()) {
        if(auto radioButton = dynamic_cast<mRadioButton*>(object.data())) {
          if(auto self = radioButton->self()) {
            SendMessage(self->hwnd, BM_SETCHECK, (WPARAM)(&self->reference == &reference), 0);
          }
        }
      }
    }
  }
}

auto pRadioButton::setEnabled(bool enabled) -> void {
  pWidget::setEnabled(enabled);
}

auto pRadioButton::setFont(const Font& font) -> void {
  pWidget::setFont(font);
}

auto pRadioButton::setGroup(sGroup group) -> void {
  bool first = true;
  if(auto& group = state().group) {
    for(auto& weak : group->state.objects) {
      if(auto object = weak.acquire()) {
        if(auto radioButton = dynamic_cast<mRadioButton*>(object.data())) {
          if(auto self = radioButton->self()) {
            SendMessage(self->hwnd, BM_SETCHECK, (WPARAM)(radioButton->state.checked = first), 0);
            first = false;
          }
        }
      }
    }
  }
}

auto pRadioButton::setIcon(const image& icon) -> void {
  _setState();
}

auto pRadioButton::setOrientation(Orientation orientation) -> void {
  _setState();
}

auto pRadioButton::setText(const string& text) -> void {
  _setState();
}

auto pRadioButton::setVisible(bool visible) -> void {
  pWidget::setVisible(visible);
}

//

auto pRadioButton::onActivate() -> void {
  if(state().checked) return;
  self().setChecked();
  self().doActivate();
}

auto pRadioButton::windowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> maybe<LRESULT> {
  // Native BUTTON handles painting, focus cues, Space/Enter and click dispatch.
  return pWidget::windowProc(hwnd, msg, wparam, lparam);
}

//

auto pRadioButton::_setState() -> void {
  auto style = GetWindowLongPtr(hwnd, GWL_STYLE);
  SetWindowLongPtr(hwnd, GWL_STYLE, state().bordered ? style & ~BS_FLAT : style | BS_FLAT);
  Button_SetNativeState(hwnd, nativeImages, state().icon, state().orientation, state().text);
}

}

#endif

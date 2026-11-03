#if defined(Hiro_ComboButton)

namespace hiro {

auto pComboButton::construct() -> void {
  hwnd = CreateWindow(
    L"COMBOBOX", L"",
    WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
    0, 0, 0, 0,
    _parentHandle(), nullptr, GetModuleHandle(0), 0
  );
  pWidget::construct();
  SendMessage(hwnd, CB_SETEXTENDEDUI, TRUE, 0);
  #if defined(CB_SETMINVISIBLE)
  SendMessage(hwnd, CB_SETMINVISIBLE, 12, 0);
  #endif
  for(auto& item : state().items) append(item);
}

auto pComboButton::destruct() -> void {
  DestroyWindow(hwnd);
}

auto pComboButton::append(sComboButtonItem item) -> void {
  lock();
  SendMessage(hwnd, CB_ADDSTRING, 0, (LPARAM)(wchar_t*)utf16_t(item->state.text));
  if(item->state.selected) SendMessage(hwnd, CB_SETCURSEL, item->offset(), 0);
  if(SendMessage(hwnd, CB_GETCURSEL, 0, 0) == CB_ERR) item->setSelected();
  unlock();
}

auto pComboButton::minimumSize() const -> Size {
  signed width = 0;
  for(auto& item : state().items) {
    width = max(width, pFont::size(hfont, item->state.text).width());
  }
  return {width + 24_sx, pFont::size(hfont, " ").height() + 10_sy};
}

auto pComboButton::remove(sComboButtonItem item) -> void {
  lock();
  SendMessage(hwnd, CB_DELETESTRING, item->offset(), 0);
  if(item->state.selected) SendMessage(hwnd, CB_SETCURSEL, 0, 0);
  unlock();
}

auto pComboButton::reset() -> void {
  SendMessage(hwnd, CB_RESETCONTENT, 0, 0);
}

// A Win32 COMBOBOX's window height includes the drop-down. Keep its native
// selection height and give the list real space, rather than a one-pixel box.
auto pComboButton::setGeometry(Geometry geometry) -> void {
  RECT rc{};
  GetWindowRect(hwnd, &rc);
  auto closedHeight = rc.bottom - rc.top;
  auto itemHeight = SendMessage(hwnd, CB_GETITEMHEIGHT, 0, 0);
  if(itemHeight == CB_ERR || itemHeight < 1) itemHeight = max(1, closedHeight);
  auto rows = max(1u, min(12u, (uint)state().items.size()));
  geometry.setY(geometry.y() + (geometry.height() - closedHeight) / 2);
  pWidget::setGeometry({geometry.x(), geometry.y(), geometry.width(), closedHeight + (rows + 1) * itemHeight});
}

auto pComboButton::onChange() -> void {
  signed offset = SendMessage(hwnd, CB_GETCURSEL, 0, 0);
  if(offset == CB_ERR) return;
  for(auto& item : state().items) item->state.selected = false;
  if(auto item = self().item(offset)) item->setSelected();
  self().doChange();
}

}

#endif

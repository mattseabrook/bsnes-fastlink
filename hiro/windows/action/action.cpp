#if defined(Hiro_Action)

namespace hiro {

auto pAction::construct() -> void {
}

auto pAction::destruct() -> void {
}

auto pAction::setEnabled(bool enabled) -> void {
  if(auto menu = _nativeParentMenu()) {
    EnableMenuItem(menu, position, MF_BYPOSITION | (self().enabled(true) ? MF_ENABLED : MF_GRAYED));
    if(auto parent = _parentMenuBar()) if(auto window = parent->_parent()) DrawMenuBar(window->hwnd);
  }
}

auto pAction::_nativeParentMenu() -> HMENU {
  if(position == ~0u || !self().visible()) return nullptr;
  if(auto parent = _parentMenu()) return parent->hmenu;
  if(auto parent = _parentPopupMenu()) return parent->hmenu;
  if(auto parent = _parentMenuBar()) return parent->hmenu;
  return nullptr;
}

auto pAction::_setText(const string& text) -> void {
  if(auto menu = _nativeParentMenu()) {
    utf16_t nativeText(text);
    MENUITEMINFO item{sizeof(MENUITEMINFO)};
    item.fMask = MIIM_STRING;
    item.dwTypeData = (wchar_t*)nativeText;
    SetMenuItemInfo(menu, position, TRUE, &item);
    if(auto parent = _parentMenuBar()) if(auto window = parent->_parent()) DrawMenuBar(window->hwnd);
  }
}

auto pAction::setVisible(bool visible) -> void {
  _synchronize();
}

auto pAction::_parentMenu() -> maybe<pMenu&> {
  if(auto parent = self().parentMenu()) {
    if(auto self = parent->self()) return *self;
  }
  return nothing;
}

auto pAction::_parentMenuBar() -> maybe<pMenuBar&> {
  if(auto parent = self().parentMenuBar(true)) {
    if(auto self = parent->self()) return *self;
  }
  return nothing;
}

auto pAction::_parentPopupMenu() -> maybe<pPopupMenu&> {
  if(auto parent = self().parentPopupMenu(true)) {
    if(auto self = parent->self()) return *self;
  }
  return nothing;
}

auto pAction::_synchronize() -> void {
  if(auto parent = _parentMenuBar()) parent->_update();
}

}

#endif

#if defined(Hiro_MenuBar)

namespace hiro {

auto pMenuBar::construct() -> void {
  _update();
}

auto pMenuBar::destruct() -> void {
  if(auto parent = _parent()) {
    SetMenu(parent->hwnd, nullptr);
  }
  if(hmenu) {
    while(GetMenuItemCount(hmenu) > 0) RemoveMenu(hmenu, 0, MF_BYPOSITION);
    DestroyMenu(hmenu);
    hmenu = nullptr;
  }
}

auto pMenuBar::append(sMenu) -> void {
  _update();
}

auto pMenuBar::remove(sMenu) -> void {
  _update();
}

auto pMenuBar::setEnabled(bool enabled) -> void {
  _update();
}

auto pMenuBar::setFont(const Font& font) -> void {
  //unsupported
}

auto pMenuBar::setVisible(bool visible) -> void {
  if(auto parent = _parent()) {
    SetMenu(parent->hwnd, visible ? hmenu : nullptr);
    parent->setGeometry(parent->state().geometry);
  }
}

auto pMenuBar::_parent() -> maybe<pWindow&> {
  if(auto parent = self().parentWindow(true)) {
    if(auto self = parent->self()) return *self;
  }
  return nothing;
}

auto pMenuBar::_update() -> void {
  if(!hmenu) hmenu = CreateMenu();
  while(GetMenuItemCount(hmenu) > 0) RemoveMenu(hmenu, 0, MF_BYPOSITION);

  MENUINFO mi{sizeof(MENUINFO)};
  mi.fMask = MIM_STYLE;
  mi.dwStyle = MNS_NOTIFYBYPOS;  //| MNS_MODELESS;
  SetMenuInfo(hmenu, &mi);

  unsigned position = 0;

  #if defined(Hiro_Menu)
  for(auto& menu : state().menus) {
    if(auto peer = menu->self()) peer->position = ~0u;
    unsigned enabled = menu->enabled() ? 0 : MF_GRAYED;

    MENUITEMINFO mii{sizeof(MENUITEMINFO)};
    mii.fMask = MIIM_DATA;
    mii.dwItemData = (ULONG_PTR)menu.data();

    if(menu->visible()) {
      if(auto self = menu->self()) {
        self->position = position;
        self->_update();
        AppendMenu(hmenu, MF_STRING | MF_POPUP | enabled, (UINT_PTR)self->hmenu, utf16_t(menu->text()));
        SetMenuItemInfo(hmenu, position++, true, &mii);
      }
    }
  }
  #endif

  if(auto parent = _parent()) {
    auto desired = self().visible(true) ? hmenu : nullptr;
    if(GetMenu(parent->hwnd) != desired) {
      SetMenu(parent->hwnd, desired);
      parent->setGeometry(parent->state().geometry);
    }
    DrawMenuBar(parent->hwnd);
  }
}

}

#endif

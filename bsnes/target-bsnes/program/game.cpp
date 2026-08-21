auto Program::load() -> void {
  if(loading) {
    fastLinkDebug("Program::load ignored because another load is active");
    return;
  }
  fastLinkCrashStep("Program::load: begin");
  fastLinkDebug("Program::load begin queued=%d loaded=%d", (bool)gameQueue, emulator && emulator->loaded());
  loading = true;
  struct LoadGuard {
    bool& loading;
    ~LoadGuard() {
      fastLinkDebug("Program::load guard release");
      loading = false;
    }
  } loadGuard{loading};

  auto clearGameSlots = [&] {
    superFamicom = {};
    gameBoy = {};
    bsMemory = {};
    sufamiTurboA = {};
    sufamiTurboB = {};
  };

  if(!gameQueue) {
    fastLinkDebug("Program::load opening file dialog");
    fastLinkCrashStep("Program::load: before openGame");
    BrowserDialog dialog;
    dialog.setAlignment(*presentation);
    dialog.setOptions({"Auto", "NTSC", "PAL"});
    dialog.setTitle("Load SNES ROM");
    dialog.setPath(path("Games", settings.path.recent.superFamicom));
    dialog.setFilters({string{"SNES ROMs|*.sfc:*.smc:*.zip:*.7z:*.SFC:*.SMC:*.ZIP:*.7Z:*.Sfc:*.Smc:*.Zip"}, string{"All Files|*"}});
    auto location = openGame(dialog);
    fastLinkCrashStep("Program::load: after openGame");
    fastLinkDebug("Program::load dialog selected exists=%d location=%s", inode::exists(location), (const char*)location);
    if(!inode::exists(location)) return;
    gameQueue.append({dialog.option(), ";", location});
  }

  fastLinkDebug("Program::load acquiring frontend lock");
  fastLinkCrashStep("Program::load: acquire frontend lock");
  auto lock = acquire();
  fastLinkDebug("Program::load frontend lock acquired; unload previous game begin");
  unload();
  fastLinkDebug("Program::load unload previous game done");
  clearGameSlots();
  screenshot = {};

  fastLinkDebug("Program::load configured accuracy-only core");
  fastLinkDebug("Program::load emulator->load begin");
    fastLinkCrashStep("Program::load: emulator load begin");
  if(!emulator->load()) {
      fastLinkDebug("Program::load emulator->load failed");
      fastLinkCrashStep("Program::load: emulator load failed");
    gameQueue = {};
    screenshot = {};
    clearGameSlots();
    return showMessage("Game loading failed");
  }
  fastLinkCrashStep("Program::load: emulator load succeeded");
  fastLinkDebug("Program::load emulator->load succeeded loaded=%d title=%s", emulator->loaded(), (const char*)emulator->title());

  gameQueue = {};
  screenshot = {};
  if(!verified() && emulatorSettings.warnOnUnverifiedGames.checked()) {
    fastLinkDebug("Program::load unverified warning dialog begin");
    fastLinkCrashStep("Program::load: unverified warning");
    //Emulator::loaded() is true at this point:
    //prevent Program::main() from calling Emulator::run() during this dialog window
    auto lock = acquire();
    auto response = MessageDialog(
      "Warning: this game image is unverified.\n"
      "Running it *may* be a security risk.\n\n"
      "Do you wish to run the game anyway?"
    ).setAlignment(*presentation).question({"Always", "Yes", "No"});
    if(response == "No") {
      fastLinkDebug("Program::load unverified warning cancelled");
      emulator->unload();
      screenshot = {};
      clearGameSlots();
      return showMessage("Game loading cancelled");
    }
    if(response == "Always") {
      emulatorSettings.warnOnUnverifiedGames.setChecked(false).doToggle();
    }
    fastLinkDebug("Program::load unverified warning accepted");
  }
  fastLinkDebug("Program::load emulator->power begin");
  emulator->power();
  fastLinkDebug("Program::load emulator->power done");
  if(emulatorSettings.autoLoadStateOnLoad.checked()) {
    fastLinkDebug("Program::load auto load state begin");
    program.loadState("Quick/Undo");
    fastLinkDebug("Program::load auto load state done");
  }
  showMessage({
    verified() ? "Verified game loaded" : "Game loaded",
    appliedPatch() ? " and patch applied" : ""
  });
  presentation.setFocused();
  presentation.setTitle({emulator->title(), " - ", Emulator::Name});
  presentation.resetSystem.setEnabled(true);
  presentation.unloadGame.setEnabled(true);
  presentation.toolsMenu.setVisible(true);
  presentation.updateStateMenus();
  presentation.speedNormal.setChecked();
  presentation.runEmulation.setChecked().doActivate();
  presentation.updateProgramIcon();
  presentation.updateStatusIcon();
  fastLinkDebug("Program::load frontend menus/status updated");
  fastLinkDebug("Program::load cheatFinder.restart begin");
  cheatFinder.restart();  //clear any old cheat search results
  fastLinkDebug("Program::load cheatEditor.loadCheats begin");
  cheatEditor.loadCheats();
  fastLinkDebug("Program::load stateManager.loadStates begin");
  stateManager.loadStates();
  fastLinkDebug("Program::load manifestViewer.loadManifest begin");
  manifestViewer.loadManifest();
  fastLinkDebug("Program::load tools data loaded");

  string games;
  if(auto& game = superFamicom) games.append(game.option, ";", game.location, "|");
  if(auto& game = gameBoy     ) games.append(game.option, ";", game.location, "|");
  if(auto& game = bsMemory    ) games.append(game.option, ";", game.location, "|");
  if(auto& game = sufamiTurboA) games.append(game.option, ";", game.location, "|");
  if(auto& game = sufamiTurboB) games.append(game.option, ";", game.location, "|");
  presentation.addRecentGame(games.trimRight("|", 1L));

  fastLinkDebug("Program::load update video/audio effects begin");
  updateVideoPalette();
  updateVideoEffects();
  updateAudioEffects();
  updateAudioFrequency();
  fastLinkDebug("Program::load complete");
  fastLinkCrashStep("Program::load: complete");
}

auto Program::loadFile(string location) -> vector<uint8_t> {
  auto suffix = Location::suffix(location).downcase();
  fastLinkDebug("Program::loadFile begin suffix=%s location=%s", (const char*)suffix, (const char*)location);
  if(suffix == ".zip") {
    Decode::ZIP archive;
    if(archive.open(location)) {
      for(auto& file : archive.file) {
        auto type = Location::suffix(file.name).downcase();
        if(type == ".sfc" || type == ".smc" || type == ".gb" || type == ".gbc" || type == ".bs" || type == ".st") {
          auto data = archive.extract(file);
          fastLinkDebug("Program::loadFile zip member=%s size=%u", (const char*)file.name, (uint)data.size());
          return data;
        }
      }
    }
    fastLinkDebug("Program::loadFile zip had no supported ROM");
    return {};
  } else if(suffix == ".7z") {
    auto data = LZMA::extract(location);
    fastLinkDebug("Program::loadFile 7z size=%u", (uint)data.size());
    return data;
  } else {
    auto data = file::read(location);
    fastLinkDebug("Program::loadFile raw size=%u", (uint)data.size());
    return data;
  }
}

auto Program::loadSuperFamicom(string location) -> bool {
  fastLinkCrashStep("Program::loadSuperFamicom: begin");
  fastLinkDebug("Program::loadSuperFamicom begin location=%s", (const char*)location);
  string manifest;
  vector<uint8_t> rom;

  if(location.endsWith("/")) {
    manifest = file::read({location, "manifest.bml"});
    rom.append(file::read({location, "program.rom"}));
    rom.append(file::read({location, "data.rom"}));
    rom.append(file::read({location, "expansion.rom"}));
    for(auto& filename : directory::files(location, "*.boot.rom"   )) rom.append(file::read({location, filename}));
    for(auto& filename : directory::files(location, "*.program.rom")) rom.append(file::read({location, filename}));
    for(auto& filename : directory::files(location, "*.data.rom"   )) rom.append(file::read({location, filename}));
  } else {
    manifest = file::read({Location::notsuffix(location), ".bml"});
    rom = loadFile(location);
  }
  fastLinkDebug("Program::loadSuperFamicom rom size=%u manifestBytes=%u", (uint)rom.size(), (uint)manifest.size());
  fastLinkCrashStep("Program::loadSuperFamicom: rom too small");
  if(rom.size() < 0x8000) {
    fastLinkDebug("Program::loadSuperFamicom failed: ROM too small");
    return false;
  }

  if((rom.size() & 0x7fff) == 512) {
    //remove copier header
    memory::move(&rom[0], &rom[512], rom.size() - 512);
    rom.resize(rom.size() - 512);
    fastLinkDebug("Program::loadSuperFamicom removed copier header newSize=%u", (uint)rom.size());
  }

  if(!superFamicom.patched) superFamicom.patched = applyPatchIPS(rom, location);
  if(!superFamicom.patched) superFamicom.patched = applyPatchBPS(rom, location);
  fastLinkDebug("Program::loadSuperFamicom patches applied=%d", superFamicom.patched);
  auto heuristics = Heuristics::SuperFamicom(rom, location);
  auto sha256 = Hash::SHA256(rom).digest();
  superFamicom.title = heuristics.title();
  superFamicom.region = heuristics.videoRegion();
  fastLinkDebug("Program::loadSuperFamicom heuristics title=%s region=%s", (const char*)superFamicom.title, (const char*)superFamicom.region);
  if(auto document = BML::unserialize(string::read(locate("Database/Super Famicom.bml")))) {
    if(auto game = document[{"game(sha256=", sha256, ")"}]) {
      manifest = BML::serialize(game);
      //the internal ROM header title is not present in the database, but is needed for internal core overrides
      manifest.append("  title: ", superFamicom.title, "\n");
      superFamicom.verified = true;
    }
  }
  fastLinkDebug("Program::loadSuperFamicom database verified=%d", superFamicom.verified);
  superFamicom.manifest = manifest ? manifest : heuristics.manifest();
  superFamicom.document = BML::unserialize(superFamicom.manifest);
  superFamicom.location = location;
  fastLinkDebug("Program::loadSuperFamicom manifest parsed bytes=%u", (uint)superFamicom.manifest.size());

  uint offset = 0;
  auto copySegment = [&](auto& target, uint size, const char* name) -> bool {
    if(!size) return true;
    if(offset > rom.size() || size > rom.size() - offset) {
      fastLinkDebug(
        "Program::loadSuperFamicom failed: %s size=%u offset=%u rom=%u",
        name,
        size,
        offset,
        (uint)rom.size()
      );
      return false;
    }

    target.resize(size);
    memory::copy(&target[0], &rom[offset], size);
    offset += size;
    return true;
  };

  if(auto size = heuristics.programRomSize()) {
    if(!copySegment(superFamicom.program, size, "program.rom")) return false;
  }
  if(auto size = heuristics.dataRomSize()) {
    if(!copySegment(superFamicom.data, size, "data.rom")) return false;
  }
  if(auto size = heuristics.expansionRomSize()) {
    if(!copySegment(superFamicom.expansion, size, "expansion.rom")) return false;
  }
  if(auto size = heuristics.firmwareRomSize()) {
    if(!copySegment(superFamicom.firmware, size, "firmware.rom")) return false;
  }

  fastLinkDebug(
    "Program::loadSuperFamicom done program=%u data=%u expansion=%u firmware=%u",
    (uint)superFamicom.program.size(),
    (uint)superFamicom.data.size(),
    (uint)superFamicom.expansion.size(),
    (uint)superFamicom.firmware.size()
  );
  fastLinkCrashStep("Program::loadSuperFamicom: complete");
  return true;
}

auto Program::loadGameBoy(string location) -> bool {
  fastLinkCrashStep("Program::loadGameBoy: begin");
  string manifest;
  vector<uint8_t> rom;

  if(location.endsWith("/")) {
    manifest = file::read({location, "manifest.bml"});
    rom.append(file::read({location, "program.rom"}));
  } else {
    manifest = file::read({Location::notsuffix(location), ".bml"});
    rom = loadFile(location);
  }
  if(rom.size() < 0x4000) return false;

  gameBoy.patched = applyPatchIPS(rom, location) || applyPatchBPS(rom, location);
  auto heuristics = Heuristics::GameBoy(rom, location);
  auto sha256 = Hash::SHA256(rom).digest();
  if(auto document = BML::unserialize(string::read(locate("Database/Game Boy.bml")))) {
    if(auto game = document[{"game(sha256=", sha256, ")"}]) {
      manifest = BML::serialize(game);
      gameBoy.verified = true;
    }
  }
  if(auto document = BML::unserialize(string::read(locate("Database/Game Boy Color.bml")))) {
    if(auto game = document[{"game(sha256=", sha256, ")"}]) {
      manifest = BML::serialize(game);
      gameBoy.verified = true;
    }
  }
  gameBoy.manifest = manifest ? manifest : heuristics.manifest();
  gameBoy.document = BML::unserialize(gameBoy.manifest);
  gameBoy.location = location;

  gameBoy.program = rom;
  fastLinkCrashStep("Program::loadGameBoy: complete");
  return true;
}

auto Program::loadBSMemory(string location) -> bool {
  fastLinkCrashStep("Program::loadBSMemory: begin");
  string manifest;
  vector<uint8_t> rom;

  if(location.endsWith("/")) {
    manifest = file::read({location, "manifest.bml"});
    rom.append(file::read({location, "program.rom"}));
    rom.append(file::read({location, "program.flash"}));
  } else {
    manifest = file::read({Location::notsuffix(location), ".bml"});
    rom = loadFile(location);
  }
  if(rom.size() < 0x8000) return false;

  bsMemory.patched = applyPatchIPS(rom, location) || applyPatchBPS(rom, location);
  auto heuristics = Heuristics::BSMemory(rom, location);
  auto sha256 = Hash::SHA256(rom).digest();
  if(auto document = BML::unserialize(string::read(locate("Database/BS Memory.bml")))) {
    if(auto game = document[{"game(sha256=", sha256, ")"}]) {
      manifest = BML::serialize(game);
      bsMemory.verified = true;
    }
  }
  bsMemory.manifest = manifest ? manifest : heuristics.manifest();
  bsMemory.document = BML::unserialize(bsMemory.manifest);
  bsMemory.location = location;

  bsMemory.program = rom;
  fastLinkCrashStep("Program::loadBSMemory: complete");
  return true;
}

auto Program::loadSufamiTurboA(string location) -> bool {
  fastLinkCrashStep("Program::loadSufamiTurboA: begin");
  string manifest;
  vector<uint8_t> rom;

  if(location.endsWith("/")) {
    manifest = file::read({location, "manifest.bml"});
    rom.append(file::read({location, "program.rom"}));
  } else {
    manifest = file::read({Location::notsuffix(location), ".bml"});
    rom = loadFile(location);
  }
  if(rom.size() < 0x20000) return false;

  sufamiTurboA.patched = applyPatchIPS(rom, location) || applyPatchBPS(rom, location);
  auto heuristics = Heuristics::SufamiTurbo(rom, location);
  auto sha256 = Hash::SHA256(rom).digest();
  if(auto document = BML::unserialize(string::read(locate("Database/Sufami Turbo.bml")))) {
    if(auto game = document[{"game(sha256=", sha256, ")"}]) {
      manifest = BML::serialize(game);
      sufamiTurboA.verified = true;
    }
  }
  sufamiTurboA.manifest = manifest ? manifest : heuristics.manifest();
  sufamiTurboA.document = BML::unserialize(sufamiTurboA.manifest);
  sufamiTurboA.location = location;

  sufamiTurboA.program = rom;
  fastLinkCrashStep("Program::loadSufamiTurboA: complete");
  return true;
}

auto Program::loadSufamiTurboB(string location) -> bool {
  fastLinkCrashStep("Program::loadSufamiTurboB: begin");
  string manifest;
  vector<uint8_t> rom;

  if(location.endsWith("/")) {
    manifest = file::read({location, "manifest.bml"});
    rom.append(file::read({location, "program.rom"}));
  } else {
    manifest = file::read({Location::notsuffix(location), ".bml"});
    rom = loadFile(location);
  }
  if(rom.size() < 0x20000) return false;

  sufamiTurboB.patched = applyPatchIPS(rom, location) || applyPatchBPS(rom, location);
  auto heuristics = Heuristics::SufamiTurbo(rom, location);
  auto sha256 = Hash::SHA256(rom).digest();
  if(auto document = BML::unserialize(string::read(locate("Database/Sufami Turbo.bml")))) {
    if(auto game = document[{"game(sha256=", sha256, ")"}]) {
      manifest = BML::serialize(game);
      sufamiTurboB.verified = true;
    }
  }
  sufamiTurboB.manifest = manifest ? manifest : heuristics.manifest();
  sufamiTurboB.document = BML::unserialize(sufamiTurboB.manifest);
  sufamiTurboB.location = location;

  sufamiTurboB.program = rom;
  fastLinkCrashStep("Program::loadSufamiTurboB: complete");
  return true;
}

auto Program::save() -> void {
  if(!emulator->loaded()) return;
  emulator->save();
}

auto Program::reset() -> void {
  if(!emulator->loaded()) {
    fastLinkDebug("Program::reset ignored: no game loaded");
    return;
  }
  fastLinkDebug("Program::reset begin");
  fastLinkDebug("Program::reset emulator->reset begin");
  emulator->reset();
  fastLinkDebug("Program::reset emulator->reset done");
  showMessage("Game reset");
  fastLinkDebug("Program::reset complete");
}

auto Program::power() -> void {
  if(!emulator->loaded()) {
    fastLinkDebug("Program::power ignored: no game loaded");
    return;
  }
  fastLinkDebug("Program::power begin");
  fastLinkDebug("Program::power emulator->power begin");
  emulator->power();
  fastLinkDebug("Program::power emulator->power done");
  showMessage("Power cycle");
  fastLinkDebug("Program::power complete");
}

auto Program::unload() -> void {
  if(!emulator->loaded()) {
    fastLinkDebug("Program::unload ignored: no game loaded");
    return;
  }
  fastLinkDebug("Program::unload begin title=%s", (const char*)emulator->title());
  fastLinkRAMTraceService.clearSnapshot();
  //todo: video.clear() is not working on macOS/OpenGL 3.2
  fastLinkDebug("Program::unload clear video begin");
  if(auto [output, length] = video.acquire(1, 1); output) {
    *output = 0;
    video.release();
    video.output();
  }
  fastLinkDebug("Program::unload clear audio begin");
  audio.clear();
  fastLinkDebug("Program::unload save cheats/tools begin");
  cheatEditor.saveCheats();
  toolsWindow.setVisible(false);
  if(emulatorSettings.autoSaveStateOnUnload.checked()) {
    fastLinkDebug("Program::unload auto save undo state begin");
    saveUndoState();
    fastLinkDebug("Program::unload auto save undo state done");
  }
  screenshot = {};
  fastLinkDebug("Program::unload emulator->unload begin");
  emulator->unload();
  fastLinkDebug("Program::unload emulator->unload done");
  showMessage("Game unloaded");
  superFamicom = {};
  gameBoy = {};
  bsMemory = {};
  sufamiTurboA = {};
  sufamiTurboB = {};
  presentation.setTitle({Emulator::Name, " v", Emulator::Version});
  presentation.resetSystem.setEnabled(true);
  presentation.unloadGame.setEnabled(false);
  presentation.toolsMenu.setVisible(false);
  presentation.updateProgramIcon();
  presentation.updateStatusIcon();
  fastLinkDebug("Program::unload complete");
}

//a game is considered verified if the game plus its slot(s) are found in the games database
auto Program::verified() const -> bool {
  if(!emulator->loaded()) return false;
  if(superFamicom && !superFamicom.verified) return false;
  if(gameBoy && !gameBoy.verified) return false;
  if(bsMemory && !bsMemory.verified) return false;
  if(sufamiTurboA && !sufamiTurboA.verified) return false;
  if(sufamiTurboB && !sufamiTurboB.verified) return false;
  return true;
}

auto Program::loadFile(string location) -> vector<uint8_t> {
  auto suffix = Location::suffix(location).downcase();
  if(suffix == ".zip") {
    Decode::ZIP archive;
    if(archive.open(location)) {
      for(auto& file : archive.file) {
        auto type = Location::suffix(file.name).downcase();
        if(type == ".sfc" || type == ".smc" || type == ".gb" || type == ".gbc" || type == ".bs" || type == ".st") {
          auto data = archive.extract(file);
          return data;
        }
      }
    }
    return {};
  } else if(suffix == ".7z") {
    auto data = LZMA::extract(location);
    return data;
  } else {
    auto data = file::read(location);
    return data;
  }
}

auto Program::loadSuperFamicom(string location) -> bool {
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
  if(rom.size() < 0x8000) {
    return false;
  }

  if((rom.size() & 0x7fff) == 512) {
    //remove copier header
    memory::move(&rom[0], &rom[512], rom.size() - 512);
    rom.resize(rom.size() - 512);
  }

  if(!superFamicom.patched) superFamicom.patched = applyPatchIPS(rom, location);
  if(!superFamicom.patched) superFamicom.patched = applyPatchBPS(rom, location);
  auto heuristics = Heuristics::SuperFamicom(rom, location);
  auto sha256 = Hash::SHA256(rom).digest();
  superFamicom.title = heuristics.title();
  superFamicom.region = heuristics.videoRegion();
  if(auto document = BML::unserialize(string::read(locate("Database/Super Famicom.bml")))) {
    if(auto game = document[{"game(sha256=", sha256, ")"}]) {
      manifest = BML::serialize(game);
      //the internal ROM header title is not present in the database, but is needed for internal core overrides
      manifest.append("  title: ", superFamicom.title, "\n");
      superFamicom.verified = true;
    }
  }
  superFamicom.manifest = manifest ? manifest : heuristics.manifest();
  superFamicom.document = BML::unserialize(superFamicom.manifest);
  superFamicom.location = location;

  uint offset = 0;
  auto copySegment = [&](auto& target, uint size, const char* name) -> bool {
    if(!size) return true;
    if(offset > rom.size() || size > rom.size() - offset) {
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

  return true;
}

auto Program::loadGameBoy(string location) -> bool {
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
  return true;
}

auto Program::loadBSMemory(string location) -> bool {
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
  return true;
}

auto Program::loadSufamiTurboA(string location) -> bool {
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
  return true;
}

auto Program::loadSufamiTurboB(string location) -> bool {
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
  return true;
}

auto Program::verified() const -> bool {
  if(!emulator->loaded()) return false;
  if(superFamicom && !superFamicom.verified) return false;
  if(gameBoy && !gameBoy.verified) return false;
  if(bsMemory && !bsMemory.verified) return false;
  if(sufamiTurboA && !sufamiTurboA.verified) return false;
  if(sufamiTurboB && !sufamiTurboB.verified) return false;
  return true;
}

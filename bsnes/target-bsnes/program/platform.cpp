#include <heuristics/heuristics.hpp>
#include <heuristics/heuristics.cpp>
#include <heuristics/super-famicom.cpp>
#include <heuristics/game-boy.cpp>
#include <heuristics/bs-memory.cpp>
#include <heuristics/sufami-turbo.cpp>

auto Program::open(uint id, string name, vfs::file::mode mode, bool required) -> shared_pointer<vfs::file> {
  shared_pointer<vfs::file> result;
  bool read = mode == vfs::file::mode::read;
  if(id == 0 && read) {
    if(name == "boards.bml") result = vfs::memory::file::open(Resource::System::Boards, sizeof(Resource::System::Boards));
    if(name == "ipl.rom") result = vfs::memory::file::open(Resource::System::IPLROM, sizeof(Resource::System::IPLROM));
  }
  if(id == 1) {
    if(name == "manifest.bml" && read) result = vfs::memory::file::open(superFamicom.manifest.data<uint8_t>(), superFamicom.manifest.size());
    else if(name == "program.rom" && read) result = vfs::memory::file::open(superFamicom.program.data(), superFamicom.program.size());
    else if(name == "data.rom" && read) result = vfs::memory::file::open(superFamicom.data.data(), superFamicom.data.size());
    else if(name == "expansion.rom" && read) result = vfs::memory::file::open(superFamicom.expansion.data(), superFamicom.expansion.size());
    else if(superFamicom.location.endsWith("/")) result = openPakSuperFamicom(name, mode);
    else result = openRomSuperFamicom(name, mode);
  }
  if(id == 2) {
    if(name == "manifest.bml" && read) result = vfs::memory::file::open(gameBoy.manifest.data<uint8_t>(), gameBoy.manifest.size());
    else if(name == "program.rom" && read) result = vfs::memory::file::open(gameBoy.program.data(), gameBoy.program.size());
    else if(gameBoy.location.endsWith("/")) result = openPakGameBoy(name, mode);
    else result = openRomGameBoy(name, mode);
  }
  if(id == 3) {
    if(name == "manifest.bml" && read) result = vfs::memory::file::open(bsMemory.manifest.data<uint8_t>(), bsMemory.manifest.size());
    else if(name == "program.rom" && read) result = vfs::memory::file::open(bsMemory.program.data(), bsMemory.program.size());
    else if(name == "program.flash") result = vfs::memory::file::open(bsMemory.program.data(), bsMemory.program.size());
    else if(bsMemory.location.endsWith("/")) result = openPakBSMemory(name, mode);
    else result = openRomBSMemory(name, mode);
  }
  if(id == 4) {
    if(name == "manifest.bml" && read) result = vfs::memory::file::open(sufamiTurboA.manifest.data<uint8_t>(), sufamiTurboA.manifest.size());
    else if(name == "program.rom" && read) result = vfs::memory::file::open(sufamiTurboA.program.data(), sufamiTurboA.program.size());
    else if(sufamiTurboA.location.endsWith("/")) result = openPakSufamiTurboA(name, mode);
    else result = openRomSufamiTurboA(name, mode);
  }
  if(id == 5) {
    if(name == "manifest.bml" && read) result = vfs::memory::file::open(sufamiTurboB.manifest.data<uint8_t>(), sufamiTurboB.manifest.size());
    else if(name == "program.rom" && read) result = vfs::memory::file::open(sufamiTurboB.program.data(), sufamiTurboB.program.size());
    else if(sufamiTurboB.location.endsWith("/")) result = openPakSufamiTurboB(name, mode);
    else result = openRomSufamiTurboB(name, mode);
  }
  if(!result && required) nativeError({"Missing required data: ", name});
  return result;
}

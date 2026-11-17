auto Program::decodeSNES(string& code) -> bool {
  //Game Genie
  if(code.size() == 9 && code[4u] == '-') {
    //strip '-'
    code = {code.slice(0, 4), code.slice(5, 4)};
    //validate
    for(uint n : code) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      return false;
    }
    //decode
    code.transform("df4709156bc8a23e", "0123456789abcdef");
    uint32_t r = toHex(code);
    //abcd efgh ijkl mnop qrst uvwx
    //ijkl qrst opab cduv wxef ghmn
    uint address =
      (!!(r & 0x002000) << 23) | (!!(r & 0x001000) << 22)
    | (!!(r & 0x000800) << 21) | (!!(r & 0x000400) << 20)
    | (!!(r & 0x000020) << 19) | (!!(r & 0x000010) << 18)
    | (!!(r & 0x000008) << 17) | (!!(r & 0x000004) << 16)
    | (!!(r & 0x800000) << 15) | (!!(r & 0x400000) << 14)
    | (!!(r & 0x200000) << 13) | (!!(r & 0x100000) << 12)
    | (!!(r & 0x000002) << 11) | (!!(r & 0x000001) << 10)
    | (!!(r & 0x008000) <<  9) | (!!(r & 0x004000) <<  8)
    | (!!(r & 0x080000) <<  7) | (!!(r & 0x040000) <<  6)
    | (!!(r & 0x020000) <<  5) | (!!(r & 0x010000) <<  4)
    | (!!(r & 0x000200) <<  3) | (!!(r & 0x000100) <<  2)
    | (!!(r & 0x000080) <<  1) | (!!(r & 0x000040) <<  0);
    uint data = r >> 24;
    code = {hex(address, 6L), "=", hex(data, 2L)};
    return true;
  }

  //Pro Action Replay
  if(code.size() == 8) {
    //validate
    for(uint n : code) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      return false;
    }
    //decode
    uint32_t r = toHex(code);
    uint address = r >> 8;
    uint data = r & 0xff;
    code = {hex(address, 6L), "=", hex(data, 2L)};
    return true;
  }

  //higan: address=data
  if(code.size() == 9 && code[6u] == '=') {
    string nibbles = {code.slice(0, 6), code.slice(7, 2)};
    //validate
    for(uint n : nibbles) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      return false;
    }
    //already in decoded form
    return true;
  }

  //higan: address=compare?data
  if(code.size() == 12 && code[6u] == '=' && code[9u] == '?') {
    string nibbles = {code.slice(0, 6), code.slice(7, 2), code.slice(10, 2)};
    //validate
    for(uint n : nibbles) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      return false;
    }
    //already in decoded form
    return true;
  }

  //unrecognized code format
  return false;
}

auto Program::decodeGB(string& code) -> bool {
  auto nibble = [&](const string& s, uint index) -> uint {
    if(index >= s.size()) return 0;
    if(s[index] >= '0' && s[index] <= '9') return s[index] - '0';
    return s[index] - 'a' + 10;
  };

  //Game Genie
  if(code.size() == 7 && code[3u] == '-') {
    code = {code.slice(0, 3), code.slice(4, 3)};
    //validate
    for(uint n : code) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      return false;
    }
    uint data = nibble(code, 0) << 4 | nibble(code, 1) << 0;
    uint address = (nibble(code, 5) ^ 15) << 12 | nibble(code, 2) << 8 | nibble(code, 3) << 4 | nibble(code, 4) << 0;
    code = {hex(address, 4L), "=", hex(data, 2L)};
    return true;
  }

  //Game Genie
  if(code.size() == 11 && code[3u] == '-' && code[7u] == '-') {
    code = {code.slice(0, 3), code.slice(4, 3), code.slice(8, 3)};
    //validate
    for(uint n : code) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      return false;
    }
    uint data = nibble(code, 0) << 4 | nibble(code, 1) << 0;
    uint address = (nibble(code, 5) ^ 15) << 12 | nibble(code, 2) << 8 | nibble(code, 3) << 4 | nibble(code, 4) << 0;
    uint8_t t = nibble(code, 6) << 4 | nibble(code, 8) << 0;
    t = t >> 2 | t << 6;
    uint compare = t ^ 0xba;
    code = {hex(address, 4L), "=", hex(compare, 2L), "?", hex(data, 2L)};
    return true;
  }

  //GameShark
  if(code.size() == 8) {
    //validate
    for(uint n : code) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      return false;
    }
    //first two characters are the code type / VRAM bank, which is almost always 01.
    //other values are presumably supported, but I have no info on them, so they're not supported.
    if(code[0u] != '0') return false;
    if(code[1u] != '1') return false;
    uint data = toHex(code.slice(2, 2));
    uint16_t address = toHex(code.slice(4, 4));
    address = address >> 8 | address << 8;
    code = {hex(address, 4L), "=", hex(data, 2L)};
    return true;
  }

  //higan: address=data
  if(code.size() == 7 && code[4u] == '=') {
    string nibbles = {code.slice(0, 4), code.slice(5, 2)};
    //validate
    for(uint n : nibbles) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      return false;
    }
    //already in decoded form
    return true;
  }

  //higan: address=compare?data
  if(code.size() == 10 && code[4u] == '=' && code[7u] == '?') {
    string nibbles = {code.slice(0, 4), code.slice(5, 2), code.slice(8, 2)};
    //validate
    for(uint n : nibbles) {
      if(n >= '0' && n <= '9') continue;
      if(n >= 'a' && n <= 'f') continue;
      return false;
    }
    //already in decoded form
    return true;
  }

  //unrecognized code format
  return false;
}

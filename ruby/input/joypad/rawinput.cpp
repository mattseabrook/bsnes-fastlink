#pragma once

#include <hidsdi.h>

struct InputJoypadRawInput {
  Input& input;
  InputJoypadRawInput(Input& input) : input(input) {}

  struct Axis {
    USAGE usage = 0;
    LONG logicalMin = 0;
    LONG logicalMax = 0;
    USHORT bitSize = 0;
    uint inputID = 0;
  };

  struct Hat {
    LONG logicalMin = 0;
    LONG logicalMax = 0;
    USHORT bitSize = 0;
    uint inputID = 0;
  };

  struct Joypad {
    shared_pointer<HID::Joypad> hid{new HID::Joypad};

    HANDLE handle = nullptr;
    string path;
    vector<uint8_t> preparsed;
    HIDP_CAPS caps = {};

    vector<USHORT> buttonDataIndices;
    vector<int16_t> buttonStates;
    vector<Axis> axes;
    vector<int16_t> axisStates;
    vector<Hat> hats;
    vector<int16_t> hatStates;
  };

  vector<Joypad> joypads;
  uint64_t lastSync = 0;

  auto assign(shared_pointer<HID::Joypad> hid, uint groupID, uint inputID, int16_t value) -> void {
    auto& group = hid->group(groupID);
    if(group.input(inputID).value() == value) return;
    input.doChange(hid, groupID, inputID, group.input(inputID).value(), value);
    group.input(inputID).setValue(value);
  }

  auto poll(vector<shared_pointer<HID::Device>>& devices) -> void {
    synchronize();

    for(auto& jp : joypads) {
      for(auto n : range(jp.axisStates.size())) {
        assign(jp.hid, HID::Joypad::GroupID::Axis, n, jp.axisStates[n]);
      }

      for(auto n : range(jp.hatStates.size())) {
        assign(jp.hid, HID::Joypad::GroupID::Hat, n, jp.hatStates[n]);
      }

      for(auto n : range(jp.buttonStates.size())) {
        assign(jp.hid, HID::Joypad::GroupID::Button, n, jp.buttonStates[n]);
      }

      devices.append(jp.hid);
    }
  }

  auto initialize() -> bool {
    rawinput.updateJoypad = {&InputJoypadRawInput::update, this};
    synchronize(true);
    return true;
  }

  auto terminate() -> void {
    rawinput.updateJoypad.reset();
    joypads.reset();
    lastSync = 0;
  }

private:
  static auto axisName(USAGE usage) -> string {
    if(usage == 0x30) return "X";
    if(usage == 0x31) return "Y";
    if(usage == 0x32) return "Z";
    if(usage == 0x33) return "Rx";
    if(usage == 0x34) return "Ry";
    if(usage == 0x35) return "Rz";
    if(usage == 0x36) return "Slider";
    if(usage == 0x37) return "Dial";
    if(usage == 0x38) return "Wheel";
    return {"Axis ", (uint)usage};
  }

  static auto decodeSignedValue(ULONG value, LONG logicalMin, USHORT bitSize) -> LONG {
    LONG signedValue = (LONG)value;
    if(logicalMin < 0 && bitSize > 0 && bitSize < 32 && (value & (1u << (bitSize - 1)))) {
      signedValue = (LONG)(value | (~0u << bitSize));
    }
    return signedValue;
  }

  static auto normalizeAxis(ULONG value, LONG logicalMin, LONG logicalMax, USHORT bitSize) -> int16_t {
    if(logicalMax <= logicalMin) return 0;

    auto signedValue = decodeSignedValue(value, logicalMin, bitSize);
    if(signedValue < logicalMin) signedValue = logicalMin;
    if(signedValue > logicalMax) signedValue = logicalMax;

    int64_t scaled = ((int64_t)(signedValue - logicalMin) * 65535 / (logicalMax - logicalMin)) - 32768;
    if(scaled < -32768) scaled = -32768;
    if(scaled > +32767) scaled = +32767;
    return (int16_t)scaled;
  }

  static auto decodeHat(LONG value, LONG logicalMin, LONG logicalMax, int16_t& x, int16_t& y) -> void {
    x = 0;
    y = 0;
    if(value < logicalMin || value > logicalMax) return;

    switch((ULONG)(value - logicalMin)) {
    case 0: y = -32767; break;
    case 1: x = +32767, y = -32767; break;
    case 2: x = +32767; break;
    case 3: x = +32767, y = +32767; break;
    case 4: y = +32767; break;
    case 5: x = -32767, y = +32767; break;
    case 6: x = -32767; break;
    case 7: x = -32767, y = -32767; break;
    }
  }

  auto findJoypad(HANDLE handle) -> Joypad* {
    for(auto& jp : joypads) {
      if(jp.handle == handle) return &jp;
    }
    return nullptr;
  }

  auto update(RAWINPUT* input) -> void {
    if(input->header.dwType != RIM_TYPEHID) return;

    auto joypad = findJoypad(input->header.hDevice);
    if(!joypad) {
      synchronize(true);
      joypad = findJoypad(input->header.hDevice);
      if(!joypad) return;
    }

    updateReport(*joypad, input->data.hid);
  }

  auto updateReport(Joypad& joypad, const RAWHID& hid) -> void {
    auto reportLength = hid.dwSizeHid;
    if(!reportLength || !hid.dwCount) return;

    auto report = hid.bRawData + (hid.dwCount - 1) * reportLength;
    auto ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(joypad.preparsed.data());
    if(!ppd) return;

    for(auto n : range(joypad.buttonStates.size())) joypad.buttonStates[n] = 0;

    auto maxData = HidP_MaxDataListLength(HidP_Input, ppd);
    if(maxData > 0 && joypad.buttonDataIndices) {
      vector<HIDP_DATA> dataList;
      dataList.resize(maxData);
      ULONG dataLength = maxData;
      if(HidP_GetData(HidP_Input, dataList.data(), &dataLength, ppd, (PCHAR)report, reportLength) == HIDP_STATUS_SUCCESS) {
        for(auto n : range(dataLength)) {
          if(auto index = joypad.buttonDataIndices.findSorted((USHORT)dataList[n].DataIndex)) {
            if(index() < joypad.buttonStates.size() && dataList[n].On) joypad.buttonStates[index()] = 1;
          }
        }
      }
    }

    for(auto& axis : joypad.axes) {
      ULONG value = 0;
      joypad.axisStates[axis.inputID] = 0;
      if(HidP_GetUsageValue(HidP_Input, 0x01, 0, axis.usage, &value, ppd, (PCHAR)report, reportLength) != HIDP_STATUS_SUCCESS) continue;
      joypad.axisStates[axis.inputID] = normalizeAxis(value, axis.logicalMin, axis.logicalMax, axis.bitSize);
    }

    for(auto& hat : joypad.hats) {
      ULONG value = 0;
      int16_t x = 0;
      int16_t y = 0;
      joypad.hatStates[hat.inputID + 0] = 0;
      joypad.hatStates[hat.inputID + 1] = 0;
      if(HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x39, &value, ppd, (PCHAR)report, reportLength) != HIDP_STATUS_SUCCESS) continue;
      auto signedValue = decodeSignedValue(value, hat.logicalMin, hat.bitSize);
      decodeHat(signedValue, hat.logicalMin, hat.logicalMax, x, y);
      joypad.hatStates[hat.inputID + 0] = x;
      joypad.hatStates[hat.inputID + 1] = y;
    }
  }

  auto initializeJoypad(const RawInput::Device& device, Joypad& joypad) -> bool {
    UINT ppdSize = 0;
    if(GetRawInputDeviceInfo(device.handle, RIDI_PREPARSEDDATA, nullptr, &ppdSize) == (UINT)-1 || !ppdSize) return false;

    joypad.preparsed.resize(ppdSize);
    if(GetRawInputDeviceInfo(device.handle, RIDI_PREPARSEDDATA, joypad.preparsed.data(), &ppdSize) == (UINT)-1) return false;

    auto ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(joypad.preparsed.data());
    if(HidP_GetCaps(ppd, &joypad.caps) != HIDP_STATUS_SUCCESS) return false;

    joypad.handle = device.handle;
    joypad.path = device.path;
    joypad.hid->setVendorID(device.vendorID);
    joypad.hid->setProductID(device.productID);
    joypad.hid->setPathID(Hash::CRC32(device.path).value());

    USHORT buttonCapsLength = joypad.caps.NumberInputButtonCaps;
    if(buttonCapsLength) {
      vector<HIDP_BUTTON_CAPS> buttonCaps;
      buttonCaps.resize(buttonCapsLength);
      if(HidP_GetButtonCaps(HidP_Input, buttonCaps.data(), &buttonCapsLength, ppd) == HIDP_STATUS_SUCCESS) {
        for(auto n : range(buttonCapsLength)) {
          if(buttonCaps[n].UsagePage != 0x09) continue;
          if(buttonCaps[n].IsRange) {
            for(USHORT dataIndex = buttonCaps[n].Range.DataIndexMin; dataIndex <= buttonCaps[n].Range.DataIndexMax; dataIndex++) {
              joypad.buttonDataIndices.append(dataIndex);
            }
          } else {
            joypad.buttonDataIndices.append(buttonCaps[n].NotRange.DataIndex);
          }
        }
      }
    }

    joypad.buttonDataIndices.sort();
    vector<USHORT> uniqueDataIndices;
    for(auto dataIndex : joypad.buttonDataIndices) {
      if(uniqueDataIndices && uniqueDataIndices.right() == dataIndex) continue;
      uniqueDataIndices.append(dataIndex);
    }
    joypad.buttonDataIndices = uniqueDataIndices;
    for(auto n : range(joypad.buttonDataIndices.size())) {
      joypad.buttonStates.append(0);
      joypad.hid->buttons().append({"Button ", 1 + n});
    }

    USHORT valueCapsLength = joypad.caps.NumberInputValueCaps;
    if(valueCapsLength) {
      vector<HIDP_VALUE_CAPS> valueCaps;
      valueCaps.resize(valueCapsLength);
      if(HidP_GetValueCaps(HidP_Input, valueCaps.data(), &valueCapsLength, ppd) == HIDP_STATUS_SUCCESS) {
        for(auto n : range(valueCapsLength)) {
          if(valueCaps[n].UsagePage != 0x01) continue;

          auto usageMin = valueCaps[n].IsRange ? valueCaps[n].Range.UsageMin : valueCaps[n].NotRange.Usage;
          auto usageMax = valueCaps[n].IsRange ? valueCaps[n].Range.UsageMax : valueCaps[n].NotRange.Usage;

          for(USAGE usage = usageMin; usage <= usageMax; usage++) {
            if(usage == 0x39) {
              auto hatID = joypad.hid->hats().size();
              joypad.hats.append({valueCaps[n].LogicalMin, valueCaps[n].LogicalMax, valueCaps[n].BitSize, hatID});
              joypad.hid->hats().append({"Hat ", 1 + joypad.hats.size(), " X"});
              joypad.hid->hats().append({"Hat ", 1 + joypad.hats.size(), " Y"});
              joypad.hatStates.append(0);
              joypad.hatStates.append(0);
              continue;
            }

            auto axisID = joypad.hid->axes().size();
            joypad.axes.append({usage, valueCaps[n].LogicalMin, valueCaps[n].LogicalMax, valueCaps[n].BitSize, axisID});
            joypad.hid->axes().append(axisName(usage));
            joypad.axisStates.append(0);
          }
        }
      }
    }

    return joypad.buttonStates || joypad.axisStates || joypad.hatStates;
  }

  auto synchronize(bool force = false) -> void {
    auto now = GetTickCount64();
    if(!force && now - lastSync < 250) return;
    lastSync = now;

    vector<RawInput::Device> available;
    if(rawinput.mutex) {
      WaitForSingleObject(rawinput.mutex, INFINITE);
      for(auto& device : rawinput.devices) {
        if(device.type != RawInput::Device::Type::Joypad) continue;
        available.append(device);
      }
      ReleaseMutex(rawinput.mutex);
    }

    bool changed = available.size() != joypads.size();
    if(!changed) {
      for(auto n : range(available.size())) {
        if(available[n].handle != joypads[n].handle || available[n].path != joypads[n].path) {
          changed = true;
          break;
        }
      }
    }
    if(!changed) return;

    vector<Joypad> rebuilt;
    for(auto& device : available) {
      Joypad joypad;
      if(initializeJoypad(device, joypad)) rebuilt.append(joypad);
    }
    joypads = rebuilt;
  }
};

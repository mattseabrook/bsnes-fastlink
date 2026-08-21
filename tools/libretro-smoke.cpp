#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <string>
#include <vector>

#include "../bsnes/target-libretro/libretro.h"

namespace {

retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
std::vector<uint8_t> frame;
unsigned frameWidth = 0;
unsigned frameHeight = 0;
size_t framePitch = 0;

void logMessage(enum retro_log_level, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stderr, format, arguments);
  va_end(arguments);
}

bool environment(unsigned command, void* data) {
  switch(command) {
  case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    *(bool*)data = true;
    return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *(const char**)data = "/tmp";
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    pixelFormat = *(retro_pixel_format*)data;
    return pixelFormat == RETRO_PIXEL_FORMAT_0RGB1555
        || pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888
        || pixelFormat == RETRO_PIXEL_FORMAT_RGB565;
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    auto& variable = *(retro_variable*)data;
    variable.value = nullptr;
    return false;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *(bool*)data = false;
    return true;
  case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
  case RETRO_ENVIRONMENT_SET_VARIABLES:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
    return true;
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    ((retro_log_callback*)data)->log = logMessage;
    return true;
  case RETRO_ENVIRONMENT_GET_LANGUAGE:
    *(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    return true;
  case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
    *(unsigned*)data = 0;
    return true;
  default:
    return false;
  }
}

void video(const void* data, unsigned width, unsigned height, size_t pitch) {
  if(!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;
  frameWidth = width;
  frameHeight = height;
  framePitch = pitch;
  frame.resize(height * pitch);
  std::memcpy(frame.data(), data, frame.size());
}

void audio(int16_t, int16_t) {}
size_t audioBatch(const int16_t*, size_t frames) { return frames; }
void inputPoll() {}
int16_t inputState(unsigned, unsigned, unsigned, unsigned) { return 0; }

template<typename Function>
Function symbol(void* library, const char* name) {
  auto address = dlsym(library, name);
  if(!address) {
    std::fprintf(stderr, "missing libretro symbol: %s\n", name);
    std::exit(2);
  }
  return reinterpret_cast<Function>(address);
}

uint8_t expand5(uint8_t value) {
  return (value << 3) | (value >> 2);
}

bool writePPM(const std::string& path) {
  if(frame.empty()) return false;
  std::ofstream output(path, std::ios::binary);
  if(!output) return false;
  output << "P6\n" << frameWidth << " " << frameHeight << "\n255\n";
  for(unsigned y = 0; y < frameHeight; y++) {
    auto row = frame.data() + y * framePitch;
    for(unsigned x = 0; x < frameWidth; x++) {
      uint8_t rgb[3] = {};
      if(pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        auto color = ((const uint32_t*)row)[x];
        rgb[0] = color >> 16;
        rgb[1] = color >> 8;
        rgb[2] = color;
      } else {
        auto color = ((const uint16_t*)row)[x];
        if(pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
          rgb[0] = expand5(color >> 11);
          auto green = (uint8_t)(color >> 5 & 0x3f);
          rgb[1] = (green << 2) | (green >> 4);
          rgb[2] = expand5(color);
        } else {
          rgb[0] = expand5(color >> 10);
          rgb[1] = expand5(color >> 5);
          rgb[2] = expand5(color);
        }
      }
      output.write((const char*)rgb, sizeof(rgb));
    }
  }
  return output.good();
}

}  // namespace

int main(int argc, char** argv) {
  if(argc < 3 || argc > 5) {
    std::fprintf(stderr, "usage: %s CORE ROM [FRAMES=3600] [OUTPUT.ppm]\n", argv[0]);
    return 2;
  }

  unsigned frames = argc >= 4 ? std::strtoul(argv[3], nullptr, 10) : 3600;
  std::string outputPath = argc >= 5 ? argv[4] : "/tmp/libretro-smoke.ppm";

  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if(!library) {
    std::fprintf(stderr, "cannot load core: %s\n", dlerror());
    return 2;
  }

  auto setEnvironment = symbol<decltype(&retro_set_environment)>(library, "retro_set_environment");
  auto setVideo = symbol<decltype(&retro_set_video_refresh)>(library, "retro_set_video_refresh");
  auto setAudio = symbol<decltype(&retro_set_audio_sample)>(library, "retro_set_audio_sample");
  auto setAudioBatch = symbol<decltype(&retro_set_audio_sample_batch)>(library, "retro_set_audio_sample_batch");
  auto setInputPoll = symbol<decltype(&retro_set_input_poll)>(library, "retro_set_input_poll");
  auto setInputState = symbol<decltype(&retro_set_input_state)>(library, "retro_set_input_state");
  auto init = symbol<decltype(&retro_init)>(library, "retro_init");
  auto deinit = symbol<decltype(&retro_deinit)>(library, "retro_deinit");
  auto getSystemInfo = symbol<decltype(&retro_get_system_info)>(library, "retro_get_system_info");
  auto loadGame = symbol<decltype(&retro_load_game)>(library, "retro_load_game");
  auto unloadGame = symbol<decltype(&retro_unload_game)>(library, "retro_unload_game");
  auto run = symbol<decltype(&retro_run)>(library, "retro_run");

  setEnvironment(environment);
  setVideo(video);
  setAudio(audio);
  setAudioBatch(audioBatch);
  setInputPoll(inputPoll);
  setInputState(inputState);
  init();

  retro_system_info systemInfo{};
  getSystemInfo(&systemInfo);
  std::ifstream romFile(argv[2], std::ios::binary);
  std::vector<char> rom((std::istreambuf_iterator<char>(romFile)), {});
  if(rom.empty()) {
    std::fprintf(stderr, "cannot read ROM: %s\n", argv[2]);
    deinit();
    dlclose(library);
    return 2;
  }

  retro_game_info game{};
  game.path = argv[2];
  if(!systemInfo.need_fullpath) {
    game.data = rom.data();
    game.size = rom.size();
  }
  if(!loadGame(&game)) {
    std::fprintf(stderr, "core rejected ROM: %s\n", argv[2]);
    deinit();
    dlclose(library);
    return 1;
  }

  for(unsigned frameNumber = 0; frameNumber < frames; frameNumber++) run();
  bool wroteFrame = writePPM(outputPath);
  std::printf("core=%s version=%s frames=%u video=%ux%u format=%u output=%s\n",
    systemInfo.library_name, systemInfo.library_version, frames, frameWidth,
    frameHeight, (unsigned)pixelFormat, wroteFrame ? outputPath.c_str() : "none");

  unloadGame();
  deinit();
  dlclose(library);
  return wroteFrame ? 0 : 1;
}

auto Program::captureScreenshot() -> bool {
  if(emulator->loaded() && screenshot.data) {
    if(auto filename = screenshotPath()) {
      //RGB555 -> RGB888
      image capture{0, 16, 0x8000, 0x7c00, 0x03e0, 0x001f};
      capture.copy(screenshot.data, screenshot.pitch, screenshot.width, screenshot.height);
      capture.transform(0, 32, 0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff);

      //normalize pixel aspect ratio to 1:1
      if(capture.width() == 512 && capture.height() == 240) capture.scale(512, 480, false);  //hires
      if(capture.width() == 256 && capture.height() == 480) capture.scale(512, 480, false);  //interlace

      auto data = capture.data();
      auto pitch = capture.pitch();
      auto width = capture.width();
      auto height = capture.height();

      if(!settings.video.overscan) {
        if(height == 240) data +=  8 * pitch, height -= 16;
        if(height == 480) data += 16 * pitch, height -= 32;
      }

      if(Encode::BMP::create(filename, data, width << 2, width, height, /* alpha = */ false)) {
        showMessage({"Captured screenshot [", Location::file(filename), "]"});
        return true;
      }
    }
  }
  return false;
}


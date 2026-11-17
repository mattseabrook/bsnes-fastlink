#pragma once
#include <cstdint>

struct FastLinkVideoSize { int width,height; };

inline auto fastLinkResizeVideo(int width,int height,bool heightDriven,bool overscan) -> FastLinkVideoSize {
  const int nativeHeight=overscan?240:224;
  int64_t requested=heightDriven?(int64_t(height)*256+nativeHeight/2)/nativeHeight:width;
  if(requested<256)requested=256;
  int64_t scale=(requested+128)/256;
  int64_t difference=requested-scale*256;
  if(difference>=-10 && difference<=10)return {int(scale*256),int(scale*nativeHeight)};
  // Keep the source-pixel width even, independent of window chrome/DPI.
  requested=(requested+1)&~int64_t(1);
  return {int(requested),int((requested*nativeHeight+128)/256)};
}

inline auto fastLinkFitVideo(int width,int height,bool overscan) -> FastLinkVideoSize {
  if(width<=0 || height<=0)return {0,0};
  const int nativeHeight=overscan?240:224;
  if(int64_t(width)*nativeHeight<=int64_t(height)*256)
    return {width,int((int64_t(width)*nativeHeight+128)/256)};
  return {int((int64_t(height)*256+nativeHeight/2)/nativeHeight),height};
}

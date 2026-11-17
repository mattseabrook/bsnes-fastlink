#include "../bsnes.hpp"

#if defined(PLATFORM_WINDOWS)

#define VK_USE_PLATFORM_WIN32_KHR
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstring>
#include <vulkan/vulkan.h>
#include "../fastlink/video-frames.hpp"
#include "../fastlink/video-size.hpp"

namespace {
auto CALLBACK NativeVideoWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
  if(message == WM_COMMAND || message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CLOSE) {
    return SendMessageW(mainWindow, message, wparam, lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

auto CALLBACK NativeMonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT lprcMonitor, LPARAM dwData) -> BOOL {
  auto& monitors = *(vector<NativeVideoManager::Monitor>*)dwData;

  MONITORINFOEXW info = {};
  info.cbSize = sizeof(MONITORINFOEXW);
  if(!GetMonitorInfoW(hMonitor, &info)) return true;

  string deviceName = (const char*)utf8_t(info.szDevice);
  if(deviceName.beginsWith(R"(\\.\DISPLAYV)")) return true;

  DISPLAY_DEVICEW display = {};
  display.cb = sizeof(DISPLAY_DEVICEW);
  EnumDisplayDevicesW(info.szDevice, 0, &display, 0);

  NativeVideoManager::Monitor monitor;
  string displayName = (const char*)utf8_t(display.DeviceString);
  monitor.name = {1 + monitors.size(), ": ", displayName ? displayName : deviceName};
  monitor.primary = info.dwFlags & MONITORINFOF_PRIMARY;
  monitor.x = lprcMonitor->left;
  monitor.y = lprcMonitor->top;
  monitor.width = lprcMonitor->right - lprcMonitor->left;
  monitor.height = lprcMonitor->bottom - lprcMonitor->top;
  monitors.append(monitor);
  return true;
}

auto chooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& capabilities) -> VkCompositeAlphaFlagBitsKHR {
  constexpr VkCompositeAlphaFlagBitsKHR candidates[] = {
    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
  };

  for(auto candidate : candidates) {
    if(capabilities.supportedCompositeAlpha & candidate) return candidate;
  }

  return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

auto hasPresentMode(const VkPresentModeKHR* presentModes, uint32_t count, VkPresentModeKHR mode) -> bool {
  for(uint32_t index = 0; index < count; index++) {
    if(presentModes[index] == mode) return true;
  }
  return false;
}

auto requestedPresentMode(string name) -> VkPresentModeKHR {
  if(name == "Immediate") return VK_PRESENT_MODE_IMMEDIATE_KHR;
  if(name == "Mailbox") return VK_PRESENT_MODE_MAILBOX_KHR;
  return VK_PRESENT_MODE_FIFO_KHR;
}

auto choosePresentMode(const VkPresentModeKHR* presentModes, uint32_t count, string requested) -> VkPresentModeKHR {
  auto preferred = requestedPresentMode(requested);
  if(hasPresentMode(presentModes, count, preferred)) return preferred;
  // FIFO is guaranteed by Vulkan and is the only safe fallback for an
  // explicitly requested presentation mode. Never introduce tearing when a
  // user's requested mode is unavailable.
  return VK_PRESENT_MODE_FIFO_KHR;
}
}

struct NativeVideoImplementation {
  explicit NativeVideoImplementation(NativeVideoManager& owner) : owner(owner) {}
  ~NativeVideoImplementation() { terminate(); }

  auto initialize() -> bool {
    terminate();

    auto hwnd = (HWND)owner.context();
    if(owner.fullScreen()) {
      auto monitors = owner.hasMonitors();
      NativeVideoManager::Monitor selected;
      bool found = false;
      for(auto& monitor : monitors) {
        if(monitor.name == owner.monitor()) {
          selected = monitor;
          found = true;
          break;
        }
      }
      if(!found && monitors) {
        selected = monitors.first();
        found = true;
      }
      if(!found) return false;

      WNDCLASSW windowClass = {};
      windowClass.hInstance = GetModuleHandle(nullptr);
      windowClass.lpfnWndProc = NativeVideoWindowProc;
      windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
      windowClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
      windowClass.lpszClassName = L"BsnesFastlinkVulkanFullscreen";
      if(!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

      utf16_t windowTitle(string{Emulator::Name, " v", Emulator::Version});
      _fullScreenWindow = CreateWindowExW(
        WS_EX_TOPMOST,
        windowClass.lpszClassName,
        windowTitle,
        WS_VISIBLE | WS_POPUP,
        selected.x, selected.y, selected.width, selected.height,
        nullptr, nullptr, windowClass.hInstance, nullptr
      );
      if(!_fullScreenWindow) return false;
      hwnd = _fullScreenWindow;
      POINT point = {selected.x + selected.width / 2, selected.y + selected.height / 2};
      _monitorHandle = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
      SetForegroundWindow(hwnd);
      SetFocus(hwnd);
    } else {
      _monitorHandle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }
    if(!hwnd || !IsWindow(hwnd)) return false;

    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "bsnes-fastlink";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    uint32_t instanceExtensionCount=0;
    vkEnumerateInstanceExtensionProperties(nullptr,&instanceExtensionCount,nullptr);
    vector<VkExtensionProperties> availableInstanceExtensions;
    availableInstanceExtensions.resize(instanceExtensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr,&instanceExtensionCount,availableInstanceExtensions.data());
    bool surfaceCapabilities2=false,surfaceMaintenance=false;
    for(auto& extension:availableInstanceExtensions) {
      if(!strcmp(extension.extensionName,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME))surfaceCapabilities2=true;
      if(!strcmp(extension.extensionName,VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME))surfaceMaintenance=true;
    }
    const char* instanceExtensions[4] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    uint32_t enabledInstanceExtensions=2;
    if(surfaceCapabilities2)instanceExtensions[enabledInstanceExtensions++]=VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
    if(surfaceCapabilities2&&surfaceMaintenance)instanceExtensions[enabledInstanceExtensions++]=VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;

    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = enabledInstanceExtensions;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    auto result = vkCreateInstance(&instanceInfo, nullptr, &_instance);
    if(result != VK_SUCCESS) return false;

    VkWin32SurfaceCreateInfoKHR surfaceInfo = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    surfaceInfo.hwnd = hwnd;
    result = vkCreateWin32SurfaceKHR(_instance, &surfaceInfo, nullptr, &_surface);
    if(result != VK_SUCCESS) return false;

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr);
    if(!deviceCount) return false;

    VkPhysicalDevice devices[16] = {};
    if(deviceCount > 16) deviceCount = 16;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, devices);

    for(uint32_t deviceIndex = 0; deviceIndex < deviceCount; deviceIndex++) {
      auto candidate = devices[deviceIndex];

      uint32_t extensionCount = 0;
      vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr);
      if(!extensionCount) continue;

      vector<VkExtensionProperties> extensions;
      extensions.resize(extensionCount);
      vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data());

      bool hasSwapchain = false;
      bool hasFullScreenExclusive = false;
      bool hasSwapchainMaintenance = false;
      for(uint32_t extensionIndex = 0; extensionIndex < extensionCount; extensionIndex++) {
        if(!strcmp(extensions[extensionIndex].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
          hasSwapchain = true;
        }
        if(!strcmp(extensions[extensionIndex].extensionName, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME)) {
          hasFullScreenExclusive = true;
        }
        if(!strcmp(extensions[extensionIndex].extensionName,VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))hasSwapchainMaintenance=true;
      }
      if(!hasSwapchain) continue;

      uint32_t queueFamilyCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, nullptr);
      if(!queueFamilyCount) continue;

      VkQueueFamilyProperties queueProps[16] = {};
      if(queueFamilyCount > 16) queueFamilyCount = 16;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, queueProps);

      for(uint32_t queueFamily = 0; queueFamily < queueFamilyCount; queueFamily++) {
        if(!(queueProps[queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 presentSupported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(candidate, queueFamily, _surface, &presentSupported);
        if(!presentSupported) continue;
        _physicalDevice = candidate;
        _queueFamily = queueFamily;
        _fullScreenExclusiveSupported = hasFullScreenExclusive&&surfaceCapabilities2;
        _presentFencesSupported=hasSwapchainMaintenance&&surfaceCapabilities2&&surfaceMaintenance;
        break;
      }

      if(_physicalDevice) break;
    }

    if(!_physicalDevice || _queueFamily == UINT32_MAX) return false;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = _queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    if(_presentFencesSupported) {
      VkPhysicalDeviceFeatures2 features={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      features.pNext=&maintenance;vkGetPhysicalDeviceFeatures2(_physicalDevice,&features);
      _presentFencesSupported=maintenance.swapchainMaintenance1!=VK_FALSE;
    }
    const char* deviceExtensions[3] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t deviceExtensionCount = 1;
    if(_fullScreenExclusiveSupported) {
      deviceExtensions[deviceExtensionCount++] = VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME;
    }
    if(_presentFencesSupported)deviceExtensions[deviceExtensionCount++]=VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    if(_presentFencesSupported)deviceInfo.pNext=&maintenance;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = deviceExtensionCount;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    result = vkCreateDevice(_physicalDevice, &deviceInfo, nullptr, &_device);
    if(result != VK_SUCCESS) return false;

    if(_fullScreenExclusiveSupported) {
      _acquireFullScreenExclusiveMode = (PFN_vkAcquireFullScreenExclusiveModeEXT)
        vkGetDeviceProcAddr(_device, "vkAcquireFullScreenExclusiveModeEXT");
      _releaseFullScreenExclusiveMode = (PFN_vkReleaseFullScreenExclusiveModeEXT)
        vkGetDeviceProcAddr(_device, "vkReleaseFullScreenExclusiveModeEXT");
      if(!_acquireFullScreenExclusiveMode || !_releaseFullScreenExclusiveMode) {
        _fullScreenExclusiveSupported = false;
      }
    }

    vkGetDeviceQueue(_device, _queueFamily, 0, &_queue);

    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = _queueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    result = vkCreateCommandPool(_device, &poolInfo, nullptr, &_commandPool);
    if(result != VK_SUCCESS) return false;

    return createFrameSync()
      && createSwapchain()
      && createRenderPass()
      && createFramebuffers()
      && createSampler()
      && createDescriptors()
      && createPipeline()
      && createCommandBuffers()
      && startPresentWorker();
  }

  auto terminate() -> void {
    stopPresentWorker();
    if(_device) vkDeviceWaitIdle(_device);
    waitForPresents();
    releaseFullScreenExclusive();

    destroyTexture();
    destroyStaging();

    if(_device && _commandBufferCount && _commandPool) {
      vkFreeCommandBuffers(_device, _commandPool, _commandBufferCount, _commandBuffers);
    }

    for(uint i = 0; i < _imageCount; i++) {
      if(_framebuffers[i]) vkDestroyFramebuffer(_device, _framebuffers[i], nullptr);
      if(_swapViews[i]) vkDestroyImageView(_device, _swapViews[i], nullptr);
      _framebuffers[i] = VK_NULL_HANDLE;
      _swapViews[i] = VK_NULL_HANDLE;
      _commandBuffers[i] = VK_NULL_HANDLE;
      _swapImages[i] = VK_NULL_HANDLE;
    }

    if(_pipeline) vkDestroyPipeline(_device, _pipeline, nullptr);
    if(_pipelineLayout) vkDestroyPipelineLayout(_device, _pipelineLayout, nullptr);
    if(_descriptorPool) vkDestroyDescriptorPool(_device, _descriptorPool, nullptr);
    if(_descriptorSetLayout) vkDestroyDescriptorSetLayout(_device, _descriptorSetLayout, nullptr);
    if(_sampler) vkDestroySampler(_device, _sampler, nullptr);
    if(_renderPass) vkDestroyRenderPass(_device, _renderPass, nullptr);
    if(_fence) vkDestroyFence(_device, _fence, nullptr);
    if(_acquireSemaphore) vkDestroySemaphore(_device, _acquireSemaphore, nullptr);
    _fence = VK_NULL_HANDLE;
    _acquireSemaphore = VK_NULL_HANDLE;
    for(auto& semaphore : _renderSemaphores) {
      if(semaphore) vkDestroySemaphore(_device, semaphore, nullptr);
      semaphore = VK_NULL_HANDLE;
    }
    for(auto& fence:_presentFences){if(fence)vkDestroyFence(_device,fence,nullptr);fence=VK_NULL_HANDLE;}
    for(auto& pending:_presentFencePending)pending=false;
    _presentFencesSupported=false;

    if(_commandPool) vkDestroyCommandPool(_device, _commandPool, nullptr);
    if(_swapchain) vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    if(_surface) vkDestroySurfaceKHR(_instance, _surface, nullptr);
    if(_device) vkDestroyDevice(_device, nullptr);
    if(_instance) vkDestroyInstance(_instance, nullptr);
    if(_fullScreenWindow) DestroyWindow(_fullScreenWindow);

    _instance = VK_NULL_HANDLE;
    _physicalDevice = VK_NULL_HANDLE;
    _device = VK_NULL_HANDLE;
    _queue = VK_NULL_HANDLE;
    _surface = VK_NULL_HANDLE;
    _swapchain = VK_NULL_HANDLE;
    _renderPass = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _pipeline = VK_NULL_HANDLE;
    _descriptorSetLayout = VK_NULL_HANDLE;
    _descriptorPool = VK_NULL_HANDLE;
    _descriptorSet = VK_NULL_HANDLE;
    _sampler = VK_NULL_HANDLE;
    _commandPool = VK_NULL_HANDLE;
    _queueFamily = UINT32_MAX;
    _imageCount = 0;
    _commandBufferCount = 0;
    _stagingFramePending = false;
    _preparedImageIndex = 0;
    _framePrepared = false;
    _recreateAfterPresent = false;
    _fullScreenWindow = nullptr;
    _monitorHandle = nullptr;
    _fullScreenExclusiveSupported = false;
    _fullScreenExclusiveAcquired = false;
    _acquireFullScreenExclusiveMode = nullptr;
    _releaseFullScreenExclusiveMode = nullptr;
    _swapExtent = {};
  }

  auto clear() -> void {
    uint32_t slot=_frames.active.load();
    uint width=slot<FastLinkVideoFrames::Slots?_frames.frames[slot].width:512;
    uint height=slot<FastLinkVideoFrames::Slots?_frames.frames[slot].height:448;
    if(auto data=_frames.acquire(width,height)) {
      memset(data,0,size_t(width)*height*4);_frames.publish(0,0);SetEvent(_presentWake);
    }
  }

  auto updateSampler() -> bool {
    if(!_device) return false;
    if(!prepareUpload()) return false;
    if(_sampler) {
      vkDestroySampler(_device, _sampler, nullptr);
      _sampler = VK_NULL_HANDLE;
    }
    if(!createSampler()) return false;
    return refreshDescriptorSets();
  }

  auto acquire(uint width,uint height) -> NativeVideoManager::Acquire {
    return {_frames.acquire(width,height),width*sizeof(uint32_t)};
  }

  auto output(uint width,uint height) -> void {
    _frames.publish(width,height);SetEvent(_presentWake);
  }

  auto acquireGPU(uint width, uint height) -> NativeVideoManager::Acquire {
    NativeVideoManager::Acquire result;
    // Only the renderer accesses mapped GPU memory. The frontend publishes
    // completed CPU images independently through the fixed frame buffers.
    if(!prepareUpload()) return result;
    if(!ensureTexture(width, height)) return result;
    result.data = (uint32_t*)_mappedStaging;
    result.pitch = width * sizeof(uint32_t);
    return result;
  }

  auto prepareUpload(uint64_t timeout = UINT64_MAX) -> bool {
    if(!_device) return false;
    if(!_stagingFramePending) return true;
    // This wait runs on the renderer, protecting the single staging buffer.
    auto waitResult = vkWaitForFences(
      _device, 1, &_fence, VK_TRUE, timeout
    );
    if(waitResult != VK_SUCCESS) return false;
    _stagingFramePending = false;
    return true;
  }

  auto prepareFrame() -> bool {
    fastLinkCrashStep("Vulkan renderer prepare");
    if(!_fence || !_acquireSemaphore || !_swapchain || !_imageCount)return false;
    if(_framePrepared)return true;
    // These short driver waits belong exclusively to the renderer. Retrying
    // cannot delay SNES execution, and the image is sampled after the wait.
    if(!prepareUpload(5'000'000)){SetEvent(_presentWake);return false;}
    uint32_t imageIndex=0;
    VkResult result=vkAcquireNextImageKHR(_device,_swapchain,5'000'000,_acquireSemaphore,VK_NULL_HANDLE,&imageIndex);
    if(result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
      _surfaceSizeChanged=true;SetEvent(_presentWake);return false;
    }
    if(result==VK_TIMEOUT || result==VK_NOT_READY){SetEvent(_presentWake);return false;}
    if(result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR){_presentResult=result;return false;}
    _preparedImageIndex=imageIndex;_framePrepared=true;return true;
  }

  auto outputGPU(uint width,uint height) -> VkResult {
    fastLinkCrashStep("Vulkan renderer submit/present");
    if(!_device || !_swapchain || !_mappedStaging || !_texImage || !_framePrepared)return VK_NOT_READY;
    uint32_t imageIndex=_preparedImageIndex;
    if(!owner.fullScreen()) {
      auto fitted=fastLinkFitVideo(_swapExtent.width,_swapExtent.height,_texHeight==480);
      width=fitted.width;height=fitted.height;
    }
    if(!_stagingCoherent && _stagingMemory && _stagingSize) {
      VkMappedMemoryRange range={VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=_stagingMemory;
      range.size=(VkDeviceSize(_texWidth)*_texHeight*4+_stagingAtomSize-1)&~(_stagingAtomSize-1);
      if(range.size>_stagingSize)range.size=VK_WHOLE_SIZE;
      VkResult result=vkFlushMappedMemoryRanges(_device,1,&range);if(result!=VK_SUCCESS)return result;
    }
    if(!recordCommandBuffer(imageIndex,width,height))return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result=vkResetFences(_device,1,&_fence);if(result!=VK_SUCCESS)return result;
    return submitPreparedFrame();
  }

  auto submitPreparedFrame() -> VkResult {
        uint32_t imageIndex=_preparedImageIndex;
        VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&_acquireSemaphore;
        submit.pWaitDstStageMask=&waitStage;submit.commandBufferCount=1;
        submit.pCommandBuffers=&_commandBuffers[imageIndex];
        submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&_renderSemaphores[imageIndex];
        _framePrepared=false;
        VkResult result=vkQueueSubmit(_queue,1,&submit,_fence);
        if(result==VK_SUCCESS) {
          _textureLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;_stagingFramePending=true;
          VkPresentInfoKHR present={VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
          present.waitSemaphoreCount=1;present.pWaitSemaphores=&_renderSemaphores[imageIndex];
          present.swapchainCount=1;present.pSwapchains=&_swapchain;present.pImageIndices=&imageIndex;
          VkSwapchainPresentFenceInfoEXT presentFence={VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
          if(_presentFencesSupported) {
            if(_presentFencePending[imageIndex])vkWaitForFences(_device,1,&_presentFences[imageIndex],VK_TRUE,UINT64_MAX);
            vkResetFences(_device,1,&_presentFences[imageIndex]);
            presentFence.swapchainCount=1;presentFence.pFences=&_presentFences[imageIndex];present.pNext=&presentFence;
          }
          result=vkQueuePresentKHR(_queue,&present);
          if(_presentFencesSupported)_presentFencePending[imageIndex]=result==VK_SUCCESS
            || result==VK_SUBOPTIMAL_KHR || result==VK_ERROR_OUT_OF_DATE_KHR
            || result==VK_ERROR_SURFACE_LOST_KHR || result==VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;
        }
    return result;
  }

  static DWORD WINAPI presentWorkerEntry(void* context) {
    auto self=(NativeVideoImplementation*)context;
    ULONG crashStack=64*1024;SetThreadStackGuarantee(&crashStack);
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
    while(WaitForSingleObject(self->_presentWake,INFINITE)==WAIT_OBJECT_0) {
      if(self->_presentStopping.load(std::memory_order_acquire))break;
      self->_presentPending.store(true,std::memory_order_release);
      fastLinkCrashStep("Vulkan renderer worker");
      bool capture=self->_captureTiming.load(std::memory_order_relaxed);
      uint64_t start=capture?chrono::nanosecond():0;
      VkResult result=VK_SUCCESS;
      auto client=self->owner.currentClientSize();
      bool visible=client.width && client.height;
      bool rebuilt=false;
      if(visible && (self->_surfaceSizeChanged.load() || self->_builtResizeEpoch.load()!=self->_resizeEpoch.load())) {
        rebuilt=self->recreateSwapchain(true);
        if(!rebuilt)result=VK_ERROR_INITIALIZATION_FAILED;
        else self->_redrawPending=true;
      }
      uint32_t slot=self->_frames.read();
      bool hasFrame=slot<FastLinkVideoFrames::Slots;
      bool fresh=hasFrame && self->_frames.frames[slot].sequence!=self->_lastRenderedSequence;
      if(visible && result==VK_SUCCESS && hasFrame && (fresh || self->_redrawPending) && self->prepareFrame()) {
        // Select again after image availability, so a busy driver gets the
        // newest completed SNES frame rather than the frame it waited on.
        auto latest=self->_frames.read();
        if(latest<FastLinkVideoFrames::Slots){self->_frames.release(slot);slot=latest;}
        if(slot<FastLinkVideoFrames::Slots) {
          const auto& frame=self->_frames.frames[slot];
          uint width=frame.width,height=frame.height,outWidth=frame.outputWidth,outHeight=frame.outputHeight;
          uint64_t sequence=frame.sequence;
          auto target=self->acquireGPU(width,height);
          if(target)std::memcpy(target.data,frame.pixels.data(),size_t(width)*height*4);
          self->_frames.release(slot);
          slot=FastLinkVideoFrames::Slots;
          if(target) {
            result=self->outputGPU(outWidth,outHeight);
            if(result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR){self->_lastRenderedSequence=sequence;self->_redrawPending=false;}
          }
        }
      }
      if(slot<FastLinkVideoFrames::Slots)self->_frames.release(slot);
      if(result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        self->_surfaceSizeChanged=true;SetEvent(self->_presentWake);
      } else if(result<0) {
        self->recoverFrameFence();self->_surfaceSizeChanged=true;
      }
      if(capture)self->recordPresentTiming(chrono::nanosecond()-start);
      if(result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR)self->_presentResult.store(result);
      self->_presentPending.store(false,std::memory_order_release);
    }
    return 0;
  }

  auto recordPresentTiming(uint64_t elapsed) -> void {
    // Keep the worst completed job until the host consumes it. A short job
    // must not overwrite the evidence of a long driver stall between frames.
    auto previous=_presentTiming.load(std::memory_order_relaxed);
    while(previous<elapsed && !_presentTiming.compare_exchange_weak(previous,elapsed,
      std::memory_order_relaxed,std::memory_order_relaxed)){}
  }

  auto startPresentWorker() -> bool {
    _presentPending=false;_presentStopping=false;_presentResult=VK_SUCCESS;_lastRenderedSequence=0;_redrawPending=true;
    _captureTiming=owner._captureTiming;_presentTiming=0;
    _presentWake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!_presentWake)return false;
    _presentThread=CreateThread(nullptr,0,presentWorkerEntry,this,0,nullptr);
    if(_presentThread && _frames.active.load()<FastLinkVideoFrames::Slots)SetEvent(_presentWake);
    return _presentThread!=nullptr;
  }

  auto stopPresentWorker() -> void {
    if(_presentThread) {
      _presentStopping.store(true,std::memory_order_release);SetEvent(_presentWake);
      // WSI can synchronously send messages to its HWND owner. Dispatch sent
      // messages while joining, so fullscreen/exit cannot deadlock USER32.
      bool wasInFrame=program.inFrame;program.inFrame=true;
      while(MsgWaitForMultipleObjectsEx(1,&_presentThread,INFINITE,QS_SENDMESSAGE,0)==WAIT_OBJECT_0+1) {
        MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
      }
      program.inFrame=wasInFrame;
      CloseHandle(_presentThread);_presentThread=nullptr;
    }
    if(_presentWake){CloseHandle(_presentWake);_presentWake=nullptr;}
    _presentPending=false;
  }

  auto createSwapchain() -> bool {
    VkSurfaceCapabilitiesKHR capabilities = {};
    auto result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, _surface, &capabilities);
    if(result != VK_SUCCESS) return false;

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &formatCount, nullptr);
    if(!formatCount) return false;

    VkSurfaceFormatKHR formats[32] = {};
    if(formatCount > 32) formatCount = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &formatCount, formats);

    _swapFormat = formats[0].format;
    _swapColorSpace = formats[0].colorSpace;
    if(formatCount == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
      _swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
      _swapColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    } else {
      for(uint32_t index = 0; index < formatCount; index++) {
        if(formats[index].format == VK_FORMAT_B8G8R8A8_UNORM && formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
          _swapFormat = formats[index].format;
          _swapColorSpace = formats[index].colorSpace;
          break;
        }
      }
    }

    _swapExtent = capabilities.currentExtent;
    if(_swapExtent.width == UINT32_MAX || _swapExtent.height == UINT32_MAX) {
      auto clientSize = owner.currentClientSize();
      _swapExtent.width = clientSize.width;
      _swapExtent.height = clientSize.height;
    }
    if(!_swapExtent.width || !_swapExtent.height) return false;

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(_physicalDevice, _surface, &presentModeCount, nullptr);
    VkPresentModeKHR presentModes[16] = {};
    if(presentModeCount > 16) presentModeCount = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(_physicalDevice, _surface, &presentModeCount, presentModes);

    VkPresentModeKHR presentMode = choosePresentMode(presentModes, presentModeCount, owner.presentMode());

    uint32_t requestedImages = owner.swapchainImages();
    if(requestedImages < 2) requestedImages = 2;
    // A two-image Mailbox swapchain can have one image in scanout and the
    // other queued, leaving no target for the next frame. Requesting three
    // preserves Mailbox's replace-the-pending-frame behavior without pacing
    // SNES execution to image availability.
    if(presentMode == VK_PRESENT_MODE_MAILBOX_KHR && requestedImages < 3) requestedImages = 3;
    if(requestedImages > _maxImages) requestedImages = _maxImages;
    uint32_t minImages = capabilities.minImageCount;
    if(minImages < requestedImages) minImages = requestedImages;
    if(capabilities.maxImageCount && minImages > capabilities.maxImageCount) {
      minImages = capabilities.maxImageCount;
    }

    auto oldSwapchain = _swapchain;
    releaseFullScreenExclusive();

    VkSurfaceFullScreenExclusiveWin32InfoEXT win32ExclusiveInfo = {
      VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT
    };
    win32ExclusiveInfo.hmonitor = _monitorHandle;
    VkSurfaceFullScreenExclusiveInfoEXT exclusiveInfo = {
      VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT
    };
    exclusiveInfo.pNext = &win32ExclusiveInfo;
    exclusiveInfo.fullScreenExclusive = owner.fullScreen()
      ? VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT
      : VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;

    VkSwapchainCreateInfoKHR swapInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    if(_fullScreenExclusiveSupported) swapInfo.pNext = &exclusiveInfo;
    swapInfo.surface = _surface;
    swapInfo.minImageCount = minImages;
    swapInfo.imageFormat = _swapFormat;
    swapInfo.imageColorSpace = _swapColorSpace;
    swapInfo.imageExtent = _swapExtent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = capabilities.currentTransform;
    swapInfo.compositeAlpha = chooseCompositeAlpha(capabilities);
    swapInfo.presentMode = presentMode;
    swapInfo.clipped = VK_TRUE;
    swapInfo.oldSwapchain = oldSwapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    result = vkCreateSwapchainKHR(_device, &swapInfo, nullptr, &newSwapchain);
    if(result != VK_SUCCESS) return false;

    _swapchain = newSwapchain;
    if(oldSwapchain) vkDestroySwapchainKHR(_device, oldSwapchain, nullptr);

    result = vkGetSwapchainImagesKHR(_device, _swapchain, &_imageCount, nullptr);
    // Never truncate the list: acquire can return any real image index.
    if(result != VK_SUCCESS || !_imageCount || _imageCount > _maxImages) {
      _imageCount = 0;
      return false;
    }
    result = vkGetSwapchainImagesKHR(_device, _swapchain, &_imageCount, _swapImages);
    if(result != VK_SUCCESS) return false;
    if(owner.fullScreen() && _fullScreenExclusiveSupported) {
      result = _acquireFullScreenExclusiveMode(_device, _swapchain);
      _fullScreenExclusiveAcquired = result == VK_SUCCESS;
    }
    return true;
  }

  auto recreateSwapchain(bool onWorker=false) -> bool {
    fastLinkCrashStep("Vulkan rebuild swapchain");
    if(!_device) return false;
    if(!onWorker){_surfaceSizeChanged=true;return true;}
    uint64_t resizeEpoch=_resizeEpoch.load(std::memory_order_relaxed);
    _surfaceSizeChanged=false;
    if(vkDeviceWaitIdle(_device)!=VK_SUCCESS)return false;
    if(!waitForPresents())return false;
    releaseFullScreenExclusive();

    if(_commandBufferCount && _commandPool) {
      vkFreeCommandBuffers(_device, _commandPool, _commandBufferCount, _commandBuffers);
    }
    _commandBufferCount=0;
    for(auto& command:_commandBuffers)command=VK_NULL_HANDLE;

    for(uint i = 0; i < _imageCount; i++) {
      if(_framebuffers[i]) vkDestroyFramebuffer(_device, _framebuffers[i], nullptr);
      if(_swapViews[i]) vkDestroyImageView(_device, _swapViews[i], nullptr);
      _framebuffers[i] = VK_NULL_HANDLE;
      _swapViews[i] = VK_NULL_HANDLE;
    }

    _imageCount = 0;
    // A new swapchain's image indices do not identify the old presents. Use
    // fresh binary semaphores rather than re-signalling an old present's wait.
    VkSemaphoreCreateInfo semaphoreInfo={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(auto& semaphore:_renderSemaphores) {
      if(semaphore)vkDestroySemaphore(_device,semaphore,nullptr);
      semaphore=VK_NULL_HANDLE;
      if(vkCreateSemaphore(_device,&semaphoreInfo,nullptr,&semaphore)!=VK_SUCCESS)return false;
    }
    bool rebuilt = createSwapchain()
      && createFramebuffers()
      && createCommandBuffers();
    _stagingFramePending = false;
    _framePrepared = false;
    _recreateAfterPresent = false;
    if(!rebuilt)_surfaceSizeChanged=true;
    if(rebuilt)_builtResizeEpoch.store(resizeEpoch,std::memory_order_relaxed);
    return rebuilt;
  }

  auto releaseFullScreenExclusive() -> void {
    if(!_fullScreenExclusiveAcquired || !_releaseFullScreenExclusiveMode || !_device || !_swapchain) return;
    _releaseFullScreenExclusiveMode(_device, _swapchain);
    _fullScreenExclusiveAcquired = false;
  }

  auto waitForPresents() -> bool {
    if(!_device || !_presentFencesSupported)return true;
    for(uint i=0;i<_maxImages;i++)if(_presentFencePending[i]) {
      if(vkWaitForFences(_device,1,&_presentFences[i],VK_TRUE,UINT64_MAX)!=VK_SUCCESS)return false;
      _presentFencePending[i]=false;
    }
    return true;
  }

  auto recoverFrameFence() -> void {
    if(!_device) return;
    vkDeviceWaitIdle(_device);
    if(_fence) vkDestroyFence(_device, _fence, nullptr);
    if(_acquireSemaphore)vkDestroySemaphore(_device,_acquireSemaphore,nullptr);
    _acquireSemaphore=VK_NULL_HANDLE;
    VkSemaphoreCreateInfo semaphoreInfo={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(_device,&semaphoreInfo,nullptr,&_acquireSemaphore);
    _textureLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    _fence = VK_NULL_HANDLE;
    vkCreateFence(_device, &fenceInfo, nullptr, &_fence);
    _stagingFramePending = false;
  }

  auto createRenderPass() -> bool {
    VkAttachmentDescription attachment = {};
    attachment.format = _swapFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorReference = {};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    return vkCreateRenderPass(_device, &renderPassInfo, nullptr, &_renderPass) == VK_SUCCESS;
  }

  auto createFramebuffers() -> bool {
    for(uint i = 0; i < _imageCount; i++) {
      VkImageViewCreateInfo imageViewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      imageViewInfo.image = _swapImages[i];
      imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      imageViewInfo.format = _swapFormat;
      imageViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if(vkCreateImageView(_device, &imageViewInfo, nullptr, &_swapViews[i]) != VK_SUCCESS) return false;

      VkFramebufferCreateInfo framebufferInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      framebufferInfo.renderPass = _renderPass;
      framebufferInfo.attachmentCount = 1;
      framebufferInfo.pAttachments = &_swapViews[i];
      framebufferInfo.width = _swapExtent.width;
      framebufferInfo.height = _swapExtent.height;
      framebufferInfo.layers = 1;
      if(vkCreateFramebuffer(_device, &framebufferInfo, nullptr, &_framebuffers[i]) != VK_SUCCESS) return false;

    }
    return true;
  }

  auto createSampler() -> bool {
    VkFilter filter = owner.shader() == "Blur" ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = filter;
    samplerInfo.minFilter = filter;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(_device, &samplerInfo, nullptr, &_sampler) == VK_SUCCESS;
  }

  auto createDescriptors() -> bool {
    if(!_descriptorSetLayout) {
      VkDescriptorSetLayoutBinding binding = {};
      binding.binding = 0;
      binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      binding.descriptorCount = 1;
      binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

      VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      layoutInfo.bindingCount = 1;
      layoutInfo.pBindings = &binding;
      if(vkCreateDescriptorSetLayout(_device, &layoutInfo, nullptr, &_descriptorSetLayout) != VK_SUCCESS) return false;
    }

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if(vkCreateDescriptorPool(_device, &poolInfo, nullptr, &_descriptorPool) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo allocateInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = _descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &_descriptorSetLayout;
    if(vkAllocateDescriptorSets(_device, &allocateInfo, &_descriptorSet) != VK_SUCCESS) return false;
    return true;
  }

  auto refreshDescriptorSets() -> bool {
    // Updating descriptors invalidates recorded commands that reference them.
    for(auto& recorded : _recordedFrames) recorded.valid = false;
    if(!_descriptorPool || !_texView || !_sampler) return true;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.sampler = _sampler;
    imageInfo.imageView = _texView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = _descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);

    return true;
  }

  auto createPipeline() -> bool {
    static const uint32_t vertSpv[] = {
      0x07230203, 0x00010000, 0x0008000b, 0x00000032, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
      0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
      0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x00000018, 0x00000020,
      0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005,
      0x00000009, 0x00000070, 0x00060005, 0x0000000c, 0x565f6c67, 0x65747265, 0x646e4978, 0x00007865,
      0x00030005, 0x00000018, 0x00007675, 0x00060005, 0x0000001e, 0x505f6c67, 0x65567265, 0x78657472,
      0x00000000, 0x00060006, 0x0000001e, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006,
      0x0000001e, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x0000001e,
      0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e, 0x00070006, 0x0000001e, 0x00000003,
      0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e, 0x00030005, 0x00000020, 0x00000000, 0x00040047,
      0x0000000c, 0x0000000b, 0x0000002a, 0x00040047, 0x00000018, 0x0000001e, 0x00000000, 0x00030047,
      0x0000001e, 0x00000002, 0x00050048, 0x0000001e, 0x00000000, 0x0000000b, 0x00000000, 0x00050048,
      0x0000001e, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000001e, 0x00000002, 0x0000000b,
      0x00000003, 0x00050048, 0x0000001e, 0x00000003, 0x0000000b, 0x00000004, 0x00020013, 0x00000002,
      0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
      0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00040015, 0x0000000a,
      0x00000020, 0x00000001, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b,
      0x0000000c, 0x00000001, 0x0004002b, 0x0000000a, 0x0000000e, 0x00000001, 0x0004002b, 0x0000000a,
      0x00000010, 0x00000002, 0x00040020, 0x00000017, 0x00000003, 0x00000007, 0x0004003b, 0x00000017,
      0x00000018, 0x00000003, 0x00040017, 0x0000001a, 0x00000006, 0x00000004, 0x00040015, 0x0000001b,
      0x00000020, 0x00000000, 0x0004002b, 0x0000001b, 0x0000001c, 0x00000001, 0x0004001c, 0x0000001d,
      0x00000006, 0x0000001c, 0x0006001e, 0x0000001e, 0x0000001a, 0x00000006, 0x0000001d, 0x0000001d,
      0x00040020, 0x0000001f, 0x00000003, 0x0000001e, 0x0004003b, 0x0000001f, 0x00000020, 0x00000003,
      0x0004002b, 0x0000000a, 0x00000021, 0x00000000, 0x0004002b, 0x0000001b, 0x00000022, 0x00000000,
      0x00040020, 0x00000023, 0x00000007, 0x00000006, 0x0004002b, 0x00000006, 0x00000026, 0x40000000,
      0x0004002b, 0x00000006, 0x00000028, 0x3f800000, 0x0004002b, 0x00000006, 0x0000002e, 0x00000000,
      0x00040020, 0x00000030, 0x00000003, 0x0000001a, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
      0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003d,
      0x0000000a, 0x0000000d, 0x0000000c, 0x000500c4, 0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e,
      0x000500c7, 0x0000000a, 0x00000011, 0x0000000f, 0x00000010, 0x0004006f, 0x00000006, 0x00000012,
      0x00000011, 0x0004003d, 0x0000000a, 0x00000013, 0x0000000c, 0x000500c7, 0x0000000a, 0x00000014,
      0x00000013, 0x00000010, 0x0004006f, 0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007,
      0x00000016, 0x00000012, 0x00000015, 0x0003003e, 0x00000009, 0x00000016, 0x0004003d, 0x00000007,
      0x00000019, 0x00000009, 0x0003003e, 0x00000018, 0x00000019, 0x00050041, 0x00000023, 0x00000024,
      0x00000009, 0x00000022, 0x0004003d, 0x00000006, 0x00000025, 0x00000024, 0x00050085, 0x00000006,
      0x00000027, 0x00000025, 0x00000026, 0x00050083, 0x00000006, 0x00000029, 0x00000027, 0x00000028,
      0x00050041, 0x00000023, 0x0000002a, 0x00000009, 0x0000001c, 0x0004003d, 0x00000006, 0x0000002b,
      0x0000002a, 0x00050085, 0x00000006, 0x0000002c, 0x0000002b, 0x00000026, 0x00050083, 0x00000006,
      0x0000002d, 0x00000028, 0x0000002c, 0x00070050, 0x0000001a, 0x0000002f, 0x00000029, 0x0000002d,
      0x0000002e, 0x00000028, 0x00050041, 0x00000030, 0x00000031, 0x00000020, 0x00000021, 0x0003003e,
      0x00000031, 0x0000002f, 0x000100fd, 0x00010038,
    };

    static const uint32_t fragSpv[] = {
      0x07230203, 0x00010000, 0x0008000b, 0x0000001e, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
      0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
      0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00000011, 0x00030010,
      0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
      0x00000000, 0x00040005, 0x00000009, 0x6f6c6f63, 0x00000072, 0x00030005, 0x0000000d, 0x00786574,
      0x00030005, 0x00000011, 0x00007675, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047,
      0x0000000d, 0x00000021, 0x00000000, 0x00040047, 0x0000000d, 0x00000022, 0x00000000, 0x00040047,
      0x00000011, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
      0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020,
      0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00090019,
      0x0000000a, 0x00000006, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
      0x0003001b, 0x0000000b, 0x0000000a, 0x00040020, 0x0000000c, 0x00000000, 0x0000000b, 0x0004003b,
      0x0000000c, 0x0000000d, 0x00000000, 0x00040017, 0x0000000f, 0x00000006, 0x00000002, 0x00040020,
      0x00000010, 0x00000001, 0x0000000f, 0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x00040015,
      0x00000012, 0x00000020, 0x00000000, 0x0004002b, 0x00000012, 0x00000013, 0x00000000, 0x00040020,
      0x00000014, 0x00000001, 0x00000006, 0x0004002b, 0x00000006, 0x00000017, 0x3f800000, 0x0004002b,
      0x00000012, 0x00000018, 0x00000001, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
      0x000200f8, 0x00000005, 0x0004003d, 0x0000000b, 0x0000000e, 0x0000000d, 0x00050041, 0x00000014,
      0x00000015, 0x00000011, 0x00000013, 0x0004003d, 0x00000006, 0x00000016, 0x00000015, 0x00050041,
      0x00000014, 0x00000019, 0x00000011, 0x00000018, 0x0004003d, 0x00000006, 0x0000001a, 0x00000019,
      0x00050083, 0x00000006, 0x0000001b, 0x00000017, 0x0000001a, 0x00050050, 0x0000000f, 0x0000001c,
      0x00000016, 0x0000001b, 0x00050057, 0x00000007, 0x0000001d, 0x0000000e, 0x0000001c, 0x0003003e,
      0x00000009, 0x0000001d, 0x000100fd, 0x00010038,
    };

    VkShaderModuleCreateInfo vertexModuleInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vertexModuleInfo.codeSize = sizeof(vertSpv);
    vertexModuleInfo.pCode = vertSpv;
    VkShaderModule vertexModule = VK_NULL_HANDLE;
    if(vkCreateShaderModule(_device, &vertexModuleInfo, nullptr, &vertexModule) != VK_SUCCESS) return false;

    VkShaderModuleCreateInfo fragmentModuleInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fragmentModuleInfo.codeSize = sizeof(fragSpv);
    fragmentModuleInfo.pCode = fragSpv;
    VkShaderModule fragmentModule = VK_NULL_HANDLE;
    if(vkCreateShaderModule(_device, &fragmentModuleInfo, nullptr, &fragmentModule) != VK_SUCCESS) {
      vkDestroyShaderModule(_device, vertexModule, nullptr);
      return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0, 0, (float)_swapExtent.width, (float)_swapExtent.height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, _swapExtent};
    VkPipelineViewportStateCreateInfo viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_descriptorSetLayout;
    if(vkCreatePipelineLayout(_device, &layoutInfo, nullptr, &_pipelineLayout) != VK_SUCCESS) {
      vkDestroyShaderModule(_device, vertexModule, nullptr);
      vkDestroyShaderModule(_device, fragmentModule, nullptr);
      return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = _pipelineLayout;
    pipelineInfo.renderPass = _renderPass;
    pipelineInfo.subpass = 0;

    auto result = vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_pipeline);
    vkDestroyShaderModule(_device, vertexModule, nullptr);
    vkDestroyShaderModule(_device, fragmentModule, nullptr);
    return result == VK_SUCCESS;
  }

  auto createCommandBuffers() -> bool {
    for(auto& recorded : _recordedFrames) recorded.valid = false;
    if(_imageCount > _maxImages) return false;
    VkCommandBufferAllocateInfo allocateInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = _commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = _imageCount;
    if(vkAllocateCommandBuffers(_device, &allocateInfo, _commandBuffers)!=VK_SUCCESS)return false;
    _commandBufferCount=_imageCount;return true;
  }

  auto createFrameSync() -> bool {
    VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if(vkCreateSemaphore(_device, &semaphoreInfo, nullptr, &_acquireSemaphore) != VK_SUCCESS) return false;
    if(vkCreateFence(_device, &fenceInfo, nullptr, &_fence) != VK_SUCCESS) return false;
    for(auto& semaphore : _renderSemaphores) {
      if(vkCreateSemaphore(_device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) return false;
    }
    if(_presentFencesSupported)for(auto& fence:_presentFences)
      if(vkCreateFence(_device,&fenceInfo,nullptr,&fence)!=VK_SUCCESS)return false;

    return true;
  }

  auto findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) -> uint32_t {
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &memoryProperties);
    for(uint32_t index = 0; index < memoryProperties.memoryTypeCount; index++) {
      if((typeBits & (1u << index)) && (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties) {
        return index;
      }
    }
    return UINT32_MAX;
  }

  auto ensureTexture(uint width, uint height) -> bool {
    if(_texWidth == width && _texHeight == height && _mappedStaging) return true;

    // acquire() has already retired the only graphics submission.
    destroyTexture();

    _texWidth = width;
    _texHeight = height;

    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auto result = vkCreateImage(_device, &imageInfo, nullptr, &_texImage);
    if(result != VK_SUCCESS) return false;

    VkMemoryRequirements imageMemoryRequirements = {};
    vkGetImageMemoryRequirements(_device, _texImage, &imageMemoryRequirements);
    VkMemoryAllocateInfo imageAllocateInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    imageAllocateInfo.allocationSize = imageMemoryRequirements.size;
    imageAllocateInfo.memoryTypeIndex = findMemoryType(imageMemoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(imageAllocateInfo.memoryTypeIndex == UINT32_MAX) return false;
    result = vkAllocateMemory(_device, &imageAllocateInfo, nullptr, &_texMemory);
    if(result != VK_SUCCESS) return false;
    result = vkBindImageMemory(_device, _texImage, _texMemory, 0);
    if(result != VK_SUCCESS) return false;

    VkImageViewCreateInfo imageViewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    imageViewInfo.image = _texImage;
    imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    result = vkCreateImageView(_device, &imageViewInfo, nullptr, &_texView);
    if(result != VK_SUCCESS) return false;

    VkDeviceSize stagingSize = VkDeviceSize(width) * height * 4;
    if(_mappedStaging && stagingSize <= _stagingCapacity) return refreshDescriptorSets();
    destroyStaging();
    // Reserve the largest standard SNES frame once; larger inputs can grow it.
    if(stagingSize < 512 * 480 * 4) stagingSize = 512 * 480 * 4;
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = stagingSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    result = vkCreateBuffer(_device, &bufferInfo, nullptr, &_stagingBuffer);
    if(result != VK_SUCCESS) return false;

    VkMemoryRequirements stagingMemoryRequirements = {};
    vkGetBufferMemoryRequirements(_device, _stagingBuffer, &stagingMemoryRequirements);
    VkMemoryAllocateInfo stagingAllocateInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    stagingAllocateInfo.allocationSize = stagingMemoryRequirements.size;
    stagingAllocateInfo.memoryTypeIndex = findMemoryType(
      stagingMemoryRequirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    _stagingCoherent = stagingAllocateInfo.memoryTypeIndex != UINT32_MAX;
    if(!_stagingCoherent) {
      stagingAllocateInfo.memoryTypeIndex = findMemoryType(
        stagingMemoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      );
      VkPhysicalDeviceProperties properties = {};
      vkGetPhysicalDeviceProperties(_physicalDevice, &properties);
      _stagingAtomSize = properties.limits.nonCoherentAtomSize;
    }
    if(stagingAllocateInfo.memoryTypeIndex == UINT32_MAX) return false;
    result = vkAllocateMemory(_device, &stagingAllocateInfo, nullptr, &_stagingMemory);
    if(result != VK_SUCCESS) return false;
    result = vkBindBufferMemory(_device, _stagingBuffer, _stagingMemory, 0);
    if(result != VK_SUCCESS) return false;
    result = vkMapMemory(_device, _stagingMemory, 0, stagingMemoryRequirements.size, 0, &_mappedStaging);
    if(result != VK_SUCCESS) {
      _mappedStaging = nullptr;
      return false;
    }

    _stagingSize = stagingMemoryRequirements.size;
    _stagingCapacity = stagingSize;
    _textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return refreshDescriptorSets();
  }

  auto destroyTexture() -> void {
    if(_texView) vkDestroyImageView(_device, _texView, nullptr);
    if(_texImage) vkDestroyImage(_device, _texImage, nullptr);
    if(_texMemory) vkFreeMemory(_device, _texMemory, nullptr);
    _texView = VK_NULL_HANDLE;
    _texImage = VK_NULL_HANDLE;
    _texMemory = VK_NULL_HANDLE;
    _texWidth = _texHeight = 0;
    _textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  auto destroyStaging() -> void {
    if(_mappedStaging) vkUnmapMemory(_device, _stagingMemory);
    if(_stagingBuffer) vkDestroyBuffer(_device, _stagingBuffer, nullptr);
    if(_stagingMemory) vkFreeMemory(_device, _stagingMemory, nullptr);
    _mappedStaging = nullptr;
    _stagingBuffer = VK_NULL_HANDLE;
    _stagingMemory = VK_NULL_HANDLE;
    _stagingSize = _stagingCapacity = 0;
    _stagingCoherent = true;
  }

  auto recordCommandBuffer(uint32_t imageIndex, uint outputWidth, uint outputHeight) -> bool {
    if(!outputWidth || outputWidth > _swapExtent.width) outputWidth = _swapExtent.width;
    if(!outputHeight || outputHeight > _swapExtent.height) outputHeight = _swapExtent.height;
    auto& recorded = _recordedFrames[imageIndex];
    // The source buffer, descriptors and framebuffer remain fixed between
    // resizes. Re-submit the commands while uploading fresh mapped pixels.
    // The first UNDEFINED-layout upload is re-recorded once before reuse.
    if(recorded.valid && recorded.width == outputWidth && recorded.height == outputHeight
    && recorded.initialLayout == _textureLayout) return true;
    auto commandBuffer = _commandBuffers[imageIndex];
    if(vkResetCommandBuffer(commandBuffer,0)!=VK_SUCCESS)return false;

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = 0;  // reusable; prepareUpload() prevents concurrent use
    if(vkBeginCommandBuffer(commandBuffer,&beginInfo)!=VK_SUCCESS)return false;

    VkImageMemoryBarrier imageBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageBarrier.oldLayout = _textureLayout;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imageBarrier.srcAccessMask = _textureLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imageBarrier.image = _texImage;
    imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(
      commandBuffer,
      _textureLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0,
      0, nullptr,
      0, nullptr,
      1, &imageBarrier
    );

    VkBufferImageCopy copyRegion = {};
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageExtent = {_texWidth, _texHeight, 1};
    vkCmdCopyBufferToImage(commandBuffer, _stagingBuffer, _texImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
      commandBuffer,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      0,
      0, nullptr,
      0, nullptr,
      1, &imageBarrier
    );

    VkClearValue clearValue = {};
    VkRenderPassBeginInfo renderPassInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    renderPassInfo.renderPass = _renderPass;
    renderPassInfo.framebuffer = _framebuffers[imageIndex];
    renderPassInfo.renderArea = {{0, 0}, _swapExtent};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    int32_t viewportX = (int32_t)(_swapExtent.width - outputWidth) / 2;
    int32_t viewportY = (int32_t)(_swapExtent.height - outputHeight) / 2;

    VkViewport viewport = {(float)viewportX, (float)viewportY, (float)outputWidth, (float)outputHeight, 0.0f, 1.0f};
    VkRect2D scissor = {{viewportX, viewportY}, {outputWidth, outputHeight}};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipelineLayout, 0, 1, &_descriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);
    if(vkEndCommandBuffer(commandBuffer)!=VK_SUCCESS)return false;
    recorded = {true, outputWidth, outputHeight, _textureLayout};
    return true;
  }

  NativeVideoManager& owner;

  void* _mappedStaging = nullptr;
  HWND _fullScreenWindow = nullptr;
  HMONITOR _monitorHandle = nullptr;

  VkInstance _instance = VK_NULL_HANDLE;
  VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
  VkDevice _device = VK_NULL_HANDLE;
  VkQueue _queue = VK_NULL_HANDLE;
  HANDLE _presentWake=nullptr,_presentThread=nullptr;
  std::atomic<bool> _presentPending{false},_presentStopping{false};
  std::atomic<int> _presentResult{VK_SUCCESS};
  std::atomic<bool> _captureTiming{false};
  std::atomic<uint64_t> _presentTiming{0};
  FastLinkVideoFrames _frames;
  uint64_t _lastRenderedSequence=0;
  bool _redrawPending=true;
  VkResult _reportedError=VK_SUCCESS;
  std::atomic<uint64_t> _resizeEpoch{0},_builtResizeEpoch{0};
  VkSurfaceKHR _surface = VK_NULL_HANDLE;
  VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
  uint32_t _queueFamily = UINT32_MAX;

  static constexpr uint _maxImages = 4;
  VkImage _swapImages[_maxImages] = {};
  VkImageView _swapViews[_maxImages] = {};
  VkFramebuffer _framebuffers[_maxImages] = {};
  VkCommandBuffer _commandBuffers[_maxImages] = {};
  struct RecordedFrame {
    bool valid = false;
    uint width = 0, height = 0;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  };
  RecordedFrame _recordedFrames[_maxImages] = {};
  VkDescriptorSet _descriptorSet = VK_NULL_HANDLE;
  VkFence _fence = VK_NULL_HANDLE;
  VkSemaphore _acquireSemaphore = VK_NULL_HANDLE;
  VkSemaphore _renderSemaphores[_maxImages] = {};
  bool _presentFencesSupported=false;
  VkFence _presentFences[_maxImages]={};
  bool _presentFencePending[_maxImages]={};
  uint32_t _imageCount = 0;
  uint32_t _commandBufferCount = 0;
  bool _stagingFramePending = false;
  uint32_t _preparedImageIndex = 0;
  bool _framePrepared = false;
  bool _recreateAfterPresent = false;
  std::atomic<bool> _surfaceSizeChanged{false};
  VkExtent2D _swapExtent = {};
  VkFormat _swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
  VkColorSpaceKHR _swapColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

  VkRenderPass _renderPass = VK_NULL_HANDLE;
  VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
  VkPipeline _pipeline = VK_NULL_HANDLE;
  VkDescriptorSetLayout _descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
  VkSampler _sampler = VK_NULL_HANDLE;

  VkImage _texImage = VK_NULL_HANDLE;
  VkDeviceMemory _texMemory = VK_NULL_HANDLE;
  VkImageView _texView = VK_NULL_HANDLE;
  VkBuffer _stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _stagingMemory = VK_NULL_HANDLE;
  VkDeviceSize _stagingSize = 0;
  VkDeviceSize _stagingCapacity = 0;
  VkDeviceSize _stagingAtomSize = 1;
  uint _texWidth = 0;
  uint _texHeight = 0;
  bool _stagingCoherent = true;
  VkImageLayout _textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  bool _fullScreenExclusiveSupported = false;
  bool _fullScreenExclusiveAcquired = false;
  PFN_vkAcquireFullScreenExclusiveModeEXT _acquireFullScreenExclusiveMode = nullptr;
  PFN_vkReleaseFullScreenExclusiveModeEXT _releaseFullScreenExclusiveMode = nullptr;

  VkCommandPool _commandPool = VK_NULL_HANDLE;
};

NativeVideoManager::NativeVideoManager() {
  reset();
}

NativeVideoManager::~NativeVideoManager() {
  reset();
}

NativeVideoManager::operator bool() const {
  return _driver != "None";
}

auto NativeVideoManager::hasMonitors() -> vector<Monitor> {
  vector<Monitor> monitors;
  EnumDisplayMonitors(nullptr, nullptr, NativeMonitorEnumProc, (LPARAM)&monitors);

  vector<Monitor> sorted;
  for(auto& monitor : monitors) if(monitor.primary) sorted.append(monitor);
  for(auto& monitor : monitors) if(!monitor.primary) sorted.append(monitor);
  return sorted;
}

auto NativeVideoManager::hasMonitor(string name) -> bool {
  if(!name || name == "Primary") return true;
  for(auto& monitor : hasMonitors()) {
    if(monitor.name == name) return true;
  }
  return false;
}

auto NativeVideoManager::reset() -> void {
  _impl.reset();

  _driver = "None";
  _ready = false;
  _fullScreen = false;
  _monitor = defaultMonitorName();
  _exclusive = false;
  _context = 0;
  _blocking = false;
  _flush = false;
  _presentMode = "Mailbox";
  _swapchainImages = 2;
  _format = "ARGB24";
  _shader = "None";
  _lastSize = {};
}

auto NativeVideoManager::create(string driver) -> bool {
  if(!driver) driver = optimalDriver();
  _driver = driver;
  _ready = false;

  if(driver == "None") {
    _impl.reset();
    return true;
  }

  return initialize();
}

auto NativeVideoManager::setFullScreen(bool fullScreen) -> bool {
  if(_fullScreen == fullScreen) return true;
  if(_impl && _ready)_impl->stopPresentWorker();
  _fullScreen = fullScreen;
  if(_impl && _ready) return initialize();
  return true;
}

auto NativeVideoManager::setMonitor(string monitor) -> bool {
  if(_monitor==monitor)return true;
  if(!hasMonitor(monitor)) return false;
  if(_monitor == monitor || (monitor == "Primary" && _monitor == defaultMonitorName())) return true;
  if(_impl && _ready && _fullScreen)_impl->stopPresentWorker();
  _monitor = monitor == "Primary" ? defaultMonitorName() : monitor;
  if(_fullScreen && _impl && _ready) return initialize();
  return true;
}

auto NativeVideoManager::setExclusive(bool exclusive) -> bool {
  _exclusive = exclusive;
  return true;
}

auto NativeVideoManager::setContext(uintptr context) -> bool {
  if(_context == context) return true;
  _context = context;
  return true;
}

auto NativeVideoManager::setBlocking(bool blocking) -> bool {
  _blocking = blocking;
  return true;
}

auto NativeVideoManager::setFlush(bool flush) -> bool {
  _flush = flush;
  return true;
}

auto NativeVideoManager::setPresentMode(string presentMode) -> bool {
  if(presentMode != "Immediate" && presentMode != "Mailbox" && presentMode != "FIFO") return false;
  if(_presentMode == presentMode) return true;
  if(_impl && _ready)_impl->stopPresentWorker();
  _presentMode = presentMode;
  if(_impl && _ready){_impl->recreateSwapchain();return _ready=_impl->startPresentWorker();}
  return true;
}

auto NativeVideoManager::setSwapchainImages(uint swapchainImages) -> bool {
  if(swapchainImages < 2) swapchainImages = 2;
  if(swapchainImages > NativeVideoImplementation::_maxImages) swapchainImages = NativeVideoImplementation::_maxImages;
  if(_swapchainImages == swapchainImages) return true;
  if(_impl && _ready)_impl->stopPresentWorker();
  _swapchainImages = swapchainImages;
  if(_impl && _ready){_impl->recreateSwapchain();return _ready=_impl->startPresentWorker();}
  return true;
}

auto NativeVideoManager::setFormat(string format) -> bool {
  if(format != "ARGB24") return false;
  _format = format;
  return true;
}

auto NativeVideoManager::setShader(string shader) -> bool {
  if(shader != "None" && shader != "Blur") return false;
  if(_shader == shader) return true;
  if(_impl && _ready)_impl->stopPresentWorker();
  _shader = shader;
  if(_impl && _ready){bool changed=_impl->updateSampler();return _ready=changed&&_impl->startPresentWorker();}
  return true;
}

auto NativeVideoManager::focused() -> bool {
  auto hwnd = _fullScreen && _impl ? _impl->_fullScreenWindow : (HWND)_context;
  if(!hwnd || !IsWindow(hwnd)) return true;
  auto root = GetAncestor(hwnd, GA_ROOT);
  return GetForegroundWindow() == root;
}

auto NativeVideoManager::clear() -> void {
  if(_impl) _impl->clear();
}

auto NativeVideoManager::acquire(uint width, uint height) -> Acquire {
  if(!_ready || !_impl) return {};
  return _impl->acquire(width, height);
}

auto NativeVideoManager::output(uint width, uint height) -> void {
  if(!_ready || !_impl) return;
  _impl->output(width, height);
}

auto NativeVideoManager::prepare() -> bool {
  if(!_ready || !_impl)return false;
  auto result=(VkResult)_impl->_presentResult.exchange(VK_SUCCESS);
  if(result<0 && result!=_impl->_reportedError) {
    _impl->_reportedError=result;program.showMessage({"Vulkan driver error: ",(int)result});
  }
  return !_impl->_presentPending.load(std::memory_order_acquire);
}

auto NativeVideoManager::captureTiming(bool enabled) -> void {
  _captureTiming=enabled;
  if(_impl)_impl->_captureTiming.store(enabled,std::memory_order_relaxed);
}

auto NativeVideoManager::takePresentTiming() -> uint64_t {
  return _impl?_impl->_presentTiming.exchange(0,std::memory_order_relaxed):0;
}

auto NativeVideoManager::poll() -> void {
  auto size = currentClientSize();
  if(size.width == _lastSize.width && size.height == _lastSize.height) return;
  _lastSize = size;
  if(_impl){
    _impl->_resizeEpoch.fetch_add(1,std::memory_order_relaxed);_impl->_surfaceSizeChanged=true;
    if(_ready && size.width && size.height)SetEvent(_impl->_presentWake);
  }
}

auto NativeVideoManager::initialize() -> bool {
  if(!_impl) _impl = new NativeVideoImplementation(*this);
  _ready = _impl->initialize();
  if(_ready) _lastSize = currentClientSize();
  return _ready;
}

auto NativeVideoManager::defaultMonitorName() const -> string {
  auto monitors = hasMonitors();
  for(auto& monitor : monitors) {
    if(monitor.primary) return monitor.name;
  }
  if(monitors) return monitors.first().name;
  return "Primary";
}

auto NativeVideoManager::currentClientSize() const -> Size {
  // poll() samples the actual window once per frontend iteration.
  auto hwnd = _fullScreen && _impl ? _impl->_fullScreenWindow : (HWND)_context;
  if(!hwnd || IsIconic(GetAncestor(hwnd, GA_ROOT))) return {};

  RECT rect = {};
  if(!GetClientRect(hwnd, &rect)) return {};
  return {(uint)(rect.right - rect.left), (uint)(rect.bottom - rect.top)};
}

#endif

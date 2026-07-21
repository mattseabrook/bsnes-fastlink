#include "../bsnes.hpp"

#if defined(PLATFORM_WINDOWS)

#define VK_USE_PLATFORM_WIN32_KHR
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdio>
#include <cstring>
#include <vulkan/vulkan.h>

namespace {
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
    fastLinkDebug("NativeVulkan::initialize begin context=%p", (void*)owner.context());
    terminate();
    fastLinkDebug("NativeVulkan::initialize after terminate");

    auto hwnd = (HWND)owner.context();
    if(!hwnd || !IsWindow(hwnd)) {
      fastLinkDebug("NativeVulkan::initialize failed: invalid hwnd=%p", (void*)hwnd);
      return false;
    }

    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "bsnes-fastlink";
    appInfo.apiVersion = VK_API_VERSION_1_0;

    const char* instanceExtensions[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = sizeof(instanceExtensions) / sizeof(instanceExtensions[0]);
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    fastLinkDebug("NativeVulkan::initialize vkCreateInstance begin");
    auto result = vkCreateInstance(&instanceInfo, nullptr, &_instance);
    if(result != VK_SUCCESS) {
      fastLinkDebug("NativeVulkan::initialize vkCreateInstance failed result=%d", result);
      return false;
    }
    fastLinkDebug("NativeVulkan::initialize vkCreateInstance ok");

    VkWin32SurfaceCreateInfoKHR surfaceInfo = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    surfaceInfo.hwnd = hwnd;
    fastLinkDebug("NativeVulkan::initialize vkCreateWin32SurfaceKHR begin");
    result = vkCreateWin32SurfaceKHR(_instance, &surfaceInfo, nullptr, &_surface);
    if(result != VK_SUCCESS) {
      fastLinkDebug("NativeVulkan::initialize vkCreateWin32SurfaceKHR failed result=%d", result);
      return false;
    }
    fastLinkDebug("NativeVulkan::initialize vkCreateWin32SurfaceKHR ok");

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr);
    fastLinkDebug("NativeVulkan::initialize physicalDeviceCount=%lu", (unsigned long)deviceCount);
    if(!deviceCount) return false;

    VkPhysicalDevice devices[16] = {};
    if(deviceCount > 16) deviceCount = 16;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, devices);

    for(uint32_t deviceIndex = 0; deviceIndex < deviceCount; deviceIndex++) {
      auto candidate = devices[deviceIndex];

      uint32_t extensionCount = 0;
      vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr);
      if(!extensionCount) continue;

      VkExtensionProperties extensions[64] = {};
      if(extensionCount > 64) extensionCount = 64;
      vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions);

      bool hasSwapchain = false;
      for(uint32_t extensionIndex = 0; extensionIndex < extensionCount; extensionIndex++) {
        if(!strcmp(extensions[extensionIndex].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
          hasSwapchain = true;
          break;
        }
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
        break;
      }

      if(_physicalDevice) break;
    }

    if(!_physicalDevice || _queueFamily == UINT32_MAX) {
      fastLinkDebug("NativeVulkan::initialize failed: no graphics/present queue");
      return false;
    }
    fastLinkDebug("NativeVulkan::initialize selected queueFamily=%lu", (unsigned long)_queueFamily);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = _queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    fastLinkDebug("NativeVulkan::initialize vkCreateDevice begin");
    result = vkCreateDevice(_physicalDevice, &deviceInfo, nullptr, &_device);
    if(result != VK_SUCCESS) {
      fastLinkDebug("NativeVulkan::initialize vkCreateDevice failed result=%d", result);
      return false;
    }
    fastLinkDebug("NativeVulkan::initialize vkCreateDevice ok");

    vkGetDeviceQueue(_device, _queueFamily, 0, &_queue);

    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = _queueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    result = vkCreateCommandPool(_device, &poolInfo, nullptr, &_commandPool);
    if(result != VK_SUCCESS) {
      fastLinkDebug("NativeVulkan::initialize vkCreateCommandPool failed result=%d", result);
      return false;
    }

    if(!createFrameSync()) {
      fastLinkDebug("NativeVulkan::initialize createFrameSync failed");
      return false;
    }

    if(!createSwapchain()) {
      fastLinkDebug("NativeVulkan::initialize createSwapchain failed");
      return false;
    }
    if(!createRenderPass()) {
      fastLinkDebug("NativeVulkan::initialize createRenderPass failed");
      return false;
    }
    if(!createFramebuffers()) {
      fastLinkDebug("NativeVulkan::initialize createFramebuffers failed");
      return false;
    }
    if(!createSampler()) {
      fastLinkDebug("NativeVulkan::initialize createSampler failed");
      return false;
    }
    if(!createDescriptors()) {
      fastLinkDebug("NativeVulkan::initialize createDescriptors failed");
      return false;
    }
    if(!createPipeline()) {
      fastLinkDebug("NativeVulkan::initialize createPipeline failed");
      return false;
    }
    if(!createCommandBuffers()) {
      fastLinkDebug("NativeVulkan::initialize createCommandBuffers failed");
      return false;
    }

    owner._lastSize = owner.currentClientSize();
    fastLinkDebug(
      "NativeVulkan::initialize done swap=%lux%lu images=%lu present=%s swapSetting=%u",
      (unsigned long)_swapExtent.width,
      (unsigned long)_swapExtent.height,
      (unsigned long)_imageCount,
      (const char*)owner.presentMode(),
      owner.swapchainImages()
    );
    return true;
  }

  auto terminate() -> void {
    if(_instance || _device || _swapchain) fastLinkDebug("NativeVulkan::terminate begin device=%d swapchain=%d images=%lu", _device != VK_NULL_HANDLE, _swapchain != VK_NULL_HANDLE, (unsigned long)_imageCount);
    if(_device) vkDeviceWaitIdle(_device);

    destroyTexture();

    if(_device && _imageCount && _commandPool) {
      vkFreeCommandBuffers(_device, _commandPool, _imageCount, _commandBuffers);
    }

    for(uint i = 0; i < _imageCount; i++) {
      if(_framebuffers[i]) vkDestroyFramebuffer(_device, _framebuffers[i], nullptr);
      if(_swapViews[i]) vkDestroyImageView(_device, _swapViews[i], nullptr);
      _framebuffers[i] = VK_NULL_HANDLE;
      _swapViews[i] = VK_NULL_HANDLE;
      _commandBuffers[i] = VK_NULL_HANDLE;
      _descriptorSets[i] = VK_NULL_HANDLE;
      _swapImages[i] = VK_NULL_HANDLE;
    }

    if(_pipeline) vkDestroyPipeline(_device, _pipeline, nullptr);
    if(_pipelineLayout) vkDestroyPipelineLayout(_device, _pipelineLayout, nullptr);
    if(_descriptorPool) vkDestroyDescriptorPool(_device, _descriptorPool, nullptr);
    if(_descriptorSetLayout) vkDestroyDescriptorSetLayout(_device, _descriptorSetLayout, nullptr);
    if(_sampler) vkDestroySampler(_device, _sampler, nullptr);
    if(_renderPass) vkDestroyRenderPass(_device, _renderPass, nullptr);
    for(uint i = 0; i < _maxImages; i++) {
      if(_fences[i]) vkDestroyFence(_device, _fences[i], nullptr);
      if(_acquireSemaphores[i]) vkDestroySemaphore(_device, _acquireSemaphores[i], nullptr);
      if(_renderSemaphores[i]) vkDestroySemaphore(_device, _renderSemaphores[i], nullptr);
      _fences[i] = VK_NULL_HANDLE;
      _acquireSemaphores[i] = VK_NULL_HANDLE;
      _renderSemaphores[i] = VK_NULL_HANDLE;
    }

    if(_commandPool) vkDestroyCommandPool(_device, _commandPool, nullptr);
    if(_swapchain) vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    if(_surface) vkDestroySurfaceKHR(_instance, _surface, nullptr);
    if(_device) vkDestroyDevice(_device, nullptr);
    if(_instance) vkDestroyInstance(_instance, nullptr);

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
    _sampler = VK_NULL_HANDLE;
    _commandPool = VK_NULL_HANDLE;
    _queueFamily = UINT32_MAX;
    _imageCount = 0;
    _frameIndex = 0;
    _stagingFrameIndex = 0;
    _stagingFramePending = false;
    _preparedFrameIndex = 0;
    _preparedImageIndex = 0;
    _framePrepared = false;
    _swapExtent = {};
    fastLinkDebug("NativeVulkan::terminate done");
  }

  auto clear() -> void {
    if(_mappedStaging && _bufferWidth && _bufferHeight) {
      memset(_mappedStaging, 0, _bufferWidth * _bufferHeight * 4);
    }
  }

  auto updateSampler() -> bool {
    if(!_device) return false;
    vkDeviceWaitIdle(_device);
    if(_sampler) {
      vkDestroySampler(_device, _sampler, nullptr);
      _sampler = VK_NULL_HANDLE;
    }
    if(!createSampler()) return false;
    return refreshDescriptorSets();
  }

  auto acquire(uint width, uint height) -> NativeVideoManager::Acquire {
    #if defined(BUILD_DEBUG)
    static uint acquireTraceCounter = 0;
    bool traceAcquire = acquireTraceCounter++ < 80;
    #else
    constexpr bool traceAcquire = false;
    #endif
    if(traceAcquire) fastLinkDebug("NativeVulkan::acquire begin width=%u height=%u readyDevice=%d", width, height, _device != VK_NULL_HANDLE);
    NativeVideoManager::Acquire result;
    // The frontend writes directly into one persistently mapped upload
    // buffer. Do not hand it back until the last submission that copied from
    // it has completed. This caps host/GPU work at one emulated frame without
    // blocking the later presentation operation.
    if(!prepareUpload()) return result;
    if(!ensureTexture(width, height)) {
      if(traceAcquire) fastLinkDebug("NativeVulkan::acquire ensureTexture failed");
      return result;
    }
    result.data = (uint32_t*)_mappedStaging;
    result.pitch = width * sizeof(uint32_t);
    if(traceAcquire) fastLinkDebug("NativeVulkan::acquire done data=%p pitch=%u", (void*)result.data, result.pitch);
    return result;
  }

  auto prepareUpload() -> bool {
    if(!_device) return false;
    if(!_stagingFramePending) return true;
    // Never let GPU scheduling stall the emulation/audio producer.  The main
    // loop can run an audio-only frame and try this upload slot again on its
    // next iteration.
    auto waitResult = vkGetFenceStatus(_device, _fences[_stagingFrameIndex]);
    if(waitResult == VK_NOT_READY) return false;
    if(waitResult != VK_SUCCESS) return false;
    _stagingFramePending = false;
    return true;
  }

  auto prepareFrame() -> bool {
    if(!prepareUpload() || !_swapchain || !_imageCount) return false;
    if(_framePrepared) return true;

    auto clientSize = owner.currentClientSize();
    if(!clientSize.width || !clientSize.height) return false;

    uint32_t frameIndex = _frameIndex % _imageCount;
    if(vkGetFenceStatus(_device, _fences[frameIndex]) != VK_SUCCESS) return false;

    uint32_t imageIndex = 0;
    auto result = vkAcquireNextImageKHR(
      _device, _swapchain, 0,
      _acquireSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex
    );
    if(result == VK_NOT_READY || result == VK_TIMEOUT) return false;
    if(result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
      recreateSwapchain();
      return false;
    }
    if(result != VK_SUCCESS) return false;

    _preparedFrameIndex = frameIndex;
    _preparedImageIndex = imageIndex;
    _framePrepared = true;
    return true;
  }

  auto output(uint width, uint height) -> void {
    #if defined(BUILD_DEBUG)
    static uint64 outputTraceCounter = 0;
    outputTraceCounter++;
    bool traceOutput = outputTraceCounter <= 120;
    #else
    constexpr uint64 outputTraceCounter = 0;
    constexpr bool traceOutput = false;
    #endif
    if(traceOutput) {
      fastLinkDebug(
        "NativeVulkan::output[%llu] begin width=%u height=%u device=%d swapchain=%d mapped=%p tex=%d imageCount=%lu frameIndex=%lu",
        (unsigned long long)outputTraceCounter,
        width,
        height,
        _device != VK_NULL_HANDLE,
        _swapchain != VK_NULL_HANDLE,
        _mappedStaging,
        _texImage != VK_NULL_HANDLE,
        (unsigned long)_imageCount,
        (unsigned long)_frameIndex
      );
    }
    if(!_device || !_swapchain || !_mappedStaging || !_texImage) {
      if(traceOutput) fastLinkDebug("NativeVulkan::output[%llu] skipped: missing resources", (unsigned long long)outputTraceCounter);
      return;
    }

    auto clientSize = owner.currentClientSize();
    if(!clientSize.width || !clientSize.height) {
      if(traceOutput) fastLinkDebug("NativeVulkan::output[%llu] skipped: empty client", (unsigned long long)outputTraceCounter);
      return;
    }

    if(!_imageCount) {
      if(traceOutput) fastLinkDebug("NativeVulkan::output[%llu] skipped: no swap images", (unsigned long long)outputTraceCounter);
      return;
    }

    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    VkResult result = VK_SUCCESS;
    if(_framePrepared) {
      frameIndex = _preparedFrameIndex;
      imageIndex = _preparedImageIndex;
      _framePrepared = false;
    } else {
      frameIndex = _frameIndex % _imageCount;
      if(vkGetFenceStatus(_device, _fences[frameIndex]) != VK_SUCCESS) return;
      result = vkAcquireNextImageKHR(_device, _swapchain, 0, _acquireSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex);
      if(result == VK_NOT_READY || result == VK_TIMEOUT) return;
      if(result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
        return;
      }
      if(result != VK_SUCCESS) return;
    }

    if(!_stagingCoherent && _stagingMemory && _stagingSize) {
      VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = _stagingMemory;
      range.offset = 0;
      range.size = _stagingSize;
      vkFlushMappedMemoryRanges(_device, 1, &range);
    }

    recordCommandBuffer(imageIndex, width, height);
    vkResetFences(_device, 1, &_fences[frameIndex]);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &_acquireSemaphores[frameIndex];
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &_commandBuffers[imageIndex];
    submitInfo.signalSemaphoreCount = 1;
    // A render-finished semaphore follows the swapchain image rather than the
    // CPU frame slot. Reacquiring an image guarantees its previous present has
    // released that image and consumed the corresponding semaphore wait.
    submitInfo.pSignalSemaphores = &_renderSemaphores[imageIndex];
    result = vkQueueSubmit(_queue, 1, &submitInfo, _fences[frameIndex]);
    if(traceOutput) fastLinkDebug("NativeVulkan::output[%llu] queueSubmit result=%d", (unsigned long long)outputTraceCounter, result);
    if(result != VK_SUCCESS) {
      recoverFrameFence(frameIndex);
      return;
    }
    _stagingFrameIndex = frameIndex;
    _stagingFramePending = true;

    VkPresentInfoKHR presentInfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &_renderSemaphores[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.pImageIndices = &imageIndex;
    result = vkQueuePresentKHR(_queue, &presentInfo);
    _frameIndex = (_frameIndex + 1) % _maxImages;
    if(traceOutput) fastLinkDebug("NativeVulkan::output[%llu] present result=%d nextFrameIndex=%lu", (unsigned long long)outputTraceCounter, result, (unsigned long)_frameIndex);

    if(result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
      if(traceOutput) fastLinkDebug("NativeVulkan::output[%llu] recreate after present result=%d", (unsigned long long)outputTraceCounter, result);
      recreateSwapchain();
      return;
    }
    if(result != VK_SUCCESS) return;

    if(owner.flush()) {
      vkWaitForFences(_device, 1, &_fences[frameIndex], VK_TRUE, UINT64_MAX);
    }
    if(traceOutput) fastLinkDebug("NativeVulkan::output[%llu] done", (unsigned long long)outputTraceCounter);
  }

  auto createSwapchain() -> bool {
    fastLinkDebug("NativeVulkan::createSwapchain begin requestedImages=%u present=%s", owner.swapchainImages(), (const char*)owner.presentMode());
    VkSurfaceCapabilitiesKHR capabilities = {};
    auto result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, _surface, &capabilities);
    if(result != VK_SUCCESS) {
      fastLinkDebug("NativeVulkan::createSwapchain surface capabilities failed result=%d", result);
      return false;
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &formatCount, nullptr);
    if(!formatCount) {
      fastLinkDebug("NativeVulkan::createSwapchain failed: no surface formats");
      return false;
    }

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
    fastLinkDebug(
      "NativeVulkan::createSwapchain extent=%lux%lu minImages=%lu maxImages=%lu",
      (unsigned long)_swapExtent.width,
      (unsigned long)_swapExtent.height,
      (unsigned long)capabilities.minImageCount,
      (unsigned long)capabilities.maxImageCount
    );

    uint32_t requestedImages = owner.swapchainImages();
    if(requestedImages < 2) requestedImages = 2;
    if(requestedImages > _maxImages) requestedImages = _maxImages;
    uint32_t minImages = capabilities.minImageCount;
    if(minImages < requestedImages) minImages = requestedImages;
    if(capabilities.maxImageCount && minImages > capabilities.maxImageCount) {
      minImages = capabilities.maxImageCount;
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(_physicalDevice, _surface, &presentModeCount, nullptr);
    VkPresentModeKHR presentModes[16] = {};
    if(presentModeCount > 16) presentModeCount = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(_physicalDevice, _surface, &presentModeCount, presentModes);

    VkPresentModeKHR presentMode = choosePresentMode(presentModes, presentModeCount, owner.presentMode());
    fastLinkDebug("NativeVulkan::createSwapchain presentModes=%lu chosen=%d", (unsigned long)presentModeCount, presentMode);

    auto oldSwapchain = _swapchain;

    VkSwapchainCreateInfoKHR swapInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
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
    if(result != VK_SUCCESS) {
      fastLinkDebug("NativeVulkan::createSwapchain vkCreateSwapchainKHR failed result=%d", result);
      return false;
    }

    _swapchain = newSwapchain;
    if(oldSwapchain) vkDestroySwapchainKHR(_device, oldSwapchain, nullptr);

    vkGetSwapchainImagesKHR(_device, _swapchain, &_imageCount, nullptr);
    if(_imageCount > _maxImages) _imageCount = _maxImages;
    vkGetSwapchainImagesKHR(_device, _swapchain, &_imageCount, _swapImages);
    fastLinkDebug("NativeVulkan::createSwapchain done images=%lu", (unsigned long)_imageCount);
    return true;
  }

  auto recreateSwapchain() -> bool {
    if(!_device) return false;
    fastLinkDebug("NativeVulkan::recreateSwapchain begin");
    vkDeviceWaitIdle(_device);

    if(_imageCount && _commandPool) {
      vkFreeCommandBuffers(_device, _commandPool, _imageCount, _commandBuffers);
      for(uint i = 0; i < _imageCount; i++) _commandBuffers[i] = VK_NULL_HANDLE;
    }

    for(uint i = 0; i < _imageCount; i++) {
      if(_framebuffers[i]) vkDestroyFramebuffer(_device, _framebuffers[i], nullptr);
      if(_swapViews[i]) vkDestroyImageView(_device, _swapViews[i], nullptr);
      _framebuffers[i] = VK_NULL_HANDLE;
      _swapViews[i] = VK_NULL_HANDLE;
      _descriptorSets[i] = VK_NULL_HANDLE;
    }

    if(_descriptorPool) {
      vkDestroyDescriptorPool(_device, _descriptorPool, nullptr);
      _descriptorPool = VK_NULL_HANDLE;
    }

    _imageCount = 0;
    bool rebuilt = createSwapchain()
      && createFramebuffers()
      && createDescriptors()
      && refreshDescriptorSets()
      && createCommandBuffers();
    _stagingFramePending = false;
    _frameIndex = 0;
    _framePrepared = false;
    fastLinkDebug("NativeVulkan::recreateSwapchain done images=%lu", (unsigned long)_imageCount);
    return rebuilt;
  }

  auto recoverFrameFence(uint32_t frameIndex) -> void {
    if(!_device || frameIndex >= _maxImages) return;
    vkDeviceWaitIdle(_device);
    if(_fences[frameIndex]) vkDestroyFence(_device, _fences[frameIndex], nullptr);
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    _fences[frameIndex] = VK_NULL_HANDLE;
    vkCreateFence(_device, &fenceInfo, nullptr, &_fences[frameIndex]);
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
    poolSize.descriptorCount = _maxImages;

    VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = _maxImages;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if(vkCreateDescriptorPool(_device, &poolInfo, nullptr, &_descriptorPool) != VK_SUCCESS) return false;

    VkDescriptorSetLayout layouts[_maxImages] = {};
    for(uint i = 0; i < _imageCount; i++) layouts[i] = _descriptorSetLayout;

    VkDescriptorSetAllocateInfo allocateInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = _descriptorPool;
    allocateInfo.descriptorSetCount = _imageCount;
    allocateInfo.pSetLayouts = layouts;
    if(vkAllocateDescriptorSets(_device, &allocateInfo, _descriptorSets) != VK_SUCCESS) return false;
    return true;
  }

  auto refreshDescriptorSets() -> bool {
    if(!_descriptorPool || !_texView || !_sampler) return true;

    for(uint i = 0; i < _imageCount; i++) {
      VkDescriptorImageInfo imageInfo = {};
      imageInfo.sampler = _sampler;
      imageInfo.imageView = _texView;
      imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = _descriptorSets[i];
      write.dstBinding = 0;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.pImageInfo = &imageInfo;
      vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
    }

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
    if(_imageCount > _maxImages) return false;
    VkCommandBufferAllocateInfo allocateInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = _commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = _imageCount;
    return vkAllocateCommandBuffers(_device, &allocateInfo, _commandBuffers) == VK_SUCCESS;
  }

  auto createFrameSync() -> bool {
    VkSemaphoreCreateInfo semaphoreInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for(uint i = 0; i < _maxImages; i++) {
      if(vkCreateSemaphore(_device, &semaphoreInfo, nullptr, &_acquireSemaphores[i]) != VK_SUCCESS) return false;
      if(vkCreateSemaphore(_device, &semaphoreInfo, nullptr, &_renderSemaphores[i]) != VK_SUCCESS) return false;
      if(vkCreateFence(_device, &fenceInfo, nullptr, &_fences[i]) != VK_SUCCESS) return false;
    }

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
    #if defined(BUILD_DEBUG)
    static uint textureTraceCounter = 0;
    bool traceTexture = textureTraceCounter++ < 20;
    #else
    constexpr bool traceTexture = false;
    #endif
    if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture begin width=%u height=%u old=%ux%u", width, height, _texWidth, _texHeight);

    if(_device && (_texImage || _stagingBuffer)) {
      fastLinkDebug("NativeVulkan::ensureTexture vkDeviceWaitIdle before texture resize");
      vkDeviceWaitIdle(_device);
    }
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
    if(result != VK_SUCCESS) {
      if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture vkCreateImage failed result=%d", result);
      return false;
    }

    VkMemoryRequirements imageMemoryRequirements = {};
    vkGetImageMemoryRequirements(_device, _texImage, &imageMemoryRequirements);
    VkMemoryAllocateInfo imageAllocateInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    imageAllocateInfo.allocationSize = imageMemoryRequirements.size;
    imageAllocateInfo.memoryTypeIndex = findMemoryType(imageMemoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(imageAllocateInfo.memoryTypeIndex == UINT32_MAX) {
      if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture no device-local memory type for image");
      return false;
    }
    result = vkAllocateMemory(_device, &imageAllocateInfo, nullptr, &_texMemory);
    if(result != VK_SUCCESS) {
      if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture vkAllocateMemory image failed result=%d size=%llu", result, (unsigned long long)imageAllocateInfo.allocationSize);
      return false;
    }
    result = vkBindImageMemory(_device, _texImage, _texMemory, 0);
    if(result != VK_SUCCESS) {
      if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture vkBindImageMemory failed result=%d", result);
      return false;
    }

    VkImageViewCreateInfo imageViewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    imageViewInfo.image = _texImage;
    imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    result = vkCreateImageView(_device, &imageViewInfo, nullptr, &_texView);
    if(result != VK_SUCCESS) {
      if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture vkCreateImageView failed result=%d", result);
      return false;
    }

    VkDeviceSize stagingSize = width * height * 4;
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = stagingSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    result = vkCreateBuffer(_device, &bufferInfo, nullptr, &_stagingBuffer);
    if(result != VK_SUCCESS) {
      if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture vkCreateBuffer failed result=%d stagingSize=%llu", result, (unsigned long long)stagingSize);
      return false;
    }

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
    }
    if(stagingAllocateInfo.memoryTypeIndex == UINT32_MAX) {
      if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture no host-visible memory type for staging");
      return false;
    }
    result = vkAllocateMemory(_device, &stagingAllocateInfo, nullptr, &_stagingMemory);
    if(result != VK_SUCCESS) {
      if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture vkAllocateMemory staging failed result=%d size=%llu", result, (unsigned long long)stagingAllocateInfo.allocationSize);
      return false;
    }
    result = vkBindBufferMemory(_device, _stagingBuffer, _stagingMemory, 0);
    if(result != VK_SUCCESS) {
      if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture vkBindBufferMemory failed result=%d", result);
      return false;
    }
    result = vkMapMemory(_device, _stagingMemory, 0, stagingMemoryRequirements.size, 0, &_mappedStaging);
    if(result != VK_SUCCESS) {
      if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture vkMapMemory failed result=%d", result);
      _mappedStaging = nullptr;
      return false;
    }

    _stagingSize = stagingMemoryRequirements.size;
    _bufferWidth = width;
    _bufferHeight = height;
    _textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if(traceTexture) fastLinkDebug("NativeVulkan::ensureTexture done stagingSize=%llu mapped=%p coherent=%d", (unsigned long long)_stagingSize, _mappedStaging, _stagingCoherent);
    return refreshDescriptorSets();
  }

  auto destroyTexture() -> void {
    if(_mappedStaging && _stagingMemory) {
      vkUnmapMemory(_device, _stagingMemory);
      _mappedStaging = nullptr;
    }
    if(_texView) vkDestroyImageView(_device, _texView, nullptr);
    if(_texImage) vkDestroyImage(_device, _texImage, nullptr);
    if(_texMemory) vkFreeMemory(_device, _texMemory, nullptr);
    if(_stagingBuffer) vkDestroyBuffer(_device, _stagingBuffer, nullptr);
    if(_stagingMemory) vkFreeMemory(_device, _stagingMemory, nullptr);

    _texView = VK_NULL_HANDLE;
    _texImage = VK_NULL_HANDLE;
    _texMemory = VK_NULL_HANDLE;
    _stagingBuffer = VK_NULL_HANDLE;
    _stagingMemory = VK_NULL_HANDLE;
    _stagingSize = 0;
    _texWidth = 0;
    _texHeight = 0;
    _bufferWidth = 0;
    _bufferHeight = 0;
    _stagingCoherent = true;
    _textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  auto recordCommandBuffer(uint32_t imageIndex, uint outputWidth, uint outputHeight) -> void {
    auto commandBuffer = _commandBuffers[imageIndex];
    vkResetCommandBuffer(commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

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

    if(!outputWidth || outputWidth > _swapExtent.width) outputWidth = _swapExtent.width;
    if(!outputHeight || outputHeight > _swapExtent.height) outputHeight = _swapExtent.height;

    int32_t viewportX = (int32_t)(_swapExtent.width - outputWidth) / 2;
    int32_t viewportY = (int32_t)(_swapExtent.height - outputHeight) / 2;

    VkViewport viewport = {(float)viewportX, (float)viewportY, (float)outputWidth, (float)outputHeight, 0.0f, 1.0f};
    VkRect2D scissor = {{viewportX, viewportY}, {outputWidth, outputHeight}};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipelineLayout, 0, 1, &_descriptorSets[imageIndex], 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);
    vkEndCommandBuffer(commandBuffer);
    _textureLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  NativeVideoManager& owner;

  void* _mappedStaging = nullptr;
  uint _bufferWidth = 0;
  uint _bufferHeight = 0;

  VkInstance _instance = VK_NULL_HANDLE;
  VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
  VkDevice _device = VK_NULL_HANDLE;
  VkQueue _queue = VK_NULL_HANDLE;
  VkSurfaceKHR _surface = VK_NULL_HANDLE;
  VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
  uint32_t _queueFamily = UINT32_MAX;

  static constexpr uint _maxImages = 4;
  VkImage _swapImages[_maxImages] = {};
  VkImageView _swapViews[_maxImages] = {};
  VkFramebuffer _framebuffers[_maxImages] = {};
  VkCommandBuffer _commandBuffers[_maxImages] = {};
  VkDescriptorSet _descriptorSets[_maxImages] = {};
  VkFence _fences[_maxImages] = {};
  VkSemaphore _acquireSemaphores[_maxImages] = {};
  VkSemaphore _renderSemaphores[_maxImages] = {};
  uint32_t _imageCount = 0;
  uint32_t _frameIndex = 0;
  uint32_t _stagingFrameIndex = 0;
  bool _stagingFramePending = false;
  uint32_t _preparedFrameIndex = 0;
  uint32_t _preparedImageIndex = 0;
  bool _framePrepared = false;
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
  uint _texWidth = 0;
  uint _texHeight = 0;
  bool _stagingCoherent = true;
  VkImageLayout _textureLayout = VK_IMAGE_LAYOUT_UNDEFINED;

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
  if(_impl) {
    _impl->terminate();
    _impl.reset();
  }

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
  _update = {};
}

auto NativeVideoManager::create(string driver) -> bool {
  if(!driver) driver = optimalDriver();
  _driver = driver;
  _ready = false;

  if(driver == "None") {
    if(_impl) {
      _impl->terminate();
      _impl.reset();
    }
    return true;
  }

  return initialize();
}

auto NativeVideoManager::setFullScreen(bool fullScreen) -> bool {
  _fullScreen = fullScreen;
  return false;
}

auto NativeVideoManager::setMonitor(string monitor) -> bool {
  if(!hasMonitor(monitor)) return false;
  _monitor = monitor == "Primary" ? defaultMonitorName() : monitor;
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
  _presentMode = presentMode;
  if(_impl && _ready) _impl->recreateSwapchain();
  return true;
}

auto NativeVideoManager::setSwapchainImages(uint swapchainImages) -> bool {
  if(swapchainImages < 2) swapchainImages = 2;
  if(swapchainImages > NativeVideoImplementation::_maxImages) swapchainImages = NativeVideoImplementation::_maxImages;
  if(_swapchainImages == swapchainImages) return true;
  _swapchainImages = swapchainImages;
  if(_impl && _ready) _impl->recreateSwapchain();
  return true;
}

auto NativeVideoManager::setFormat(string format) -> bool {
  if(format != "ARGB24") return false;
  _format = format;
  return true;
}

auto NativeVideoManager::setShader(string shader) -> bool {
  if(shader != "None" && shader != "Blur") return false;
  _shader = shader;
  if(_impl && _ready) return _impl->updateSampler();
  return true;
}

auto NativeVideoManager::focused() -> bool {
  auto hwnd = (HWND)_context;
  if(!hwnd || !IsWindow(hwnd)) return true;
  auto root = GetAncestor(hwnd, GA_ROOT);
  return GetForegroundWindow() == root;
}

auto NativeVideoManager::clear() -> void {
  if(_impl) _impl->clear();
}

auto NativeVideoManager::size() -> Size {
  return currentClientSize();
}

auto NativeVideoManager::acquire(uint width, uint height) -> Acquire {
  if(!_ready || !_impl) return {};
  return _impl->acquire(width, height);
}

auto NativeVideoManager::release() -> void {
}

auto NativeVideoManager::output(uint width, uint height) -> void {
  if(!_ready || !_impl) return;
  _impl->output(width, height);
}

auto NativeVideoManager::prepare() -> bool {
  if(!_ready || !_impl) return false;
  return _impl->prepareFrame();
}

auto NativeVideoManager::poll() -> void {
  auto size = currentClientSize();
  if(size.width == _lastSize.width && size.height == _lastSize.height) return;
  _lastSize = size;
  doUpdate(size.width, size.height);
}

auto NativeVideoManager::onUpdate(const function<void (uint, uint)>& callback) -> void {
  _update = callback;
}

auto NativeVideoManager::doUpdate(uint width, uint height) -> void {
  if(_update) _update(width, height);
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
  auto hwnd = (HWND)_context;
  if(!hwnd || !IsWindow(hwnd)) return {};

  RECT rect = {};
  if(!GetClientRect(hwnd, &rect)) return {};
  return {(uint)(rect.right - rect.left), (uint)(rect.bottom - rect.top)};
}

#endif

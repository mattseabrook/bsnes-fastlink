//fastlink: Vulkan video driver for bsnes ruby layer
//Lowest-latency present path: Immediate > Mailbox > FIFO
//Uses the raw Vulkan C API with no vulkan-hpp dependency.

#if defined(DISPLAY_XORG)
  #define VK_USE_PLATFORM_XLIB_KHR
#endif
#if defined(DISPLAY_WINDOWS)
  #define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>
#include <cstdarg>
#include <cstring>
#include <cstdio>

namespace {
#if defined(VK_USE_PLATFORM_WIN32_KHR) && defined(BUILD_DEBUG)
auto VideoVulkanTrace(const char* format, ...) -> void {
}
#else
auto VideoVulkanTrace(const char* format, ...) -> void {
}
#endif
}

struct VideoVulkan : VideoDriver {
  VideoVulkan(Video& super) : VideoDriver(super) {}
  ~VideoVulkan() { terminate(); }

  auto create() -> bool override {
    VideoVulkanTrace("create: context=%p flush=%d\n", (void*)context, flush);
    auto result = initialize();
    VideoVulkanTrace("create: result=%d ready=%d instance=%p device=%p swapchain=%p\n", result, _ready, _instance, _device, _swapchain);
    return result;
  }

  auto driver() -> string override { return "Vulkan"; }
  auto ready() -> bool override { return _ready; }

  auto hasFullScreen() -> bool override { return false; }
  auto hasContext() -> bool override { return true; }
  auto hasBlocking() -> bool override { return false; }
  auto hasFlush() -> bool override { return true; }

  auto setContext(uintptr context) -> bool override {
    if(this->context == context) return true;
    VideoVulkanTrace("setContext: old=%p new=%p\n", (void*)this->context, (void*)context);
    this->context = context;
    auto result = initialize();
    VideoVulkanTrace("setContext: initialize result=%d ready=%d\n", result, _ready);
    return result;
  }

  auto setFlush(bool flush) -> bool override {
    this->flush = flush;
    return true;
  }

  auto setPresentMode(string presentMode) -> bool override {
    if(presentMode != "Immediate" && presentMode != "Mailbox" && presentMode != "FIFO") return false;
    this->presentMode = presentMode;
    if(_ready) recreateSwapchain();
    return true;
  }

  auto setSwapchainImages(uint swapchainImages) -> bool override {
    if(swapchainImages < 2) swapchainImages = 2;
    if(swapchainImages > _maxImages) swapchainImages = _maxImages;
    this->swapchainImages = swapchainImages;
    if(_ready) recreateSwapchain();
    return true;
  }

  auto clear() -> void override {
    if(!_ready) return;
    if(_mappedStaging) {
      memset(_mappedStaging, 0, _bufferWidth * _bufferHeight * 4);
    }
  }

  auto size(uint& width, uint& height) -> void override {
    //query current surface size
    if(_surface && _physicalDevice) {
      VkSurfaceCapabilitiesKHR caps;
      vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, _surface, &caps);
      width = caps.currentExtent.width;
      height = caps.currentExtent.height;
    }
  }

  auto acquire(uint32_t*& data, uint& pitch, uint width, uint height) -> bool override {
    if(!_ready) return false;

    //ensure staging buffer exists at correct size
    //this persistently maps the staging buffer — filterRender writes directly into GPU-visible memory
    if(_bufferWidth != width || _bufferHeight != height) {
      ensureTexture(width, height);
      _bufferWidth = width;
      _bufferHeight = height;
    }

    if(!_mappedStaging) return false;
    data = (uint32_t*)_mappedStaging;
    pitch = width * sizeof(uint32_t);
    return true;
  }

  auto release() -> void override {
    //noop — staging buffer stays persistently mapped
  }

  auto output(uint width, uint height) -> void override {
    if(!_ready || !_mappedStaging) return;
    static uint32_t debugOutputCount = 0;
    bool traceOutput = debugOutputCount < 8;
    if(traceOutput) VideoVulkanTrace("output[%u]: begin width=%u height=%u swapchain=%p\n", debugOutputCount, width, height, _swapchain);

    //acquire swapchain image
    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(_device, _swapchain, 1000000, _acquireSemaphore, VK_NULL_HANDLE, &imageIndex);
    if(traceOutput) VideoVulkanTrace("output[%u]: vkAcquireNextImageKHR -> %d image=%u\n", debugOutputCount, result, imageIndex);
    if(result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
      recreateSwapchain();
      debugOutputCount++;
      return;
    }
    if(result != VK_SUCCESS) {
      debugOutputCount++;
      return;
    }

    //wait for previous frame on this image
    vkWaitForFences(_device, 1, &_fences[imageIndex], VK_TRUE, UINT64_MAX);
    vkResetFences(_device, 1, &_fences[imageIndex]);

    //zero-copy: pixels are already in the staging buffer (written by filterRender via acquire())
    //just ensure texture + descriptors are up to date
    ensureTexture(_bufferWidth, _bufferHeight);

    //record command buffer
    recordCommandBuffer(imageIndex, width, height);

    //submit
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &_acquireSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &_commandBuffers[imageIndex];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &_renderSemaphore;
    result = vkQueueSubmit(_queue, 1, &submitInfo, _fences[imageIndex]);
    if(traceOutput) VideoVulkanTrace("output[%u]: vkQueueSubmit -> %d\n", debugOutputCount, result);
    if(result != VK_SUCCESS) {
      debugOutputCount++;
      return;
    }

    //present
    VkPresentInfoKHR presentInfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &_renderSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.pImageIndices = &imageIndex;
    result = vkQueuePresentKHR(_queue, &presentInfo);
    if(traceOutput) VideoVulkanTrace("output[%u]: vkQueuePresentKHR -> %d\n", debugOutputCount, result);

    if(result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
      recreateSwapchain();
    }

    //optional per-frame fence wait; avoids draining unrelated queue work
    if(flush) {
      vkWaitForFences(_device, 1, &_fences[imageIndex], VK_TRUE, UINT64_MAX);
    }
    if(traceOutput) VideoVulkanTrace("output[%u]: done\n", debugOutputCount);
    debugOutputCount++;
  }

private:
  bool _ready = false;

  //zero-copy: staging buffer is persistently mapped, acquire() returns this pointer
  void* _mappedStaging = nullptr;
  uint _bufferWidth = 0;
  uint _bufferHeight = 0;

  //Vulkan handles
  VkInstance _instance = VK_NULL_HANDLE;
  VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
  VkDevice _device = VK_NULL_HANDLE;
  VkQueue _queue = VK_NULL_HANDLE;
  VkSurfaceKHR _surface = VK_NULL_HANDLE;
  VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
  uint32_t _queueFamily = 0;

  //swapchain images
  static constexpr uint _maxImages = 4;
  VkImage _swapImages[_maxImages] = {};
  VkImageView _swapViews[_maxImages] = {};
  VkFramebuffer _framebuffers[_maxImages] = {};
  VkFence _fences[_maxImages] = {};
  VkCommandBuffer _commandBuffers[_maxImages] = {};
  uint32_t _imageCount = 0;
  VkExtent2D _swapExtent = {};
  VkFormat _swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
  VkColorSpaceKHR _swapColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

  //render pass & pipeline
  VkRenderPass _renderPass = VK_NULL_HANDLE;
  VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
  VkPipeline _pipeline = VK_NULL_HANDLE;
  VkDescriptorSetLayout _descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet _descriptorSets[_maxImages] = {};
  VkSampler _sampler = VK_NULL_HANDLE;

  //per-frame texture upload
  VkImage _texImage = VK_NULL_HANDLE;
  VkDeviceMemory _texMemory = VK_NULL_HANDLE;
  VkImageView _texView = VK_NULL_HANDLE;
  VkBuffer _stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory _stagingMemory = VK_NULL_HANDLE;
  VkDeviceSize _stagingSize = 0;
  uint _texWidth = 0;
  uint _texHeight = 0;

  //sync
  VkSemaphore _acquireSemaphore = VK_NULL_HANDLE;
  VkSemaphore _renderSemaphore = VK_NULL_HANDLE;
  VkCommandPool _commandPool = VK_NULL_HANDLE;

  auto initialize() -> bool {
    VideoVulkanTrace("initialize: begin context=%p\n", (void*)context);
    terminate();
    VideoVulkanTrace("initialize: previous state cleared\n");

    if(!context) {
      VideoVulkanTrace("initialize: aborted because context is null\n");
      return false;
    }

    //create instance
    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "bsnes-fastlink";
    appInfo.apiVersion = VK_API_VERSION_1_0;

    const char* extensions[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      #if defined(VK_USE_PLATFORM_XLIB_KHR)
      VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
      #endif
      #if defined(VK_USE_PLATFORM_WIN32_KHR)
      VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
      #endif
    };
    uint extCount = sizeof(extensions) / sizeof(extensions[0]);

    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = extCount;
    instanceInfo.ppEnabledExtensionNames = extensions;

    auto result = vkCreateInstance(&instanceInfo, nullptr, &_instance);
    VideoVulkanTrace("initialize: vkCreateInstance -> %d instance=%p\n", result, _instance);
    if(result != VK_SUCCESS) return false;

    //create surface
    #if defined(VK_USE_PLATFORM_XLIB_KHR)
    {
      auto display = XOpenDisplay(nullptr);
      VkXlibSurfaceCreateInfoKHR surfaceInfo = {VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
      surfaceInfo.dpy = display;
      surfaceInfo.window = (Window)context;
      result = vkCreateXlibSurfaceKHR(_instance, &surfaceInfo, nullptr, &_surface);
      VideoVulkanTrace("initialize: vkCreateXlibSurfaceKHR -> %d surface=%p\n", result, _surface);
      if(result != VK_SUCCESS) return false;
    }
    #endif
    #if defined(VK_USE_PLATFORM_WIN32_KHR)
    {
      VkWin32SurfaceCreateInfoKHR surfaceInfo = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
      surfaceInfo.hinstance = GetModuleHandle(nullptr);
      surfaceInfo.hwnd = (HWND)context;
      result = vkCreateWin32SurfaceKHR(_instance, &surfaceInfo, nullptr, &_surface);
      VideoVulkanTrace("initialize: vkCreateWin32SurfaceKHR hwnd=%p -> %d surface=%p\n", (void*)surfaceInfo.hwnd, result, _surface);
      if(result != VK_SUCCESS) return false;
    }
    #endif

    //pick a device/queue that can both render and present to this surface
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr);
    VideoVulkanTrace("initialize: physical device count=%u\n", deviceCount);
    if(!deviceCount) return false;

    VkPhysicalDevice devices[16];
    deviceCount = deviceCount > 16 ? 16 : deviceCount;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, devices);

    _physicalDevice = VK_NULL_HANDLE;
    _queueFamily = UINT32_MAX;
    for(uint32_t deviceIndex = 0; deviceIndex < deviceCount; deviceIndex++) {
      auto candidate = devices[deviceIndex];

      uint32_t extensionCount = 0;
      vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr);
      VkExtensionProperties extensions[64];
      extensionCount = extensionCount > 64 ? 64 : extensionCount;
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
      VkQueueFamilyProperties queueProps[16];
      queueFamilyCount = queueFamilyCount > 16 ? 16 : queueFamilyCount;
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

    if(!_physicalDevice || _queueFamily == UINT32_MAX) return false;

    {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(_physicalDevice, &props);
      VideoVulkanTrace("initialize: selected device=%s queueFamily=%u\n", props.deviceName, _queueFamily);
    }

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

    result = vkCreateDevice(_physicalDevice, &deviceInfo, nullptr, &_device);
    VideoVulkanTrace("initialize: vkCreateDevice -> %d device=%p\n", result, _device);
    if(result != VK_SUCCESS) return false;
    vkGetDeviceQueue(_device, _queueFamily, 0, &_queue);
    VideoVulkanTrace("initialize: queue=%p\n", _queue);

    //create command pool
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = _queueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    result = vkCreateCommandPool(_device, &poolInfo, nullptr, &_commandPool);
    VideoVulkanTrace("initialize: vkCreateCommandPool -> %d pool=%p\n", result, _commandPool);
    if(result != VK_SUCCESS) return false;

    //create sync objects
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    result = vkCreateSemaphore(_device, &semInfo, nullptr, &_acquireSemaphore);
    VideoVulkanTrace("initialize: vkCreateSemaphore acquire -> %d handle=%p\n", result, _acquireSemaphore);
    if(result != VK_SUCCESS) return false;
    result = vkCreateSemaphore(_device, &semInfo, nullptr, &_renderSemaphore);
    VideoVulkanTrace("initialize: vkCreateSemaphore render -> %d handle=%p\n", result, _renderSemaphore);
    if(result != VK_SUCCESS) return false;

    //create swapchain, render pass, pipeline, etc.
    VideoVulkanTrace("initialize: createSwapchain\n");
    if(!createSwapchain()) return false;
    VideoVulkanTrace("initialize: createRenderPass\n");
    if(!createRenderPass()) return false;
    VideoVulkanTrace("initialize: createFramebuffers\n");
    if(!createFramebuffers()) return false;
    VideoVulkanTrace("initialize: createSampler\n");
    if(!createSampler()) return false;
    VideoVulkanTrace("initialize: createDescriptors\n");
    if(!createDescriptors()) return false;
    VideoVulkanTrace("initialize: createPipeline\n");
    if(!createPipeline()) return false;
    VideoVulkanTrace("initialize: createCommandBuffers\n");
    if(!createCommandBuffers()) return false;

    _ready = true;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(_physicalDevice, &props);
    printf("Vulkan: Using device \"%s\"\n", props.deviceName);
    VideoVulkanTrace("initialize: success device=%s extent=%ux%u images=%u\n", props.deviceName, _swapExtent.width, _swapExtent.height, _imageCount);

    return true;
  }

  auto terminate() -> void {
    VideoVulkanTrace("terminate: begin ready=%d instance=%p device=%p surface=%p swapchain=%p images=%u\n", _ready, _instance, _device, _surface, _swapchain, _imageCount);
    _ready = false;

    if(_device) {
      VideoVulkanTrace("terminate: vkDeviceWaitIdle\n");
      vkDeviceWaitIdle(_device);
    }

    VideoVulkanTrace("terminate: destroyTexture\n");
    destroyTexture();
    _bufferWidth = _bufferHeight = 0;

    VideoVulkanTrace("terminate: destroy per-image resources\n");
    for(uint i = 0; i < _imageCount; i++) {
      if(_fences[i]) vkDestroyFence(_device, _fences[i], nullptr);
      if(_framebuffers[i]) vkDestroyFramebuffer(_device, _framebuffers[i], nullptr);
      if(_swapViews[i]) vkDestroyImageView(_device, _swapViews[i], nullptr);
      _fences[i] = VK_NULL_HANDLE;
      _framebuffers[i] = VK_NULL_HANDLE;
      _swapViews[i] = VK_NULL_HANDLE;
    }

    VideoVulkanTrace("terminate: destroy pipeline/layout/descriptors\n");
    if(_pipeline) vkDestroyPipeline(_device, _pipeline, nullptr); _pipeline = VK_NULL_HANDLE;
    if(_pipelineLayout) vkDestroyPipelineLayout(_device, _pipelineLayout, nullptr); _pipelineLayout = VK_NULL_HANDLE;
    if(_descriptorPool) vkDestroyDescriptorPool(_device, _descriptorPool, nullptr); _descriptorPool = VK_NULL_HANDLE;
    if(_descriptorSetLayout) vkDestroyDescriptorSetLayout(_device, _descriptorSetLayout, nullptr); _descriptorSetLayout = VK_NULL_HANDLE;
    if(_sampler) vkDestroySampler(_device, _sampler, nullptr); _sampler = VK_NULL_HANDLE;
    if(_renderPass) vkDestroyRenderPass(_device, _renderPass, nullptr); _renderPass = VK_NULL_HANDLE;
    VideoVulkanTrace("terminate: destroy pool/semaphores/swapchain/device/instance\n");
    if(_commandPool) vkDestroyCommandPool(_device, _commandPool, nullptr); _commandPool = VK_NULL_HANDLE;
    if(_acquireSemaphore) vkDestroySemaphore(_device, _acquireSemaphore, nullptr); _acquireSemaphore = VK_NULL_HANDLE;
    if(_renderSemaphore) vkDestroySemaphore(_device, _renderSemaphore, nullptr); _renderSemaphore = VK_NULL_HANDLE;
    if(_swapchain) vkDestroySwapchainKHR(_device, _swapchain, nullptr); _swapchain = VK_NULL_HANDLE;
    if(_surface) vkDestroySurfaceKHR(_instance, _surface, nullptr); _surface = VK_NULL_HANDLE;
    if(_device) vkDestroyDevice(_device, nullptr); _device = VK_NULL_HANDLE;
    if(_instance) vkDestroyInstance(_instance, nullptr); _instance = VK_NULL_HANDLE;

    _imageCount = 0;
    VideoVulkanTrace("terminate: done\n");
  }

  auto createSwapchain() -> bool {
    VideoVulkanTrace("createSwapchain: begin surface=%p device=%p\n", _surface, _device);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, _surface, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &formatCount, nullptr);
    if(!formatCount) return false;

    VkSurfaceFormatKHR formats[32];
    formatCount = formatCount > 32 ? 32 : formatCount;
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

    _swapExtent = caps.currentExtent;
    if(_swapExtent.width == UINT32_MAX) {
      _swapExtent.width = 960;
      _swapExtent.height = 720;
    }

    uint32_t requestedImages = swapchainImages;
    if(requestedImages < 2) requestedImages = 2;
    if(requestedImages > _maxImages) requestedImages = _maxImages;
    uint32_t minImages = caps.minImageCount;
    if(minImages < requestedImages) minImages = requestedImages;
    if(caps.maxImageCount && minImages > caps.maxImageCount) minImages = caps.maxImageCount;

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(_physicalDevice, _surface, &presentModeCount, nullptr);
    VkPresentModeKHR presentModes[16];
    presentModeCount = presentModeCount > 16 ? 16 : presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(_physicalDevice, _surface, &presentModeCount, presentModes);

    auto hasPresentMode = [&](VkPresentModeKHR mode) -> bool {
      for(uint32_t i = 0; i < presentModeCount; i++) {
        if(presentModes[i] == mode) return true;
      }
      return false;
    };

    VkPresentModeKHR requestedMode = VK_PRESENT_MODE_FIFO_KHR;
    if(presentMode == "Immediate") requestedMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    if(presentMode == "Mailbox") requestedMode = VK_PRESENT_MODE_MAILBOX_KHR;

    VkPresentModeKHR bestMode = VK_PRESENT_MODE_FIFO_KHR;
    if(hasPresentMode(requestedMode)) bestMode = requestedMode;
    else if(hasPresentMode(VK_PRESENT_MODE_MAILBOX_KHR)) bestMode = VK_PRESENT_MODE_MAILBOX_KHR;
    else if(hasPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR)) bestMode = VK_PRESENT_MODE_IMMEDIATE_KHR;

    VkSwapchainCreateInfoKHR swapInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapInfo.surface = _surface;
    swapInfo.minImageCount = minImages;
    swapInfo.imageFormat = _swapFormat;
    swapInfo.imageColorSpace = _swapColorSpace;
    swapInfo.imageExtent = _swapExtent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = bestMode;
    swapInfo.clipped = VK_TRUE;
    swapInfo.oldSwapchain = _swapchain;

    VkSwapchainKHR newSwapchain;
    auto result = vkCreateSwapchainKHR(_device, &swapInfo, nullptr, &newSwapchain);
    VideoVulkanTrace("createSwapchain: vkCreateSwapchainKHR -> %d mode=%d minImages=%u extent=%ux%u\n", result, bestMode, minImages, _swapExtent.width, _swapExtent.height);
    if(result != VK_SUCCESS) return false;

    if(_swapchain) vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    _swapchain = newSwapchain;

    vkGetSwapchainImagesKHR(_device, _swapchain, &_imageCount, nullptr);
    if(_imageCount > _maxImages) _imageCount = _maxImages;
    vkGetSwapchainImagesKHR(_device, _swapchain, &_imageCount, _swapImages);
    VideoVulkanTrace("createSwapchain: success imageCount=%u format=%d colorSpace=%d\n", _imageCount, _swapFormat, _swapColorSpace);

    return true;
  }

  auto recreateSwapchain() -> void {
    VideoVulkanTrace("recreateSwapchain: begin\n");
    vkDeviceWaitIdle(_device);

    if(_imageCount) {
      vkFreeCommandBuffers(_device, _commandPool, _imageCount, _commandBuffers);
      for(uint i = 0; i < _imageCount; i++) {
        _commandBuffers[i] = VK_NULL_HANDLE;
      }
    }

    for(uint i = 0; i < _imageCount; i++) {
      if(_framebuffers[i]) vkDestroyFramebuffer(_device, _framebuffers[i], nullptr);
      if(_swapViews[i]) vkDestroyImageView(_device, _swapViews[i], nullptr);
      if(_fences[i]) vkDestroyFence(_device, _fences[i], nullptr);
      _framebuffers[i] = VK_NULL_HANDLE;
      _swapViews[i] = VK_NULL_HANDLE;
      _fences[i] = VK_NULL_HANDLE;
    }

    createSwapchain();
    createFramebuffers();
    createCommandBuffers();
    VideoVulkanTrace("recreateSwapchain: done\n");
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

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &attachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dep;

    return vkCreateRenderPass(_device, &rpInfo, nullptr, &_renderPass) == VK_SUCCESS;
  }

  auto createFramebuffers() -> bool {
    for(uint i = 0; i < _imageCount; i++) {
      VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = _swapImages[i];
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = _swapFormat;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if(vkCreateImageView(_device, &viewInfo, nullptr, &_swapViews[i]) != VK_SUCCESS) return false;

      VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fbInfo.renderPass = _renderPass;
      fbInfo.attachmentCount = 1;
      fbInfo.pAttachments = &_swapViews[i];
      fbInfo.width = _swapExtent.width;
      fbInfo.height = _swapExtent.height;
      fbInfo.layers = 1;
      if(vkCreateFramebuffer(_device, &fbInfo, nullptr, &_framebuffers[i]) != VK_SUCCESS) return false;

      VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      vkCreateFence(_device, &fenceInfo, nullptr, &_fences[i]);
    }
    return true;
  }

  auto createSampler() -> bool {
    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(_device, &samplerInfo, nullptr, &_sampler) == VK_SUCCESS;
  }

  auto createDescriptors() -> bool {
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if(vkCreateDescriptorSetLayout(_device, &layoutInfo, nullptr, &_descriptorSetLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = _maxImages;

    VkDescriptorPoolCreateInfo dpInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpInfo.maxSets = _maxImages;
    dpInfo.poolSizeCount = 1;
    dpInfo.pPoolSizes = &poolSize;
    if(vkCreateDescriptorPool(_device, &dpInfo, nullptr, &_descriptorPool) != VK_SUCCESS) return false;

    VkDescriptorSetLayout layouts[_maxImages];
    for(uint i = 0; i < _imageCount; i++) layouts[i] = _descriptorSetLayout;

    VkDescriptorSetAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = _descriptorPool;
    allocInfo.descriptorSetCount = _imageCount;
    allocInfo.pSetLayouts = layouts;
    return vkAllocateDescriptorSets(_device, &allocInfo, _descriptorSets) == VK_SUCCESS;
  }

  auto createPipeline() -> bool {
    //Validated fullscreen-triangle shaders compiled with glslangValidator for Vulkan 1.0.
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

    VideoVulkanTrace("createPipeline: begin shader compilation state\n");

    VkShaderModuleCreateInfo vertModInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vertModInfo.codeSize = sizeof(vertSpv);
    vertModInfo.pCode = vertSpv;
    VkShaderModule vertModule;
    auto result = vkCreateShaderModule(_device, &vertModInfo, nullptr, &vertModule);
    VideoVulkanTrace("createPipeline: vkCreateShaderModule(vert) -> %d module=%p\n", result, vertModule);
    if(result != VK_SUCCESS) return false;

    VkShaderModuleCreateInfo fragModInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fragModInfo.codeSize = sizeof(fragSpv);
    fragModInfo.pCode = fragSpv;
    VkShaderModule fragModule;
    result = vkCreateShaderModule(_device, &fragModInfo, nullptr, &fragModule);
    VideoVulkanTrace("createPipeline: vkCreateShaderModule(frag) -> %d module=%p\n", result, fragModule);
    if(result != VK_SUCCESS) {
      vkDestroyShaderModule(_device, vertModule, nullptr);
      return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
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

    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttach = {};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttach.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttach;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates = dynStates;

    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_descriptorSetLayout;
    result = vkCreatePipelineLayout(_device, &layoutInfo, nullptr, &_pipelineLayout);
    VideoVulkanTrace("createPipeline: vkCreatePipelineLayout -> %d layout=%p\n", result, _pipelineLayout);
    if(result != VK_SUCCESS) {
      vkDestroyShaderModule(_device, vertModule, nullptr);
      vkDestroyShaderModule(_device, fragModule, nullptr);
      return false;
    }

    VkGraphicsPipelineCreateInfo pipeInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vertexInput;
    pipeInfo.pInputAssemblyState = &inputAssembly;
    pipeInfo.pViewportState = &viewportState;
    pipeInfo.pRasterizationState = &raster;
    pipeInfo.pMultisampleState = &multisample;
    pipeInfo.pColorBlendState = &blend;
    pipeInfo.pDynamicState = &dynState;
    pipeInfo.layout = _pipelineLayout;
    pipeInfo.renderPass = _renderPass;
    pipeInfo.subpass = 0;

    VideoVulkanTrace("createPipeline: vkCreateGraphicsPipelines begin renderPass=%p extent=%ux%u\n", _renderPass, _swapExtent.width, _swapExtent.height);
    result = vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &_pipeline);
    VideoVulkanTrace("createPipeline: vkCreateGraphicsPipelines -> %d pipeline=%p\n", result, _pipeline);
    vkDestroyShaderModule(_device, vertModule, nullptr);
    vkDestroyShaderModule(_device, fragModule, nullptr);
    return result == VK_SUCCESS;
  }

  auto createCommandBuffers() -> bool {
    if(_imageCount > _maxImages) return false;
    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = _commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = _imageCount;
    return vkAllocateCommandBuffers(_device, &allocInfo, _commandBuffers) == VK_SUCCESS;
  }

  auto findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) -> uint32_t {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &memProps);
    for(uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
      if((typeBits & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return 0;
  }

  auto ensureTexture(uint width, uint height) -> void {
    if(_texWidth == width && _texHeight == height) return;
    destroyTexture();

    _texWidth = width;
    _texHeight = height;

    //create texture image
    VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imgInfo.extent = {width, height, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(_device, &imgInfo, nullptr, &_texImage);

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(_device, _texImage, &memReqs);
    VkMemoryAllocateInfo memAlloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memAlloc.allocationSize = memReqs.size;
    memAlloc.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(_device, &memAlloc, nullptr, &_texMemory);
    vkBindImageMemory(_device, _texImage, _texMemory, 0);

    //create texture image view
    VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _texImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(_device, &viewInfo, nullptr, &_texView);

    //create staging buffer
    VkDeviceSize bufSize = width * height * 4;
    VkBufferCreateInfo bufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = bufSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(_device, &bufInfo, nullptr, &_stagingBuffer);

    VkMemoryRequirements bufReqs;
    vkGetBufferMemoryRequirements(_device, _stagingBuffer, &bufReqs);
    VkMemoryAllocateInfo bufAlloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bufAlloc.allocationSize = bufReqs.size;
    bufAlloc.memoryTypeIndex = findMemoryType(bufReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(_device, &bufAlloc, nullptr, &_stagingMemory);
    vkBindBufferMemory(_device, _stagingBuffer, _stagingMemory, 0);
    _stagingSize = bufSize;

    //persistently map the staging buffer — this is the zero-copy acquire target
    _mappedStaging = nullptr;
    vkMapMemory(_device, _stagingMemory, 0, bufSize, 0, &_mappedStaging);

    //update all descriptor sets to point to new texture
    for(uint i = 0; i < _imageCount; i++) {
      VkDescriptorImageInfo imgDescInfo = {};
      imgDescInfo.sampler = _sampler;
      imgDescInfo.imageView = _texView;
      imgDescInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

      VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = _descriptorSets[i];
      write.dstBinding = 0;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.pImageInfo = &imgDescInfo;
      vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
    }
  }

  auto destroyTexture() -> void {
    if(_mappedStaging && _stagingMemory) {
      vkUnmapMemory(_device, _stagingMemory);
      _mappedStaging = nullptr;
    }
    if(_texView) vkDestroyImageView(_device, _texView, nullptr); _texView = VK_NULL_HANDLE;
    if(_texImage) vkDestroyImage(_device, _texImage, nullptr); _texImage = VK_NULL_HANDLE;
    if(_texMemory) vkFreeMemory(_device, _texMemory, nullptr); _texMemory = VK_NULL_HANDLE;
    if(_stagingBuffer) vkDestroyBuffer(_device, _stagingBuffer, nullptr); _stagingBuffer = VK_NULL_HANDLE;
    if(_stagingMemory) vkFreeMemory(_device, _stagingMemory, nullptr); _stagingMemory = VK_NULL_HANDLE;
    _stagingSize = 0;
    _texWidth = _texHeight = 0;
  }

  auto recordCommandBuffer(uint32_t imageIndex, uint outputWidth, uint outputHeight) -> void {
    auto& cmd = _commandBuffers[imageIndex];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    //transition texture to transfer dst
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = _texImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    //copy staging buffer to texture
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {_texWidth, _texHeight, 1};
    vkCmdCopyBufferToImage(cmd, _stagingBuffer, _texImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    //transition texture to shader read
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    //begin render pass
    VkClearValue clearValue = {};
    VkRenderPassBeginInfo rpBegin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass = _renderPass;
    rpBegin.framebuffer = _framebuffers[imageIndex];
    rpBegin.renderArea = {{0, 0}, _swapExtent};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clearValue;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    //set dynamic viewport/scissor for output rect
    if(!outputWidth || outputWidth > _swapExtent.width) outputWidth = _swapExtent.width;
    if(!outputHeight || outputHeight > _swapExtent.height) outputHeight = _swapExtent.height;
    int32_t viewportX = (int32_t)(_swapExtent.width - outputWidth) / 2;
    int32_t viewportY = (int32_t)(_swapExtent.height - outputHeight) / 2;

    VkViewport vp = {(float)viewportX, (float)viewportY, (float)outputWidth, (float)outputHeight, 0.0f, 1.0f};
    VkRect2D sc = {{viewportX, viewportY}, {outputWidth, outputHeight}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipelineLayout, 0, 1, &_descriptorSets[imageIndex], 0, nullptr);

    //draw fullscreen triangle (3 vertices, no vertex buffer)
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
  }
};

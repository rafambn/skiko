#include <jawt_md.h>
#include <jni.h>

#include <X11/Xlib.h>

#define VK_USE_PLATFORM_XLIB_KHR
#define SK_USE_INTERNAL_VULKAN_HEADERS
#include "include/third_party/vulkan/vulkan/vulkan.h"
#include "include/third_party/vulkan/vulkan/vulkan_xlib.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

#include "jni_helpers.h"

namespace {

template <typename Handle>
jlong toJavaHandle(Handle handle) {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<jlong>(reinterpret_cast<uintptr_t>(handle));
    } else {
        return static_cast<jlong>(handle);
    }
}

template <typename Handle>
Handle fromJavaHandle(jlong handle) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(static_cast<uintptr_t>(handle));
    } else {
        return static_cast<Handle>(handle);
    }
}

bool succeeded(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    std::fprintf(stderr, "GraphiteVulkanHost: %s failed with VkResult=%d\n", operation,
                 static_cast<int>(result));
    return false;
}

bool hasInstanceExtension(const char* requestedExtension) {
    uint32_t propertyCount = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr) != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> properties(propertyCount);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, properties.data()) !=
        VK_SUCCESS) {
        return false;
    }

    for (const VkExtensionProperties& property : properties) {
        if (std::strcmp(property.extensionName, requestedExtension) == 0) return true;
    }
    return false;
}

bool hasDeviceExtension(VkPhysicalDevice physicalDevice, const char* requestedExtension) {
    uint32_t propertyCount = 0;
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &propertyCount, nullptr) !=
        VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> properties(propertyCount);
    if (vkEnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &propertyCount, properties.data()) != VK_SUCCESS) {
        return false;
    }

    for (const VkExtensionProperties& property : properties) {
        if (std::strcmp(property.extensionName, requestedExtension) == 0) return true;
    }
    return false;
}

VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> candidates = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR candidate : candidates) {
        if ((supported & candidate) != 0) return candidate;
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

class VulkanHost final {
public:
    ~VulkanHost() {
        dispose();
    }

    bool initialize(JAWT_X11DrawingSurfaceInfo* surfaceInfo) {
        if (surfaceInfo == nullptr || surfaceInfo->display == nullptr ||
            surfaceInfo->drawable == 0) {
            return false;
        }

        display_ = surfaceInfo->display;
        window_ = surfaceInfo->drawable;

        if (!hasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME) ||
            !hasInstanceExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME)) {
            std::fprintf(stderr, "GraphiteVulkanHost: Xlib Vulkan surface extension is unavailable\n");
            return false;
        }

        VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        applicationInfo.pApplicationName = "GraphiteSurface JVM Linux POC";
        applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        applicationInfo.pEngineName = "GraphiteSurface";
        applicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        applicationInfo.apiVersion = VK_API_VERSION_1_1;

        const std::array<const char*, 2> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
        };
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &applicationInfo;
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        instanceInfo.ppEnabledExtensionNames = extensions.data();
        if (!succeeded(vkCreateInstance(&instanceInfo, nullptr, &instance_), "vkCreateInstance")) {
            return false;
        }

        VkXlibSurfaceCreateInfoKHR surfaceInfoVulkan{
            VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
        surfaceInfoVulkan.dpy = display_;
        surfaceInfoVulkan.window = window_;
        if (!succeeded(
                vkCreateXlibSurfaceKHR(instance_, &surfaceInfoVulkan, nullptr, &surface_),
                "vkCreateXlibSurfaceKHR")) {
            return false;
        }

        if (!initializeDevice()) return false;

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!succeeded(
                vkCreateFence(device_, &fenceInfo, nullptr, &acquireFence_),
                "vkCreateFence(acquire)")) {
            return false;
        }
        return true;
    }

    bool resize(uint32_t width, uint32_t height) {
        if (width == 0 || height == 0 || device_ == VK_NULL_HANDLE) return false;
        if (swapchain_ != VK_NULL_HANDLE && !swapchainOutOfDate_ && width_ == width &&
            height_ == height) {
            return true;
        }

        if (!succeeded(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(resize)")) return false;
        destroySwapchain();

        VkSurfaceCapabilitiesKHR capabilities{};
        if (!succeeded(
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities),
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
            return false;
        }

        uint32_t formatCount = 0;
        if (!succeeded(
                vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr),
                "vkGetPhysicalDeviceSurfaceFormatsKHR")) {
            return false;
        }
        if (formatCount == 0) return false;
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        if (!succeeded(
                vkGetPhysicalDeviceSurfaceFormatsKHR(
                    physicalDevice_, surface_, &formatCount, formats.data()),
                "vkGetPhysicalDeviceSurfaceFormatsKHR")) {
            return false;
        }

        surfaceFormat_ = formats.front();
        for (const VkSurfaceFormatKHR& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                surfaceFormat_ = format;
                break;
            }
        }

        constexpr VkImageUsageFlags requiredImageUsage =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        if ((capabilities.supportedUsageFlags & requiredImageUsage) != requiredImageUsage) {
            std::fprintf(stderr,
                         "GraphiteVulkanHost: swapchain lacks Graphite render-attachment usage\n");
            return false;
        }
        imageUsage_ = requiredImageUsage;

        VkExtent2D extent{};
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            extent = capabilities.currentExtent;
        } else {
            extent.width = std::clamp(width, capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(height, capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) return false;

        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount != 0) {
            imageCount = std::min(imageCount, capabilities.maxImageCount);
        }

        VkSwapchainCreateInfoKHR swapchainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapchainInfo.surface = surface_;
        swapchainInfo.minImageCount = imageCount;
        swapchainInfo.imageFormat = surfaceFormat_.format;
        swapchainInfo.imageColorSpace = surfaceFormat_.colorSpace;
        swapchainInfo.imageExtent = extent;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.imageUsage = imageUsage_;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.preTransform = capabilities.currentTransform;
        swapchainInfo.compositeAlpha = chooseCompositeAlpha(capabilities.supportedCompositeAlpha);
        swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchainInfo.clipped = VK_TRUE;

        if (!succeeded(
                vkCreateSwapchainKHR(device_, &swapchainInfo, nullptr, &swapchain_),
                "vkCreateSwapchainKHR")) {
            return false;
        }

        uint32_t swapchainImageCount = 0;
        if (!succeeded(
                vkGetSwapchainImagesKHR(device_, swapchain_, &swapchainImageCount, nullptr),
                "vkGetSwapchainImagesKHR")) {
            destroySwapchain();
            return false;
        }
        swapchainImages_.resize(swapchainImageCount);
        if (!succeeded(
                vkGetSwapchainImagesKHR(
                    device_, swapchain_, &swapchainImageCount, swapchainImages_.data()),
                "vkGetSwapchainImagesKHR")) {
            destroySwapchain();
            return false;
        }

        width_ = extent.width;
        height_ = extent.height;
        imageLayouts_.assign(swapchainImages_.size(), VK_IMAGE_LAYOUT_UNDEFINED);
        swapchainOutOfDate_ = false;
        return true;
    }

    VkImage nextDrawable() {
        if (swapchain_ == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        if (drawableAcquired_) return swapchainImages_[imageIndex_];

        if (!succeeded(vkResetFences(device_, 1, &acquireFence_), "vkResetFences(acquire)")) {
            return VK_NULL_HANDLE;
        }
        const VkResult acquireResult = vkAcquireNextImageKHR(
            device_, swapchain_, std::numeric_limits<uint64_t>::max(), VK_NULL_HANDLE,
            acquireFence_, &imageIndex_);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchainOutOfDate_ = true;
            return VK_NULL_HANDLE;
        }
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
            succeeded(acquireResult, "vkAcquireNextImageKHR");
            return VK_NULL_HANDLE;
        }
        if (!succeeded(
                vkWaitForFences(device_, 1, &acquireFence_, VK_TRUE,
                                std::numeric_limits<uint64_t>::max()),
                "vkWaitForFences(acquire)")) {
            return VK_NULL_HANDLE;
        }

        drawableAcquired_ = true;
        if (acquireResult == VK_SUBOPTIMAL_KHR) swapchainOutOfDate_ = true;
        return swapchainImages_[imageIndex_];
    }

    void present() {
        if (!drawableAcquired_) return;

        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex_;
        const VkResult presentResult = vkQueuePresentKHR(queue_, &presentInfo);
        if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR &&
            presentResult != VK_ERROR_OUT_OF_DATE_KHR) {
            succeeded(presentResult, "vkQueuePresentKHR");
        }
        if (presentResult != VK_SUCCESS) swapchainOutOfDate_ = true;
        imageLayouts_[imageIndex_] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        drawableAcquired_ = false;
    }

    void dropDrawable() {
        if (!drawableAcquired_) return;
        // Recreating the swapchain is the only portable way to return an image that was acquired
        // without submitting a presentation operation.
        swapchainOutOfDate_ = true;
        drawableAcquired_ = false;
    }

    void dispose() {
        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        if (acquireFence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, acquireFence_, nullptr);
            acquireFence_ = VK_NULL_HANDLE;
        }
        destroySwapchain();
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
    }

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queueFamilyIndex() const { return queueFamilyIndex_; }
    VkFormat imageFormat() const { return surfaceFormat_.format; }
    VkImageUsageFlags imageUsage() const { return imageUsage_; }
    VkImageLayout imageLayout() const {
        return drawableAcquired_ ? imageLayouts_[imageIndex_] : VK_IMAGE_LAYOUT_UNDEFINED;
    }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    bool initializeDevice() {
        uint32_t physicalDeviceCount = 0;
        if (!succeeded(
                vkEnumeratePhysicalDevices(instance_, &physicalDeviceCount, nullptr),
                "vkEnumeratePhysicalDevices")) {
            return false;
        }
        if (physicalDeviceCount == 0) return false;

        std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
        if (!succeeded(
                vkEnumeratePhysicalDevices(
                    instance_, &physicalDeviceCount, physicalDevices.data()),
                "vkEnumeratePhysicalDevices")) {
            return false;
        }

        for (VkPhysicalDevice candidate : physicalDevices) {
            if (!hasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(
                candidate, &queueFamilyCount, queueFamilies.data());
            for (uint32_t index = 0; index < queueFamilyCount; ++index) {
                VkBool32 supportsPresent = VK_FALSE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR(
                        candidate, index, surface_, &supportsPresent) != VK_SUCCESS) {
                    continue;
                }
                if ((queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                    supportsPresent == VK_TRUE) {
                    physicalDevice_ = candidate;
                    queueFamilyIndex_ = index;
                    break;
                }
            }
            if (physicalDevice_ != VK_NULL_HANDLE) break;
        }
        if (physicalDevice_ == VK_NULL_HANDLE) {
            std::fprintf(stderr, "GraphiteVulkanHost: no Vulkan graphics/present queue found\n");
            return false;
        }

        const float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamilyIndex_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        const std::array<const char*, 1> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        deviceInfo.ppEnabledExtensionNames = extensions.data();
        deviceInfo.pEnabledFeatures = &features;
        if (!succeeded(vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_),
                       "vkCreateDevice")) {
            return false;
        }
        vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &queue_);
        return queue_ != VK_NULL_HANDLE;
    }

    void destroySwapchain() {
        if (swapchain_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        }
        swapchain_ = VK_NULL_HANDLE;
        swapchainImages_.clear();
        imageLayouts_.clear();
        drawableAcquired_ = false;
        imageIndex_ = 0;
        width_ = 0;
        height_ = 0;
    }

    Display* display_ = nullptr;
    Window window_ = 0;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surfaceFormat_{};
    VkImageUsageFlags imageUsage_ = 0;
    VkFence acquireFence_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageLayout> imageLayouts_;
    uint32_t imageIndex_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool drawableAcquired_ = false;
    bool swapchainOutOfDate_ = false;
};

VulkanHost* fromHandle(jlong handle) {
    return reinterpret_cast<VulkanHost*>(static_cast<uintptr_t>(handle));
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeCreate(
    JNIEnv*, jobject, jlong platformInfo) {
    auto* host = new VulkanHost();
    auto* surfaceInfo = fromJavaPointer<JAWT_X11DrawingSurfaceInfo*>(platformInfo);
    if (!host->initialize(surfaceInfo)) {
        delete host;
        return 0;
    }
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(host));
}

JNIEXPORT jboolean JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeResize(
    JNIEnv*, jobject, jlong handle, jint width, jint height) {
    auto* host = fromHandle(handle);
    return host != nullptr && host->resize(
        static_cast<uint32_t>(std::max(width, 0)), static_cast<uint32_t>(std::max(height, 0)));
}

JNIEXPORT jlong JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeNextDrawable(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : toJavaHandle(host->nextDrawable());
}

JNIEXPORT void JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativePresent(
    JNIEnv*, jobject, jlong handle) {
    if (auto* host = fromHandle(handle)) host->present();
}

JNIEXPORT void JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeDropDrawable(
    JNIEnv*, jobject, jlong handle) {
    if (auto* host = fromHandle(handle)) host->dropDrawable();
}

JNIEXPORT jlong JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeInstance(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : toJavaHandle(host->instance());
}

JNIEXPORT jlong JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativePhysicalDevice(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : toJavaHandle(host->physicalDevice());
}

JNIEXPORT jlong JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeDevice(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : toJavaHandle(host->device());
}

JNIEXPORT jlong JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeQueue(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : toJavaHandle(host->queue());
}

JNIEXPORT jint JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeQueueFamilyIndex(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : static_cast<jint>(host->queueFamilyIndex());
}

JNIEXPORT jint JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeImageFormat(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : static_cast<jint>(host->imageFormat());
}

JNIEXPORT jint JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeImageUsage(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : static_cast<jint>(host->imageUsage());
}

JNIEXPORT jint JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeImageLayout(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : static_cast<jint>(host->imageLayout());
}

JNIEXPORT jint JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeWidth(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : static_cast<jint>(host->width());
}

JNIEXPORT jint JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeHeight(
    JNIEnv*, jobject, jlong handle) {
    auto* host = fromHandle(handle);
    return host == nullptr ? 0 : static_cast<jint>(host->height());
}

JNIEXPORT void JNICALL
Java_org_jetbrains_skiko_graphite_GraphiteVulkanHost_nativeDispose(
    JNIEnv*, jobject, jlong handle) {
    delete fromHandle(handle);
}

}

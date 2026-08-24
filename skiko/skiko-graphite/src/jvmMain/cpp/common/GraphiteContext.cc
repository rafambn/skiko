#include <jni.h>

#if defined(SK_BUILD_FOR_LINUX)
#define VK_USE_PLATFORM_XLIB_KHR
#define SK_USE_INTERNAL_VULKAN_HEADERS
#include <X11/Xlib.h>
#include "include/third_party/vulkan/vulkan/vulkan_core.h"
#include "include/third_party/vulkan/vulkan/vulkan_xlib.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "include/gpu/graphite/vk/VulkanGraphiteContext.h"

// The release Skia bundle exposes the allocator factory from libskia but does
// not ship its private declaration header. Keep the declaration local so this
// bridge can use the bundled allocator without depending on Skia source files.
namespace skgpu {
enum class ThreadSafe : bool { kNo, kYes };

namespace VulkanMemoryAllocators {
SK_API sk_sp<VulkanMemoryAllocator> Make(const VulkanBackendContext&, ThreadSafe);
}
}
#endif

#include "GraphiteImageProvider.hh"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/Recorder.h"
#if defined(SK_METAL)
#include "include/gpu/graphite/mtl/MtlBackendContext.h"
#endif

static void deleteGraphiteContext(skgpu::graphite::Context* context) {
    delete context;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_GraphiteContextKt__1nGetGraphiteContextFinalizer(JNIEnv*, jclass) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(&deleteGraphiteContext));
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_GraphiteContextKt__1nMakeMetal(
        JNIEnv*, jclass, jlong devicePtr, jlong queuePtr) {
#if defined(SK_METAL)
    skgpu::graphite::MtlBackendContext backendContext{};
    backendContext.fDevice.retain(
            reinterpret_cast<CFTypeRef>(static_cast<uintptr_t>(devicePtr)));
    backendContext.fQueue.retain(
            reinterpret_cast<CFTypeRef>(static_cast<uintptr_t>(queuePtr)));

    skgpu::graphite::ContextOptions options{};
    options.fRequireOrderedRecordings = true;
    return reinterpret_cast<jlong>(
            skgpu::graphite::ContextFactory::MakeMetal(backendContext, options).release());
#else
    return 0;
#endif
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_GraphiteContextKt__1nMakeVulkan(
        JNIEnv*, jclass, jlong instancePtr, jlong physicalDevicePtr, jlong devicePtr,
        jlong queuePtr, jint queueFamilyIndex) {
#if defined(SK_BUILD_FOR_LINUX)
    skgpu::VulkanBackendContext backendContext{};
    backendContext.fInstance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(instancePtr));
    backendContext.fPhysicalDevice =
            reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physicalDevicePtr));
    backendContext.fDevice = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(devicePtr));
    backendContext.fQueue = reinterpret_cast<VkQueue>(static_cast<uintptr_t>(queuePtr));
    backendContext.fGraphicsQueueIndex = static_cast<uint32_t>(queueFamilyIndex);
    backendContext.fMaxAPIVersion = VK_API_VERSION_1_1;
    backendContext.fGetProc = [](const char* name, VkInstance instance, VkDevice device) {
        if (device != VK_NULL_HANDLE) return vkGetDeviceProcAddr(device, name);
        return vkGetInstanceProcAddr(instance, name);
    };

    const char* instanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
    };
    const char* deviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    skgpu::VulkanExtensions extensions;
    extensions.init(
            backendContext.fGetProc,
            backendContext.fInstance,
            backendContext.fPhysicalDevice,
            2,
            instanceExtensions,
            1,
            deviceExtensions);
    backendContext.fVkExtensions = &extensions;
    backendContext.fMemoryAllocator = skgpu::VulkanMemoryAllocators::Make(
            backendContext,
            skgpu::ThreadSafe::kYes);
    if (!backendContext.fMemoryAllocator) return 0;

    skgpu::graphite::ContextOptions options{};
    options.fRequireOrderedRecordings = true;
    return reinterpret_cast<jlong>(
            skgpu::graphite::ContextFactory::MakeVulkan(backendContext, options).release());
#else
    return 0;
#endif
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_GraphiteContextKt__1nMakeRecorder(
        JNIEnv*, jclass, jlong contextPtr) {
    auto context = reinterpret_cast<skgpu::graphite::Context*>(
            static_cast<uintptr_t>(contextPtr));
    skgpu::graphite::RecorderOptions options{};
    options.fImageProvider = SkikoGraphiteImageProvider::Make();
    return reinterpret_cast<jlong>(context->makeRecorder(options).release());
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_GraphiteContextKt__1nInsertRecording(
        JNIEnv*, jclass, jlong contextPtr, jlong recordingPtr) {
    auto context = reinterpret_cast<skgpu::graphite::Context*>(
            static_cast<uintptr_t>(contextPtr));
    skgpu::graphite::InsertRecordingInfo info{};
    info.fRecording = reinterpret_cast<skgpu::graphite::Recording*>(
            static_cast<uintptr_t>(recordingPtr));
    context->insertRecording(info);
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_GraphiteContextKt__1nSubmit(
        JNIEnv*, jclass, jlong contextPtr, jboolean syncCpu) {
    auto context = reinterpret_cast<skgpu::graphite::Context*>(
            static_cast<uintptr_t>(contextPtr));
    context->submit(skgpu::graphite::SubmitInfo(
            syncCpu ? skgpu::graphite::SyncToCpu::kYes : skgpu::graphite::SyncToCpu::kNo));
}

extern "C" JNIEXPORT void JNICALL
Java_org_jetbrains_skia_gpu_graphite_GraphiteContextKt__1nCheckAsyncWorkCompletion(
        JNIEnv*, jclass, jlong contextPtr) {
    auto context = reinterpret_cast<skgpu::graphite::Context*>(
            static_cast<uintptr_t>(contextPtr));
    context->checkAsyncWorkCompletion();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_jetbrains_skia_gpu_graphite_GraphiteContextKt__1nHasUnfinishedGpuWork(
        JNIEnv*, jclass, jlong contextPtr) {
    auto context = reinterpret_cast<skgpu::graphite::Context*>(
            static_cast<uintptr_t>(contextPtr));
    return context->hasUnfinishedGpuWork();
}

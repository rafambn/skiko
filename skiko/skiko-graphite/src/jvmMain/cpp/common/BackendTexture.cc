#include <jni.h>

#if defined(SK_BUILD_FOR_LINUX)
#define SK_USE_INTERNAL_VULKAN_HEADERS
#include "include/third_party/vulkan/vulkan/vulkan_core.h"
#include "include/gpu/graphite/vk/VulkanGraphiteTypes.h"
#endif

#include "include/gpu/graphite/BackendTexture.h"
#if defined(SK_METAL)
#include "include/gpu/graphite/mtl/MtlGraphiteTypes_cpp.h"
#endif

static void deleteBackendTexture(skgpu::graphite::BackendTexture* texture) {
    delete texture;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_BackendTextureKt__1nGetBackendTextureFinalizer(JNIEnv*, jclass) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(&deleteBackendTexture));
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_BackendTextureKt__1nMakeMetal(
        JNIEnv*, jclass, jint width, jint height, jlong texturePtr) {
#if defined(SK_METAL)
    auto texture = skgpu::graphite::BackendTextures::MakeMetal(
            SkISize::Make(width, height),
            reinterpret_cast<CFTypeRef>(static_cast<uintptr_t>(texturePtr)));
    return reinterpret_cast<jlong>(new skgpu::graphite::BackendTexture(texture));
#else
    return 0;
#endif
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_BackendTextureKt__1nMakeVulkan(
        JNIEnv*, jclass, jint width, jint height, jint format, jint imageUsage,
        jint imageLayout, jint queueFamilyIndex, jlong imagePtr) {
#if defined(SK_BUILD_FOR_LINUX)
    skgpu::graphite::VulkanTextureInfo textureInfo(
            VK_SAMPLE_COUNT_1_BIT,
            skgpu::Mipmapped::kNo,
            0,
            static_cast<VkFormat>(format),
            VK_IMAGE_TILING_OPTIMAL,
            static_cast<VkImageUsageFlags>(imageUsage),
            VK_SHARING_MODE_EXCLUSIVE,
            VK_IMAGE_ASPECT_COLOR_BIT,
            {});
    const auto texture = skgpu::graphite::BackendTextures::MakeVulkan(
            {width, height},
            textureInfo,
            static_cast<VkImageLayout>(imageLayout),
            static_cast<uint32_t>(queueFamilyIndex),
            reinterpret_cast<VkImage>(static_cast<uintptr_t>(imagePtr)),
            {});
    if (!texture.isValid()) return 0;
    return reinterpret_cast<jlong>(new skgpu::graphite::BackendTexture(texture));
#else
    return 0;
#endif
}

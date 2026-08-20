#include "common.h"

#include <cstdint>

#include "include/gpu/graphite/BackendTexture.h"
#if defined(__EMSCRIPTEN__)
#include <emscripten/html5_webgpu.h>
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#else
#include "include/gpu/graphite/mtl/MtlGraphiteTypes_cpp.h"
#endif

static void deleteBackendTexture(skgpu::graphite::BackendTexture* texture) {
    delete texture;
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_BackendTexture__1nGetFinalizer() {
    return reinterpret_cast<KNativePointer>(&deleteBackendTexture);
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_BackendTexture__1nMakeMetal(
        KInt width, KInt height, KNativePointer texturePtr) {
#if defined(__EMSCRIPTEN__)
    return 0;
#else
    auto texture = skgpu::graphite::BackendTextures::MakeMetal(
            SkISize::Make(width, height),
            reinterpret_cast<CFTypeRef>(texturePtr));
    return reinterpret_cast<KNativePointer>(new skgpu::graphite::BackendTexture(texture));
#endif
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_BackendTexture__1nMakeDawn(
        KNativePointer textureHandle) {
#if defined(__EMSCRIPTEN__)
    const auto handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(textureHandle));
    auto texture = emscripten_webgpu_import_texture(handle);
    if (texture == nullptr) {
        emscripten_webgpu_release_js_handle(handle);
        return 0;
    }

    auto backendTexture = skgpu::graphite::BackendTextures::MakeDawn(texture);
    emscripten_webgpu_release_js_handle(handle);
    return reinterpret_cast<KNativePointer>(
            new skgpu::graphite::BackendTexture(backendTexture));
#else
    return 0;
#endif
}

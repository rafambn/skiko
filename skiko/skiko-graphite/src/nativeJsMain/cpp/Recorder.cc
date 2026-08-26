#include "common.h"

#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/TextureInfo.h"

static void deleteRecorder(skgpu::graphite::Recorder* recorder) {
    delete recorder;
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_Recorder__1nGetFinalizer() {
    return reinterpret_cast<KNativePointer>(&deleteRecorder);
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_Recorder__1nMakeDeferredCanvas(
        KNativePointer recorderPtr,
        KInt width,
        KInt height,
        KInt colorType,
        KInt alphaType,
        KNativePointer colorSpacePtr,
        KNativePointer textureInfoPtr) {
    auto recorder = reinterpret_cast<skgpu::graphite::Recorder*>(recorderPtr);
    auto colorSpace = sk_ref_sp(reinterpret_cast<SkColorSpace*>(colorSpacePtr));
    auto textureInfo = reinterpret_cast<skgpu::graphite::TextureInfo*>(textureInfoPtr);
    if (!recorder || !textureInfo) return 0;
    const auto imageInfo = SkImageInfo::Make(
            width,
            height,
            static_cast<SkColorType>(colorType),
            static_cast<SkAlphaType>(alphaType),
            std::move(colorSpace));
    return reinterpret_cast<KNativePointer>(
            recorder->makeDeferredCanvas(imageInfo, *textureInfo));
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_Recorder__1nSnap(
        KNativePointer recorderPtr) {
    auto recorder = reinterpret_cast<skgpu::graphite::Recorder*>(recorderPtr);
    return reinterpret_cast<KNativePointer>(recorder->snap().release());
}

#include <jni.h>

#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/TextureInfo.h"

static void deleteRecorder(skgpu::graphite::Recorder* recorder) {
    delete recorder;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_RecorderKt__1nGetRecorderFinalizer(JNIEnv*, jclass) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(&deleteRecorder));
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_RecorderKt__1nMakeDeferredCanvas(
        JNIEnv*,
        jclass,
        jlong recorderPtr,
        jint width,
        jint height,
        jint colorType,
        jint alphaType,
        jlong colorSpacePtr,
        jlong textureInfoPtr) {
    auto recorder = reinterpret_cast<skgpu::graphite::Recorder*>(
            static_cast<uintptr_t>(recorderPtr));
    auto colorSpace = sk_ref_sp(reinterpret_cast<SkColorSpace*>(
            static_cast<uintptr_t>(colorSpacePtr)));
    auto textureInfo = reinterpret_cast<skgpu::graphite::TextureInfo*>(
            static_cast<uintptr_t>(textureInfoPtr));
    if (!recorder || !textureInfo) return 0;
    const auto imageInfo = SkImageInfo::Make(
            width,
            height,
            static_cast<SkColorType>(colorType),
            static_cast<SkAlphaType>(alphaType),
            std::move(colorSpace));
    return reinterpret_cast<jlong>(recorder->makeDeferredCanvas(imageInfo, *textureInfo));
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_RecorderKt__1nSnap(
        JNIEnv*, jclass, jlong recorderPtr) {
    auto recorder = reinterpret_cast<skgpu::graphite::Recorder*>(
            static_cast<uintptr_t>(recorderPtr));
    return reinterpret_cast<jlong>(recorder->snap().release());
}

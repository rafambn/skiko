#include <jni.h>

#include "include/gpu/graphite/TextureInfo.h"

static void deleteTextureInfo(skgpu::graphite::TextureInfo* info) {
    delete info;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_jetbrains_skia_gpu_graphite_TextureInfoKt__1nGetTextureInfoFinalizer(JNIEnv*, jclass) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(&deleteTextureInfo));
}

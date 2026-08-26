#include "common.h"

#include "include/gpu/graphite/TextureInfo.h"

static void deleteTextureInfo(skgpu::graphite::TextureInfo* info) {
    delete info;
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_TextureInfo__1nGetFinalizer() {
    return reinterpret_cast<KNativePointer>(&deleteTextureInfo);
}

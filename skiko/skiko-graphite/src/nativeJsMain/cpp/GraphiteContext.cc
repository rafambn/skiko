#include <vector>

#include "common.h"
#include "GraphiteImageProvider.hh"

#include "include/gpu/graphite/BackendSemaphore.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/Recorder.h"
#if defined(__EMSCRIPTEN__)
#include <emscripten/html5_webgpu.h>
#include <webgpu/webgpu_cpp.h>
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#else
#include "include/gpu/graphite/mtl/MtlBackendContext.h"
#endif

static void deleteGraphiteContext(skgpu::graphite::Context* context) {
    delete context;
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_GraphiteContext__1nGetFinalizer() {
    return reinterpret_cast<KNativePointer>(&deleteGraphiteContext);
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_GraphiteContext__1nMakeMetal(
        KNativePointer devicePtr, KNativePointer queuePtr) {
#if defined(__EMSCRIPTEN__)
    return 0;
#else
    skgpu::graphite::MtlBackendContext backendContext{};
    backendContext.fDevice.retain(reinterpret_cast<CFTypeRef>(devicePtr));
    backendContext.fQueue.retain(reinterpret_cast<CFTypeRef>(queuePtr));

    skgpu::graphite::ContextOptions options{};
    options.fRequireOrderedRecordings = true;
    return reinterpret_cast<KNativePointer>(
            skgpu::graphite::ContextFactory::MakeMetal(backendContext, options).release());
#endif
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_GraphiteContext__1nMakeDawn() {
#if defined(__EMSCRIPTEN__)
    const auto rawDevice = emscripten_webgpu_get_device();
    if (rawDevice == nullptr) {
        return 0;
    }

    skgpu::graphite::DawnBackendContext backendContext{};
    backendContext.fDevice = wgpu::Device::Acquire(rawDevice);
    backendContext.fQueue = backendContext.fDevice.GetQueue();

    skgpu::graphite::ContextOptions options{};
    options.fRequireOrderedRecordings = true;
    return reinterpret_cast<KNativePointer>(
            skgpu::graphite::ContextFactory::MakeDawn(backendContext, options).release());
#else
    return 0;
#endif
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_GraphiteContext__1nMakeVulkan(
        KNativePointer,
        KNativePointer,
        KNativePointer,
        KNativePointer,
        int,
        int) {
    return 0;
}

SKIKO_EXPORT KNativePointer org_jetbrains_skia_gpu_graphite_GraphiteContext__1nMakeRecorder(
        KNativePointer contextPtr) {
    auto context = reinterpret_cast<skgpu::graphite::Context*>(contextPtr);
    skgpu::graphite::RecorderOptions options{};
    options.fImageProvider = SkikoGraphiteImageProvider::Make();
    return reinterpret_cast<KNativePointer>(context->makeRecorder(options).release());
}

SKIKO_EXPORT void org_jetbrains_skia_gpu_graphite_GraphiteContext__1nInsertRecording(
        KNativePointer contextPtr,
        KNativePointer recordingPtr,
        KInteropPointer waitSemaphoresPtrs,
        KInt waitSemaphoresCount,
        KInteropPointer signalSemaphoresPtrs,
        KInt signalSemaphoresCount) {
    auto context = reinterpret_cast<skgpu::graphite::Context*>(contextPtr);
    if (!context) return;

    skgpu::graphite::InsertRecordingInfo info{};
    info.fRecording = reinterpret_cast<skgpu::graphite::Recording*>(recordingPtr);

    std::vector<skgpu::graphite::BackendSemaphore> waitSemaphores;
    if (waitSemaphoresPtrs && waitSemaphoresCount > 0) {
        waitSemaphores.reserve(waitSemaphoresCount);
        auto ptrs = reinterpret_cast<KNativePointer*>(waitSemaphoresPtrs);
        for (KInt i = 0; i < waitSemaphoresCount; ++i) {
            auto sem = reinterpret_cast<skgpu::graphite::BackendSemaphore*>(ptrs[i]);
            waitSemaphores.push_back(*sem);
        }
    }
    info.fNumWaitSemaphores = waitSemaphores.size();
    info.fWaitSemaphores = waitSemaphores.data();

    std::vector<skgpu::graphite::BackendSemaphore> signalSemaphores;
    if (signalSemaphoresPtrs && signalSemaphoresCount > 0) {
        signalSemaphores.reserve(signalSemaphoresCount);
        auto ptrs = reinterpret_cast<KNativePointer*>(signalSemaphoresPtrs);
        for (KInt i = 0; i < signalSemaphoresCount; ++i) {
            auto sem = reinterpret_cast<skgpu::graphite::BackendSemaphore*>(ptrs[i]);
            signalSemaphores.push_back(*sem);
        }
    }
    info.fNumSignalSemaphores = signalSemaphores.size();
    info.fSignalSemaphores = signalSemaphores.data();

    context->insertRecording(info);
}

SKIKO_EXPORT void org_jetbrains_skia_gpu_graphite_GraphiteContext__1nSubmit(
        KNativePointer contextPtr, KBoolean syncCpu) {
    auto context = reinterpret_cast<skgpu::graphite::Context*>(contextPtr);
    context->submit(skgpu::graphite::SubmitInfo(
            syncCpu ? skgpu::graphite::SyncToCpu::kYes : skgpu::graphite::SyncToCpu::kNo));
}

SKIKO_EXPORT void org_jetbrains_skia_gpu_graphite_GraphiteContext__1nCheckAsyncWorkCompletion(
        KNativePointer contextPtr) {
    auto context = reinterpret_cast<skgpu::graphite::Context*>(contextPtr);
    context->checkAsyncWorkCompletion();
}

SKIKO_EXPORT KBoolean org_jetbrains_skia_gpu_graphite_GraphiteContext__1nHasUnfinishedGpuWork(
        KNativePointer contextPtr) {
    auto context = reinterpret_cast<skgpu::graphite::Context*>(contextPtr);
    return context->hasUnfinishedGpuWork();
}

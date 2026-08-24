#include "common.h"

#if defined(__EMSCRIPTEN__) && defined(GRAPHITE_PTHREADS_EXPERIMENT)

#include <atomic>
#include <cstdint>
#include <memory>
#include <pthread.h>

#include <emscripten.h>
#include <emscripten/html5_webgpu.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"

extern "C" void graphite_pthread_request_device(std::uintptr_t callback);
extern "C" std::uint32_t graphite_pthread_current_texture_handle();

namespace {

constexpr int kWidth = 960;
constexpr int kHeight = 540;
constexpr int kRecorderCount = 2;

enum Status : int {
    kIdle = 0,
    kStartingRenderThread = 1,
    kRequestingDevice = 2,
    kRecording = 3,
    kPresented = 4,
    kThreadCreationFailed = -1,
    kDeviceInitializationFailed = -2,
    kContextCreationFailed = -3,
    kPresentationTargetFailed = -4,
    kRecorderThreadFailed = -5,
    kRecorderWorkFailed = -6,
};

struct RecorderJob {
    int index = 0;
    std::unique_ptr<skgpu::graphite::Recorder> recorder;
    std::unique_ptr<skgpu::graphite::Recording> recording;
    skgpu::graphite::TextureInfo textureInfo;
    double startedAt = 0.0;
    double finishedAt = 0.0;
};

std::atomic<int> gStatus{kIdle};
std::atomic<int> gDeviceError{0};
std::atomic<int> gCompletedRecorders{0};
pthread_t gRenderThread{};
pthread_t gRecorderThreads[kRecorderCount]{};
em_proxying_queue* gQueue = nullptr;
std::unique_ptr<skgpu::graphite::Context> gContext;
std::unique_ptr<skgpu::graphite::Recorder> gPresentationRecorder;
sk_sp<SkSurface> gPresentationSurface;
RecorderJob gJobs[kRecorderCount];

void fail(Status status) {
    gStatus.store(status, std::memory_order_release);
}

void recorder_work(void* rawJob) {
    auto* job = static_cast<RecorderJob*>(rawJob);
    job->startedAt = emscripten_get_now();

    const auto imageInfo = SkImageInfo::Make(
            kWidth,
            kHeight,
            kRGBA_8888_SkColorType,
            kPremul_SkAlphaType,
            SkColorSpace::MakeSRGB());
    SkCanvas* canvas = job->recorder->makeDeferredCanvas(imageInfo, job->textureInfo);
    if (!canvas) {
        return;
    }

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(job->index == 0 ? SK_ColorBLUE : SK_ColorGREEN);

    // Enough CPU recording work to make temporal overlap observable without
    // turning the probe into a GPU throughput benchmark.
    for (int i = 0; i < 20000; ++i) {
        const float x = static_cast<float>((i * 17 + job->index * 41) % kWidth);
        const float y = static_cast<float>((i * 29 + job->index * 13) % kHeight);
        canvas->drawCircle(x, y, 2.0f, paint);
    }

    job->recording = job->recorder->snap();
    job->finishedAt = emscripten_get_now();
}

void recorder_cancelled(void*) {
    fail(kRecorderThreadFailed);
}

void present_when_complete(void*) {
    if (gStatus.load(std::memory_order_acquire) < 0) {
        return;
    }

    const int completed = gCompletedRecorders.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed != kRecorderCount) {
        return;
    }

    for (const auto& job : gJobs) {
        if (!job.recording) {
            fail(kRecorderWorkFailed);
            return;
        }
    }

    gPresentationSurface->getCanvas()->clear(SK_ColorBLACK);
    auto targetRecording = gPresentationRecorder->snap();
    if (!targetRecording || !gContext->insertRecording({targetRecording.get()})) {
        fail(kRecorderWorkFailed);
        return;
    }

    // Submission order is deliberately stable even though completion order is
    // not. Both recordings overlap, making reversal visible in the result.
    for (const auto& job : gJobs) {
        if (!gContext->insertRecording(
                    {job.recording.get(), gPresentationSurface.get()})) {
            fail(kRecorderWorkFailed);
            return;
        }
    }

    gContext->submit(skgpu::graphite::SubmitInfo(skgpu::graphite::SyncToCpu::kNo));
    gStatus.store(kPresented, std::memory_order_release);
}

void* recorder_thread_main(void*) {
    emscripten_exit_with_live_runtime();
}

void begin_recorder_work(const skgpu::graphite::TextureInfo& textureInfo) {
    gQueue = em_proxying_queue_create();
    if (!gQueue) {
        fail(kRecorderThreadFailed);
        return;
    }

    for (int index = 0; index < kRecorderCount; ++index) {
        gJobs[index].index = index;
        gJobs[index].textureInfo = textureInfo;
        gJobs[index].recorder = gContext->makeRecorder();
        if (!gJobs[index].recorder) {
            fail(kRecorderThreadFailed);
            return;
        }

        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        emscripten_pthread_attr_settransferredcanvases(&attributes, "");
        const int result = pthread_create(
                &gRecorderThreads[index],
                &attributes,
                recorder_thread_main,
                nullptr);
        pthread_attr_destroy(&attributes);
        if (result != 0) {
            fail(kRecorderThreadFailed);
            return;
        }
    }

    gStatus.store(kRecording, std::memory_order_release);
    for (int index = 0; index < kRecorderCount; ++index) {
        if (!emscripten_proxy_callback(
                    gQueue,
                    gRecorderThreads[index],
                    recorder_work,
                    present_when_complete,
                    recorder_cancelled,
                    &gJobs[index])) {
            fail(kRecorderThreadFailed);
            return;
        }
    }
}

void on_device_ready(int errorCode) {
    gDeviceError.store(errorCode, std::memory_order_release);
    if (errorCode != 0) {
        fail(kDeviceInitializationFailed);
        return;
    }

    gDeviceError.store(20, std::memory_order_release);
    WGPUDevice rawDevice = emscripten_webgpu_get_device();
    if (!rawDevice) {
        gDeviceError.store(10, std::memory_order_release);
        fail(kDeviceInitializationFailed);
        return;
    }

    skgpu::graphite::DawnBackendContext backendContext{};
    gDeviceError.store(21, std::memory_order_release);
    backendContext.fDevice = wgpu::Device::Acquire(rawDevice);
    gDeviceError.store(22, std::memory_order_release);
    backendContext.fQueue = backendContext.fDevice.GetQueue();
    gDeviceError.store(23, std::memory_order_release);
    gContext = skgpu::graphite::ContextFactory::MakeDawn(backendContext, {});
    gDeviceError.store(24, std::memory_order_release);
    if (!gContext) {
        fail(kContextCreationFailed);
        return;
    }

    gPresentationRecorder = gContext->makeRecorder();
    const std::uint32_t textureHandle = graphite_pthread_current_texture_handle();
    WGPUTexture rawTexture = emscripten_webgpu_import_texture(textureHandle);
    if (!gPresentationRecorder || !textureHandle || !rawTexture) {
        if (textureHandle) {
            emscripten_webgpu_release_js_handle(textureHandle);
        }
        fail(kPresentationTargetFailed);
        return;
    }

    auto backendTexture = skgpu::graphite::BackendTextures::MakeDawn(rawTexture);
    emscripten_webgpu_release_js_handle(textureHandle);
    gPresentationSurface = SkSurfaces::WrapBackendTexture(
            gPresentationRecorder.get(),
            backendTexture,
            SkColorSpace::MakeSRGB(),
            nullptr);
    if (!gPresentationSurface) {
        fail(kPresentationTargetFailed);
        return;
    }

    begin_recorder_work(backendTexture.info());
}

void* render_thread_main(void*) {
    gStatus.store(kRequestingDevice, std::memory_order_release);
    graphite_pthread_request_device(reinterpret_cast<std::uintptr_t>(&on_device_ready));
    emscripten_exit_with_live_runtime();
}

}  // namespace

SKIKO_EXPORT KInt graphite_pthread_experiment_start() {
    int expected = kIdle;
    if (!gStatus.compare_exchange_strong(expected, kStartingRenderThread)) {
        return expected;
    }

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    emscripten_pthread_attr_settransferredcanvases(
            &attributes,
            "#graphite-canvas");
    const int result = pthread_create(
            &gRenderThread,
            &attributes,
            render_thread_main,
            nullptr);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
        fail(kThreadCreationFailed);
    }
    return result;
}

SKIKO_EXPORT KInt graphite_pthread_experiment_status() {
    return gStatus.load(std::memory_order_acquire);
}

SKIKO_EXPORT KDouble graphite_pthread_experiment_recorder_started(KInt index) {
    return index >= 0 && index < kRecorderCount ? gJobs[index].startedAt : 0.0;
}

SKIKO_EXPORT KDouble graphite_pthread_experiment_recorder_finished(KInt index) {
    return index >= 0 && index < kRecorderCount ? gJobs[index].finishedAt : 0.0;
}

SKIKO_EXPORT KInt graphite_pthread_experiment_device_error() {
    return gDeviceError.load(std::memory_order_acquire);
}

#endif
